// No UI of a port's own: the player and boot built without RUNTIME_PORT_UI.
#include "port_ui.h"

namespace frontend {

std::unique_ptr<PortUi> make_port_ui(const runtime::Decomp &) { return nullptr; }

} // namespace frontend
