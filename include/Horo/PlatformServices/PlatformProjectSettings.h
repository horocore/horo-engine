#pragma once

/** @file PlatformProjectSettings.h
 * @brief Inert Foundation settings contribution and typed projection for portable Platform Services project policy.
 */

#include "Horo/Foundation/ModuleHost.h"
#include "Horo/PlatformServices/PlatformProjectConfiguration.h"

namespace Horo::PlatformServices {
    /**
     * @brief Declares the project's provider selection and exhaustive service requirements without registering or activating them.
     * @return Owned contribution for horo.platform.services, with Project-only sources and ProjectReopen reload policy.
     * @details Salt, project identity, host profile and private mappings are not ordinary settings. Defaults describe explicit Null
     * with disabled services; project loading must still reject missing required authoring documents before resolution.
     */
    [[nodiscard]] ModuleConfigurationContribution MakePlatformProjectSettingsContribution();

    /**
     * @brief Projects validated portable policy into the sole Foundation resolver's project source map.
     * @param configuration Validated immutable policy, without provider-native values.
     * @return Complete project values or typed invalid-configuration failure for an uninitialized policy.
     * @details No environment or user source is introduced.
     */
    [[nodiscard]] Result<ConfigurationSourceMap> PlatformProjectSettingsValues(const PlatformProjectConfiguration &configuration);

    /**
     * @brief Builds typed policy from a captured Foundation snapshot using the common provider/capability validator.
     * @param snapshot Captured complete configuration generation from the host's sealed module schema.
     * @param projectId Authoritative root project identity, supplied by composition.
     * @param profile Explicit host-owned execution profile.
     * @param contributions Complete inert selected provider metadata.
     * @param trustedModules Exact contributing module identities approved by the host trust boundary.
     * @param limits Finite typed policy limits.
     * @return Immutable typed policy or a key-located shape, selection, trust or capability failure.
     */
    [[nodiscard]] Result<PlatformProjectConfiguration> ResolvePlatformProjectSettings(
        const ConfigurationSnapshot &snapshot, std::string_view projectId, PlatformServicesHostProfile profile,
        std::span<const PlatformProviderModuleContribution> contributions, std::span<const ModuleId> trustedModules,
        const PlatformProjectConfigurationLimits &limits = {});
}  // namespace Horo::PlatformServices
