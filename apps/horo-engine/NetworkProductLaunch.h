#pragma once

#include "Horo/Prefab/PrefabMetrics.h"

#include <memory>
#include <span>

namespace Horo::Network {
    struct GnsNativeSecurityOptions;
}

namespace Horo::Application::Internal {
    class NetworkProductAdmission;
    class AnimationRuntimeIdentitySource;
    /** @brief Run a bounded product invocation with explicit host security capabilities.
     * @param arguments Typed CLI surface parsed by the product host.
     * @param admission Retained immutable host policy/providers; absence admits no network peer.
     * @param nativeSecurity Explicit GNS root/provisioning selection; native authority slots in admission must be empty.
     * @param metrics Explicit process-prebound observation handles; no registration occurs in product factories.
     * @param animationIdentities Actual executable-owned nonwrapping issuer shared across configured world records/travel; no fallback
     * issuer.
     * @return Nonzero on capability/lifecycle failure. Credentials are never read from CLI or project data. */
    [[nodiscard]] int RunNetworkProduct(std::span<char *> arguments, std::shared_ptr<const NetworkProductAdmission> admission = nullptr,
                                        std::shared_ptr<const Network::GnsNativeSecurityOptions> nativeSecurity = nullptr,
                                        const Prefab::PrefabMetricHandles &metrics = {},
                                        std::shared_ptr<AnimationRuntimeIdentitySource> animationIdentities = nullptr);
}  // namespace Horo::Application::Internal
