#pragma once

#include <VulkanBackendModule.h>
#include <algorithm>
#include <string_view>

namespace Horo::Render::Detail {
    inline constexpr VulkanApiVersion RequiredVulkanApiVersion{1, 3, 0};

    [[nodiscard]] inline bool VulkanSupportsVersion(const VulkanApiVersion version) noexcept {
        return version.major > RequiredVulkanApiVersion.major ||
               (version.major == RequiredVulkanApiVersion.major && version.minor >= RequiredVulkanApiVersion.minor);
    }

    [[nodiscard]] inline bool VulkanLoaderMeetsBaseline(const VulkanLoaderCapabilities &loader) noexcept {
        return loader.hasInstanceVersionQuery && VulkanSupportsVersion(loader.apiVersion);
    }

    [[nodiscard]] inline bool VulkanContains(const std::vector<std::string> &values, const std::string_view expected) {
        return std::ranges::find(values, expected) != values.end();
    }

    [[nodiscard]] inline bool VulkanHasQueue(const VulkanAdapterCandidate &candidate, const bool presentation) {
        return std::ranges::any_of(candidate.queueFamilies, [presentation](const VulkanQueueFamily &queue) {
            return presentation ? queue.supportsPresentation : queue.supportsGraphics;
        });
    }

    /** @brief Validates bounded, unique adapter enumeration metadata before selection. */
    [[nodiscard]] Result<void> ValidateEnumeration(const VulkanAdapterEnumeration &enumeration);
    /** @brief Resolves the exact explicit adapter/revision without fallback. */
    [[nodiscard]] Result<const VulkanAdapterCandidate *> SelectExplicitCandidate(const RenderBackendConfig &config,
                                                                                 const VulkanAdapterEnumeration &enumeration);
    /** @brief Selects the deterministic eligible adapter when no explicit identity was requested. */
    [[nodiscard]] const VulkanAdapterCandidate *SelectAutomaticCandidate(const RenderBackendConfig &config,
                                                                         const VulkanAdapterEnumeration &enumeration);
    /** @brief Selects admitted graphics/presentation families from actual metadata. */
    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> SelectQueues(const VulkanAdapterCandidate &candidate, bool requirePresentation);
    /** @brief Preserves explicit effective queue identity; a distinct transfer role requires a distinct family. */
    [[nodiscard]] Result<std::uint32_t> SelectTransferQueue(const VulkanAdapterCandidate &candidate, const RenderBackendConfig &config,
                                                            std::uint32_t graphics);

    /** @brief Merges exact host preflight instance requirements with existing platform/baseline names before native creation.
     * @param required Existing WSI/validation requirements. @param loader Actual loader capabilities.
     * @param host Immutable selected-host requirements. @return Exact unique names or typed version/missing-extension denial.
     */
    [[nodiscard]] Result<std::vector<std::string>> ResolveHostInstanceExtensions(std::vector<std::string> required,
                                                                                 const VulkanLoaderCapabilities &loader,
                                                                                 const VulkanHostGraphicsRequirements &host);
    /** @brief Merges exact host device requirements with existing backend names before creating the selected device.
     * @param required Existing backend feature names. @param candidate Actual selected physical-device facts.
     * @param host Immutable selected-host requirements. @return Exact unique names or typed missing-extension denial.
     */
    [[nodiscard]] Result<std::vector<std::string>> ResolveHostDeviceExtensions(std::vector<std::string> required,
                                                                               const VulkanAdapterCandidate &candidate,
                                                                               const VulkanHostGraphicsRequirements &host);

    /** @brief Test seam that shares registration implementation with host composition. */
    [[nodiscard]] Result<void> RegisterVulkanRenderBackendWithRuntimePort(RenderBackendRegistry &registry, IVulkanRuntimePort &runtimePort);
    /** @brief Registration seam that retains one backend-owned native runtime. */
    [[nodiscard]] Result<void> RegisterVulkanRenderBackendWithOwnedRuntimePort(RenderBackendRegistry &registry,
                                                                               std::shared_ptr<IVulkanRuntimePort> runtimePort);
    /** @brief Test seam that creates discovery over a borrowed backend-native runtime. */
    [[nodiscard]] std::unique_ptr<IRenderAdapterDiscovery> CreateVulkanAdapterDiscoveryWithRuntimePort(IVulkanRuntimePort &runtimePort);
    /** @brief Creates discovery while retaining one backend-owned native runtime. */
    [[nodiscard]] std::unique_ptr<IRenderAdapterDiscovery> CreateVulkanAdapterDiscoveryWithOwnedRuntimePort(
        std::shared_ptr<IVulkanRuntimePort> runtimePort);
}  // namespace Horo::Render::Detail
