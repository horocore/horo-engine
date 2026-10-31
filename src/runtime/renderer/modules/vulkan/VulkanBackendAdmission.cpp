#include "VulkanBackendInternal.h"
#include "VulkanRenderBackendErrors.h"

#include <limits>
#include <set>
#include <utility>

namespace Horo::Render::Detail {
    namespace {
        constexpr std::string_view SwapchainExtension{"VK_KHR_swapchain"};

        [[nodiscard]] bool HasGraphicsQueue(const VulkanAdapterCandidate &candidate) noexcept {
            return std::ranges::any_of(candidate.queueFamilies, &VulkanQueueFamily::supportsGraphics);
        }

        [[nodiscard]] bool HasPresentationQueue(const VulkanAdapterCandidate &candidate) noexcept {
            return std::ranges::any_of(candidate.queueFamilies, &VulkanQueueFamily::supportsPresentation);
        }

        [[nodiscard]] bool IsCandidateDataValid(const VulkanAdapterCandidate &candidate) {
            if (!candidate.id.IsValid() || candidate.displayName.empty() || candidate.displayName.size() > 256 ||
                candidate.queueFamilies.size() > 256) {
                return false;
            }
            std::set<std::uint32_t, std::less<>> queueIndices;
            for (const VulkanQueueFamily &queue : candidate.queueFamilies) {
                if (!queueIndices.insert(queue.index).second) {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool IsEligible(const VulkanAdapterCandidate &candidate, const bool requirePresentation) {
            return candidate.standardApiVariant && !candidate.portabilitySubset && candidate.driverAllowed && candidate.hostAllowed &&
                   VulkanSupportsVersion(candidate.apiVersion) && candidate.requiredFeatures.AreAvailable() &&
                   HasGraphicsQueue(candidate) &&
                   (!requirePresentation ||
                    (VulkanContains(candidate.deviceExtensions, SwapchainExtension) && candidate.swapchainMaintenance &&
                     VulkanContains(candidate.deviceExtensions, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME) &&
                     HasPresentationQueue(candidate)));
        }

        [[nodiscard]] const ErrorCodeDescriptor &RejectionDescriptor(const VulkanAdapterCandidate &candidate,
                                                                     const bool requirePresentation) {
            if (!candidate.standardApiVariant || candidate.portabilitySubset) {
                return VulkanBackendErrors::ApiVariantUnsupported;
            }
            if (!VulkanSupportsVersion(candidate.apiVersion)) {
                return VulkanBackendErrors::DeviceVersionUnsupported;
            }
            if (!candidate.requiredFeatures.AreAvailable()) {
                return VulkanBackendErrors::RequiredFeaturesUnavailable;
            }
            if (!candidate.driverAllowed) {
                return VulkanBackendErrors::DriverUnsupported;
            }
            if (!candidate.hostAllowed)
                return VulkanBackendErrors::AdapterRequirementsUnsupported;
            if (!HasGraphicsQueue(candidate)) {
                return VulkanBackendErrors::GraphicsQueueUnavailable;
            }
            if (requirePresentation && !VulkanContains(candidate.deviceExtensions, SwapchainExtension)) {
                return VulkanBackendErrors::DeviceExtensionMissing;
            }
            return requirePresentation ? VulkanBackendErrors::PresentationUnsupported : VulkanBackendErrors::AdapterRequirementsUnsupported;
        }

        [[nodiscard]] std::optional<std::uint32_t> FindCommonQueue(const VulkanAdapterCandidate &candidate) {
            std::optional<std::uint32_t> commonIndex;
            for (const VulkanQueueFamily &queue : candidate.queueFamilies) {
                if (queue.supportsGraphics && queue.supportsPresentation && (!commonIndex.has_value() || queue.index < *commonIndex)) {
                    commonIndex = queue.index;
                }
            }
            return commonIndex;
        }

        [[nodiscard]] std::uint32_t FindMinimumQueue(const VulkanAdapterCandidate &candidate, const bool presentation) {
            std::uint32_t selected = std::numeric_limits<std::uint32_t>::max();
            for (const VulkanQueueFamily &queue : candidate.queueFamilies) {
                const bool supported = presentation ? queue.supportsPresentation : queue.supportsGraphics;
                selected = supported ? std::min(selected, queue.index) : selected;
            }
            return selected;
        }
    }  // namespace

    [[nodiscard]] Result<void> ValidateEnumeration(const VulkanAdapterEnumeration &enumeration) {
        if (enumeration.revision == 0 || enumeration.adapters.size() > 64) {
            return Result<void>::Failure(
                MakeError(VulkanBackendErrors::AdapterDataInvalid, "Physical-device enumeration was oversized or unversioned."));
        }
        if (enumeration.adapters.empty()) {
            return Result<void>::Failure(
                MakeError(VulkanBackendErrors::AdapterRequirementsUnsupported, "No Vulkan physical device was enumerated."));
        }
        std::set<std::string, std::less<>> identities;
        for (const VulkanAdapterCandidate &candidate : enumeration.adapters) {
            if (!IsCandidateDataValid(candidate) || !identities.insert(candidate.id.Value()).second) {
                return Result<void>::Failure(
                    MakeError(VulkanBackendErrors::AdapterDataInvalid, "Physical-device records are malformed or duplicated."));
            }
        }
        return Result<void>::Success();
    }

    [[nodiscard]] Result<const VulkanAdapterCandidate *> SelectExplicitCandidate(const RenderBackendConfig &config,
                                                                                 const VulkanAdapterEnumeration &enumeration) {
        if (config.adapterDiscoveryRevision != enumeration.revision) {
            return Result<const VulkanAdapterCandidate *>::Failure(
                MakeError(VulkanBackendErrors::AdapterRevisionStale, "Explicit adapter discovery revision is stale."));
        }
        const auto match = std::ranges::find_if(enumeration.adapters, [&](const VulkanAdapterCandidate &candidate) {
            return candidate.id == *config.adapter;
        });
        if (match == enumeration.adapters.end()) {
            return Result<const VulkanAdapterCandidate *>::Failure(
                MakeError(VulkanBackendErrors::AdapterNotFound, "Requested Vulkan adapter was not enumerated."));
        }
        if (!IsEligible(*match, config.requirePresentation)) {
            return Result<const VulkanAdapterCandidate *>::Failure(
                MakeError(RejectionDescriptor(*match, config.requirePresentation),
                          "Requested Vulkan adapter does not satisfy the Vulkan 1.3 device contract."));
        }
        return Result<const VulkanAdapterCandidate *>::Success(std::to_address(match));
    }

    [[nodiscard]] const VulkanAdapterCandidate *SelectAutomaticCandidate(const RenderBackendConfig &config,
                                                                         const VulkanAdapterEnumeration &enumeration) {
        const VulkanAdapterCandidate *best = nullptr;
        for (const VulkanAdapterCandidate &candidate : enumeration.adapters) {
            if (!IsEligible(candidate, config.requirePresentation)) {
                continue;
            }
            if (best == nullptr || candidate.id.Value() < best->id.Value()) {
                best = &candidate;
            }
        }
        return best;
    }

    [[nodiscard]] std::pair<std::uint32_t, std::uint32_t> SelectQueues(const VulkanAdapterCandidate &candidate,
                                                                       const bool requirePresentation) {
        if (const std::optional<std::uint32_t> commonIndex = requirePresentation ? FindCommonQueue(candidate) : std::nullopt;
            commonIndex.has_value()) {
            return {*commonIndex, *commonIndex};
        }
        const std::uint32_t graphics = FindMinimumQueue(candidate, false);
        const std::uint32_t presentation = requirePresentation ? FindMinimumQueue(candidate, true) : graphics;
        return {graphics, requirePresentation ? presentation : graphics};
    }

    [[nodiscard]] Result<std::uint32_t> SelectTransferQueue(const VulkanAdapterCandidate &candidate, const RenderBackendConfig &config,
                                                            const std::uint32_t graphics) {
        const auto graphicsRole = std::ranges::find(config.queueAssignments, RenderQueueRole::Graphics, &RenderQueueAssignment::role);
        const auto transferRole = std::ranges::find(config.queueAssignments, RenderQueueRole::Transfer, &RenderQueueAssignment::role);
        if (graphicsRole == config.queueAssignments.end() || transferRole == config.queueAssignments.end() ||
            std::ranges::any_of(config.queueAssignments, [](const auto &queue) {
            return queue.role == RenderQueueRole::Compute;
        }))
            return Result<std::uint32_t>::Failure(
                MakeError(VulkanBackendErrors::UnsupportedOperation,
                          "Vulkan command execution requires graphics/transfer roles; compute workloads are separate."));
        if (graphicsRole->queue == transferRole->queue)
            return Result<std::uint32_t>::Success(graphics);
        std::uint32_t transfer = VK_QUEUE_FAMILY_IGNORED;
        for (const auto &queue : candidate.queueFamilies)
            if (queue.index != graphics && queue.supportsTransfer && queue.index < transfer)
                transfer = queue.index;
        if (transfer == VK_QUEUE_FAMILY_IGNORED)
            return Result<std::uint32_t>::Failure(MakeError(VulkanBackendErrors::GraphicsQueueUnavailable,
                                                            "The requested distinct transfer queue has no admitted native "
                                                            "family; explicitly alias roles for a single-queue plan."));
        return Result<std::uint32_t>::Success(transfer);
    }
}  // namespace Horo::Render::Detail
