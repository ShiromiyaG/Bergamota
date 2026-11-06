// A frame of Dear ImGui's draw lists drawn into a picture on the CPU, for
// boot's composed shots (a port's UI over them, port_ui.h): triangles, their
// vertex colors and the atlas, blended over it.
#ifndef FRONTEND_RASTER_UI_H
#define FRONTEND_RASTER_UI_H

#include "runtime/gpu.h"

#include <imgui.h>

namespace frontend {

void raster_ui(const ImDrawData *data, runtime::Picture &into);

} // namespace frontend

#endif
