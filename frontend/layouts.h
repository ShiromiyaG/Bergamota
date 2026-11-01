// The single screen's layouts: where each screen goes on one canvas
// (VulkanPresenter's placements), shared by the player, which shows them,
// and boot, which composes them on the CPU for its shots. A layout (the
// scene's, scenes.h):
//   "panel"   the top screen fills the canvas, the bottom one a panel in a
//             corner, sliding in and out
//   "stacked" both screens one over the other, the sides filled with the top
//             one blurred and dimmed
//   "top"     the top screen alone
#ifndef FRONTEND_LAYOUTS_H
#define FRONTEND_LAYOUTS_H

#include "runtime/gpu.h"
#include "runtime/vulkan_presenter.h"

#include <string>

namespace frontend {

// the top screen fills the canvas; the bottom one a panel in corner
// ("bottom-right", "top-left"...), size times its own, in from its side as
// shown goes from 0 to 1
void place_single(runtime::VulkanPresenter::View &view, float shown, float size, const std::string &corner);

// both screens stacked on a canvas as wide as the window's shape, its sides
// filled with the top screen blurred and dimmed
void place_stacked(runtime::VulkanPresenter::View &view);

// the layout by its name ("panel", "stacked", "top"; the panel shown as
// much as shown)
void place(runtime::VulkanPresenter::View &view, const std::string &layout, float shown, float size, const std::string &corner);

// the placements in the window's own pixels (the canvas fitted), so two
// layouts with canvases of their own can be mixed
void in_window(runtime::VulkanPresenter::View &view);

// the view's canvas drawn on the CPU, one pixel a canvas unit, the screens'
// pictures sampled at their nearest pixel (boot's shots of what the player
// shows)
runtime::Picture compose(runtime::VulkanPresenter::View view, const runtime::Picture &top, const runtime::Picture &bottom);

} // namespace frontend

#endif
