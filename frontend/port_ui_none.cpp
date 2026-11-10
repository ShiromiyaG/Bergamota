// No UI of a port's own, and no look for its menu: the player and boot
// built without RUNTIME_PORT_UI.
#include "port_ui.h"

namespace frontend {

std::unique_ptr<PortUi> make_port_ui(const runtime::Decomp &) { return nullptr; }

std::unique_ptr<PortMenu> make_port_menu(uint64_t) { return nullptr; }

} // namespace frontend
