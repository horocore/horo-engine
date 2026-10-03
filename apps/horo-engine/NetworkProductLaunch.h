#pragma once

#include <span>

namespace Horo::Application::Internal {
    // A bounded product invocation; exits non-zero on capability or lifecycle failure.
    [[nodiscard]] int RunNetworkProduct(std::span<char *> arguments);
}  // namespace Horo::Application::Internal
