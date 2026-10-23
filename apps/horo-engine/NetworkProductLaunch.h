#pragma once

#include <memory>
#include <span>

namespace Horo::Network {
    struct GnsNativeSecurityOptions;
}

namespace Horo::Application::Internal {
    class NetworkProductAdmission;
    /** @brief Run a bounded product invocation with explicit host security capabilities.
     * @param arguments Typed CLI surface parsed by the product host.
     * @param admission Retained immutable host policy/providers; absence admits no network peer.
     * @param nativeSecurity Explicit GNS root/provisioning selection; native authority slots in admission must be empty.
     * @return Nonzero on capability/lifecycle failure. Credentials are never read from CLI or project data. */
    [[nodiscard]] int RunNetworkProduct(std::span<char *> arguments, std::shared_ptr<const NetworkProductAdmission> admission = nullptr,
                                        std::shared_ptr<const Network::GnsNativeSecurityOptions> nativeSecurity = nullptr);
}  // namespace Horo::Application::Internal
