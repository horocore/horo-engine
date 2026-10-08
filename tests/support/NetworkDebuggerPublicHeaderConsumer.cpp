#include "Horo/Application/NetworkDebugger.h"
#include "Horo/Network/NetworkDebugger.h"

#include <type_traits>
static_assert(std::is_trivially_copyable_v<Horo::Network::NetworkDebuggerSnapshot>);
static_assert(!std::is_copy_constructible_v<Horo::Network::NetworkDebugger>);

int main() {
    Horo::Application::NetworkDebuggerService service;
    return service.Query().state == Horo::Application::NetworkDebuggerState::Detached ? 0 : 1;
}
