#pragma once

#include "build_config.h"

#ifdef ENABLE_DUMPER

namespace Gui
{
struct ControlPanelSessionState;
}

namespace Gui::Views
{
void RenderNavigationStatusBanner(const ControlPanelSessionState& state,
                                 bool sameLine = false);
} // namespace Gui::Views

#endif // ENABLE_DUMPER
