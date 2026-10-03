import asyncio
import json
import socket
import sys

USE_TCP = False
try:
    import win32file
    import win32pipe
    import pywintypes
except ImportError:
    USE_TCP = True

from mcp.server.fastmcp import FastMCP

app = FastMCP("dllstalker-mcp")

PIPE_NAME = r'\\.\pipe\DllStalker_MCP'
TCP_HOST = '127.0.0.1'
TCP_PORT = 8765

def send_tcp_request(method: str, params: dict) -> dict:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
            s.connect((TCP_HOST, TCP_PORT))
            request = json.dumps({"method": method, "params": params, "id": 1}) + "\n"
            s.sendall(request.encode('utf-8'))
            
            resp_data = b""
            while b"\n" not in resp_data:
                chunk = s.recv(4096)
                if not chunk:
                    break
                resp_data += chunk
                
            response = json.loads(resp_data.decode('utf-8').strip())
            if "error" in response:
                raise Exception(response["error"])
            return response.get("result", {})
    except ConnectionRefusedError:
        raise Exception(f"Failed to connect to DllStalker TCP Server on {TCP_HOST}:{TCP_PORT}. Is the game running?")

def send_pipe_request(method: str, params: dict) -> dict:
    if USE_TCP:
        return send_tcp_request(method, params)

    try:
        handle = win32file.CreateFile(
            PIPE_NAME,
            win32file.GENERIC_READ | win32file.GENERIC_WRITE,
            0,
            None,
            win32file.OPEN_EXISTING,
            0,
            None
        )
    except pywintypes.error as e:
        # Fallback to TCP if pipe fails
        return send_tcp_request(method, params)

    try:
        request = json.dumps({"method": method, "params": params, "id": 1}) + "\n"
        win32file.WriteFile(handle, request.encode('utf-8'))
        
        resp_data = b""
        while True:
            err, data = win32file.ReadFile(handle, 4096)
            resp_data += data
            if b"\n" in data:
                break
        
        response = json.loads(resp_data.decode('utf-8').strip())
        if "error" in response:
            raise Exception(response["error"])
        return response.get("result", {})
    finally:
        win32file.CloseHandle(handle)

@app.tool()
async def find_image(name: str) -> str:
    """Finds an assembly image by name in the Unity process."""
    result = send_pipe_request("find_image", {"name": name})
    return f"Image pointer: {hex(result.get('image_ptr', 0))}"

@app.tool()
async def get_method_address(image_ptr: int, class_name: str, method_name: str, namespace: str = "") -> str:
    """Gets the native memory address of a C# method."""
    result = send_pipe_request("get_method_address", {
        "image_ptr": image_ptr,
        "class": class_name,
        "method": method_name,
        "namespace": namespace
    })
    return f"Method address: {hex(result.get('address', 0))}"

@app.tool()
async def get_field_offset(image_ptr: int, class_name: str, field_name: str, namespace: str = "") -> str:
    """Gets the offset of a C# field within an object instance."""
    result = send_pipe_request("get_field_offset", {
        "image_ptr": image_ptr,
        "class": class_name,
        "field": field_name,
        "namespace": namespace
    })
    return f"Field offset: {hex(result.get('offset', 0))}"

@app.tool()
async def read_i32(address: int) -> str:
    """Reads a 32-bit signed integer from the given memory address."""
    result = send_pipe_request("read_i32", {"address": address})
    return f"Value: {result.get('value')}"

@app.tool()
async def write_i32(address: int, value: int) -> str:
    """Writes a 32-bit signed integer to the given memory address."""
    result = send_pipe_request("write_i32", {"address": address, "value": value})
    return f"Success: {result.get('success')}"

@app.tool()
async def read_f32(address: int) -> str:
    """Reads a 32-bit float from the given memory address."""
    result = send_pipe_request("read_f32", {"address": address})
    return f"Value: {result.get('value')}"

@app.tool()
async def write_f32(address: int, value: float) -> str:
    """Writes a 32-bit float to the given memory address."""
    result = send_pipe_request("write_f32", {"address": address, "value": value})
    return f"Success: {result.get('success')}"

@app.tool()
async def read_pointer(address: int) -> str:
    """Reads a pointer (64-bit on x64) from the given memory address."""
    result = send_pipe_request("read_pointer", {"address": address})
    return f"Pointer: {hex(result.get('value', 0))}"

@app.tool()
async def read_string(address: int) -> str:
    """Reads a managed C# string (System.String) from the given object pointer address."""
    result = send_pipe_request("read_string", {"address": address})
    return f"String: {result.get('value')}"

@app.tool()
async def invoke_method(method_ptr: int, instance_ptr: int = 0, args: list[int] = []) -> str:
    """Invokes a C# method natively. Pass instance_ptr=0 for static methods. 
    Args must be a list of pointers to the arguments (or boxed values)."""
    result = send_pipe_request("invoke_method", {
        "method_ptr": method_ptr,
        "instance_ptr": instance_ptr,
        "args": args
    })
    return f"Returned pointer: {hex(result.get('return_ptr', 0))}"

@app.tool()
async def get_classes(image_ptr: int) -> str:
    """Gets a list of all classes within the specified image/assembly."""
    result = send_pipe_request("get_classes", {"image_ptr": image_ptr})
    classes = result.get('classes', [])
    output = []
    for c in classes:
        output.append(f"Class: {c.get('namespace', '')}.{c.get('name', '')} (ptr: {hex(c.get('class_ptr', 0))})")
    return "\n".join(output) if output else "No classes found."

@app.tool()
async def get_methods(class_ptr: int) -> str:
    """Gets a list of all methods within the specified class."""
    result = send_pipe_request("get_methods", {"class_ptr": class_ptr})
    methods = result.get('methods', [])
    output = []
    for m in methods:
        static_str = "static " if m.get('is_static') else ""
        output.append(f"Method: {static_str}{m.get('return_type')} {m.get('name')}({m.get('parameters')}) "
                      f"(address: {hex(m.get('address', 0))}, ptr: {hex(m.get('method_ptr', 0))})")
    return "\n".join(output) if output else "No methods found."

@app.tool()
async def get_fields(class_ptr: int, instance_ptr: int = 0) -> str:
    """Gets a list of all fields within the specified class. If instance_ptr is provided, also reads their values."""
    result = send_pipe_request("get_fields", {"class_ptr": class_ptr, "instance_ptr": instance_ptr})
    fields = result.get('fields', [])
    output = []
    for f in fields:
        static_str = "static " if f.get('is_static') else ""
        val_str = f" = {f.get('value_display')}" if 'value_display' in f and f.get('value_display') else ""
        output.append(f"Field: {static_str}{f.get('type')} {f.get('name')} (offset: {hex(f.get('offset', 0))}){val_str}")
    return "\n".join(output) if output else "No fields found."

@app.tool()
async def get_enum_literals(class_ptr: int) -> str:
    """Gets all enum literals for a given enum class pointer."""
    result = send_pipe_request("get_enum_literals", {"class_ptr": class_ptr})
    literals = result.get('literals', [])
    output = []
    for lit in literals:
        output.append(f"Enum: {lit.get('name')} = {lit.get('value')}")
    return "\n".join(output) if output else "No enum literals found."

@app.tool()
async def get_all_images() -> str:
    """Gets a list of all loaded images (assemblies) in the Unity process."""
    result = send_pipe_request("get_all_images", {})
    images = result.get('images', [])
    output = []
    for img in images:
        output.append(f"Image: {img.get('name')} (ptr: {hex(img.get('image_ptr', 0))}, classes: {img.get('class_count', 0)})")
    return "\n".join(output) if output else "No images found."

if __name__ == "__main__":
    app.run()
