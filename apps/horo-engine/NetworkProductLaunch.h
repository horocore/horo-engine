#pragma once

#include <memory>
#include <span>

namespace Horo::Application::Internal {
    class NetworkProductAdmission;
    /** @brief Run a bounded product invocation with explicit host security capabilities.
     * @param arguments Typed CLI surface parsed by the product host.
     * @param admission Retained immutable host policy/providers; absence admits no network peer.
     * @return Nonzero on capability/lifecycle failure. Credentials are never read from CLI or project data. */
    [[nodiscard]] int RunNetworkProduct(std::span<char *> arguments, std::shared_ptr<const NetworkProductAdmission> admission = nullptr);
}  // namespace Horo::Application::Internal
