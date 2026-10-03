#include "pch.h"
#include "services/mcp_server.h"
#include "build_config.h"
#include "dumper/catalog/class_catalog.h"
#include "dumper/catalog/field_catalog.h"
#include "dumper/catalog/method_catalog.h"
#include "engine/runtime_invoke.h"
#include "services/bootstrap_log.h"
#include "types/memory_guard.h"
#include "unity_resolver.h"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#include <string>
#include <thread>

using json = nlohmann::json;

namespace Engine {
namespace Services {
namespace {

HANDLE g_hPipe = INVALID_HANDLE_VALUE;
std::thread g_serverThread;
bool g_running = false;

void ProcessMcpMessage(const json &request, json &response) {
  std::string method = request.value("method", "");
  if (method == "ping") {
    response["result"] = "pong";
  } else if (method == "find_image") {
    std::string name = request["params"].value("name", "");
    void *image = Unity.FindImage(name.c_str());
    response["result"] = {{"image_ptr", reinterpret_cast<uintptr_t>(image)}};
  } else if (method == "get_method_address") {
    uintptr_t img = request["params"].value("image_ptr", 0ULL);
    std::string cls = request["params"].value("class", "");
    std::string mthd = request["params"].value("method", "");
    std::string ns = request["params"].value("namespace", "");
    uintptr_t addr =
        Unity.GetMethodAddress(reinterpret_cast<void *>(img), cls.c_str(),
                               mthd.c_str(), -1, ns.c_str());
    response["result"] = {{"address", addr}};
  } else if (method == "get_field_offset") {
    uintptr_t img = request["params"].value("image_ptr", 0ULL);
    std::string cls = request["params"].value("class", "");
    std::string fld = request["params"].value("field", "");
    std::string ns = request["params"].value("namespace", "");
    uintptr_t offset = Unity.GetFieldOffset(
        reinterpret_cast<void *>(img), cls.c_str(), fld.c_str(), ns.c_str());
    response["result"] = {{"offset", offset}};
  } else if (method == "read_i32") {
    uintptr_t addr = request["params"].value("address", 0ULL);
    int32_t val = 0;
    if (Engine::Memory::TryReadValue(addr, val)) {
      response["result"] = {{"value", val}};
    } else {
      response["error"] = "Failed to read i32 at address";
    }
  } else if (method == "write_i32") {
    uintptr_t addr = request["params"].value("address", 0ULL);
    int32_t val = request["params"].value("value", 0);
    if (Engine::Memory::TryWriteValue(addr, val)) {
      response["result"] = {{"success", true}};
    } else {
      response["error"] = "Failed to write i32 at address";
    }
  } else if (method == "read_f32") {
    uintptr_t addr = request["params"].value("address", 0ULL);
    float val = 0.0f;
    if (Engine::Memory::TryReadValue(addr, val)) {
      response["result"] = {{"value", val}};
    } else {
      response["error"] = "Failed to read f32 at address";
    }
  } else if (method == "write_f32") {
    uintptr_t addr = request["params"].value("address", 0ULL);
    float val = request["params"].value("value", 0.0f);
    if (Engine::Memory::TryWriteValue(addr, val)) {
      response["result"] = {{"success", true}};
    } else {
      response["error"] = "Failed to write f32 at address";
    }
  } else if (method == "read_pointer") {
    uintptr_t addr = request["params"].value("address", 0ULL);
    uintptr_t val = 0;
    if (Engine::Memory::TryReadValue(addr, val)) {
      response["result"] = {{"value", val}};
    } else {
      response["error"] = "Failed to read pointer at address";
    }
  } else if (method == "read_string") {
    uintptr_t addr = request["params"].value("address", 0ULL);
    int32_t length = 0;
    if (!Engine::Memory::TryReadValue(addr + 0x10, length) || length < 0 ||
        length > 1000000) {
      response["error"] = "Invalid string length or unreadable object";
    } else if (length == 0) {
      response["result"] = {{"value", ""}};
    } else {
      std::vector<wchar_t> buf(length);
      if (Engine::Memory::TryReadBytes(addr + 0x14, buf.data(),
                                       length * sizeof(wchar_t))) {
        std::string utf8;
        int utf8_len = WideCharToMultiByte(CP_UTF8, 0, buf.data(), length, NULL,
                                           0, NULL, NULL);
        if (utf8_len > 0) {
          utf8.resize(utf8_len);
          WideCharToMultiByte(CP_UTF8, 0, buf.data(), length, &utf8[0],
                              utf8_len, NULL, NULL);
        }
        response["result"] = {{"value", utf8}};
      } else {
        response["error"] = "Failed to read string characters";
      }
    }
  } else if (method == "invoke_method") {
    uintptr_t method_ptr = request["params"].value("method_ptr", 0ULL);
    uintptr_t instance_ptr = request["params"].value("instance_ptr", 0ULL);

    std::vector<uintptr_t> args_in =
        request["params"].value("args", std::vector<uintptr_t>());
    std::vector<void *> args(args_in.size());
    for (size_t i = 0; i < args_in.size(); ++i) {
      args[i] = reinterpret_cast<void *>(args_in[i]);
    }

    void *exc = nullptr;
    void *ret = nullptr;
    bool ok = Unity.invoker.InvokeWithSEH(
        reinterpret_cast<void *>(method_ptr),
        reinterpret_cast<void *>(instance_ptr),
        args.empty() ? nullptr : args.data(), &exc, ret);
    if (!ok) {
      response["error"] = "SEH fault during invocation";
    } else if (exc != nullptr) {
      response["error"] = "Managed exception occurred during invocation";
    } else {
      response["result"] = {{"return_ptr", reinterpret_cast<uintptr_t>(ret)}};
    }
  } else if (method == "get_classes") {
    uintptr_t img = request["params"].value("image_ptr", 0ULL);
#ifdef ENABLE_DUMPER
    ::Engine::Dumper::ClassCatalog catalog(Unity);
    auto classes = catalog.GetRawClasses(reinterpret_cast<void *>(img));
    json class_list = json::array();
    for (const auto &c : classes) {
      class_list.push_back(
          {{"name", c.name},
           {"namespace", c.ns},
           {"class_ptr", reinterpret_cast<uintptr_t>(c.klassPtr)}});
    }
    response["result"] = {{"classes", class_list}};
#else
    response["error"] = "Dumper not enabled in this build";
#endif
  } else if (method == "get_methods") {
    uintptr_t klass = request["params"].value("class_ptr", 0ULL);
#ifdef ENABLE_DUMPER
    ::Engine::Dumper::MethodCatalog catalog(Unity);
    auto methods = catalog.GetRawMethods(reinterpret_cast<void *>(klass));
    json method_list = json::array();
    for (const auto &m : methods) {
      method_list.push_back(
          {{"name", m.name},
           {"return_type", m.returnType},
           {"parameters", m.parameters},
           {"address", m.address},
           {"is_static", m.isStatic},
           {"method_ptr", reinterpret_cast<uintptr_t>(m.engineHandle)}});
    }
    response["result"] = {{"methods", method_list}};
#else
    response["error"] = "Dumper not enabled in this build";
#endif
  } else if (method == "get_fields") {
    uintptr_t klass = request["params"].value("class_ptr", 0ULL);
    uintptr_t inst = request["params"].value("instance_ptr", 0ULL);
#ifdef ENABLE_DUMPER
    ::Engine::Dumper::FieldCatalog catalog(Unity);
    auto fields = inst ? catalog.GetRawFields(reinterpret_cast<void *>(klass),
                                              reinterpret_cast<void *>(inst))
                       : catalog.GetRawFields(reinterpret_cast<void *>(klass));
    json field_list = json::array();
    for (const auto &f : fields) {
      field_list.push_back({{"name", f.name},
                            {"type", f.type},
                            {"offset", f.offset},
                            {"is_static", f.isStatic},
                            {"value_display", f.valueDisplay}});
    }
    response["result"] = {{"fields", field_list}};
#else
    response["error"] = "Dumper not enabled in this build";
#endif
  } else if (method == "get_enum_literals") {
    uintptr_t klass = request["params"].value("class_ptr", 0ULL);
#ifdef ENABLE_DUMPER
    ::Engine::Dumper::FieldCatalog catalog(Unity);
    auto literals = catalog.GetEnumLiterals(reinterpret_cast<void *>(klass));
    json lit_list = json::array();
    for (const auto &lit : literals) {
      lit_list.push_back({{"name", lit.name}, {"value", lit.value}});
    }
    response["result"] = {{"literals", lit_list}};
#else
    response["error"] = "Dumper not enabled in this build";
#endif
  } else if (method == "get_all_images") {
#ifdef ENABLE_DUMPER
    ::Engine::Dumper::ClassCatalog catalog(Unity);
    auto images = catalog.GetLoadedImages();
    json img_list = json::array();
    for (const auto &img : images) {
      img_list.push_back(
          {{"name", img.name},
           {"image_ptr", reinterpret_cast<uintptr_t>(img.imagePtr)},
           {"class_count", img.classCount}});
    }
    response["result"] = {{"images", img_list}};
#else
    response["error"] = "Dumper not enabled in this build";
#endif
  } else {
    response["error"] = "Method not found";
  }
}

void ServerThreadLoop() {
  while (g_running) {
    g_hPipe =
        CreateNamedPipeA("\\\\.\\pipe\\DllStalker_MCP", PIPE_ACCESS_DUPLEX,
                         PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                         1, 4096, 4096, 0, NULL);

    if (g_hPipe == INVALID_HANDLE_VALUE) {
      Sleep(1000);
      continue;
    }

    if (ConnectNamedPipe(g_hPipe, NULL)
            ? TRUE
            : (GetLastError() == ERROR_PIPE_CONNECTED)) {
      char buffer[4096];
      DWORD bytesRead;
      while (g_running &&
             ReadFile(g_hPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL)) {
        buffer[bytesRead] = '\0';
        try {
          json request = json::parse(buffer);
          json response;
          if (request.contains("id")) {
            response["id"] = request["id"];
          }
          ProcessMcpMessage(request, response);

          std::string out = response.dump() + "\n";
          DWORD bytesWritten;
          WriteFile(g_hPipe, out.c_str(), out.length(), &bytesWritten, NULL);
        } catch (const std::exception &e) {
          json err = {{"error", e.what()}};
          std::string out = err.dump() + "\n";
          DWORD bw;
          WriteFile(g_hPipe, out.c_str(), out.length(), &bw, NULL);
        }
      }
    }
    DisconnectNamedPipe(g_hPipe);
    CloseHandle(g_hPipe);
    g_hPipe = INVALID_HANDLE_VALUE;
  }
}

void TcpServerThreadLoop() {
  WSADATA wsaData;
  if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    return;

  SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (listenSocket == INVALID_SOCKET) {
    WSACleanup();
    return;
  }

  sockaddr_in serverAddr = {};
  serverAddr.sin_family = AF_INET;
  InetPtonA(AF_INET, "127.0.0.1", &serverAddr.sin_addr); // Localhost only
  serverAddr.sin_port = htons(8765);

  if (bind(listenSocket, (SOCKADDR *)&serverAddr, sizeof(serverAddr)) ==
      SOCKET_ERROR) {
    closesocket(listenSocket);
    WSACleanup();
    return;
  }

  if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
    closesocket(listenSocket);
    WSACleanup();
    return;
  }

  while (g_running) {
    // Simple accept loop (blocks). To properly shut down, we can close
    // listenSocket from Stop().
    SOCKET clientSocket = accept(listenSocket, nullptr, nullptr);
    if (clientSocket == INVALID_SOCKET) {
      break;
    }

    char buffer[4096];
    int bytesReceived;
    while (g_running && (bytesReceived = recv(clientSocket, buffer,
                                              sizeof(buffer) - 1, 0)) > 0) {
      buffer[bytesReceived] = '\0';

      // Note: Simple TCP loop assumes one JSON payload per recv line.
      try {
        json request = json::parse(buffer);
        json response;
        if (request.contains("id")) {
          response["id"] = request["id"];
        }
        ProcessMcpMessage(request, response);

        std::string out = response.dump() + "\n";
        send(clientSocket, out.c_str(), out.length(), 0);
      } catch (const std::exception &e) {
        json err = {{"error", e.what()}};
        std::string out = err.dump() + "\n";
        send(clientSocket, out.c_str(), out.length(), 0);
      }
    }
    closesocket(clientSocket);
  }

  closesocket(listenSocket);
  WSACleanup();
}

} // namespace

void McpServer::Start() {
  if (g_running)
    return;
  g_running = true;
  BootstrapLog::Write("[*] Starting MCP Named Pipe & TCP servers...\n");
  g_serverThread = std::thread(ServerThreadLoop);
  g_serverThread.detach(); // Let it run in background

  std::thread tcpThread(TcpServerThreadLoop);
  tcpThread.detach();
}

void McpServer::Stop() {
  g_running = false;
  if (g_hPipe != INVALID_HANDLE_VALUE) {
    // Break any waiting ReadFile/ConnectNamedPipe
    HANDLE hFakeClient =
        CreateFileA("\\\\.\\pipe\\DllStalker_MCP", GENERIC_READ | GENERIC_WRITE,
                    0, NULL, OPEN_EXISTING, 0, NULL);
    if (hFakeClient != INVALID_HANDLE_VALUE)
      CloseHandle(hFakeClient);
  }
}

} // namespace Services
} // namespace Engine
