#include "MetalDeviceCapabilities.h"

#include "MetalRenderBackendErrors.h"

#include <limits>
#include <string>

namespace Horo::Render::Detail {
    namespace {
        [[nodiscard]] bool HasUsage(const RenderTextureUsage available, const RenderTextureUsage requested) noexcept {
            return HasTextureUsage(available, requested);
        }

        [[nodiscard]] bool SupportsBaselineFormats(const MetalDeviceFacts &facts) noexcept {
            using enum RenderTextureFormat;
            using enum RenderTextureUsage;
            const RenderTextureDescriptor color{
                .extent = {1, 1},
                .format = Rgba8Unorm,
                .usage = Sampled | RenderAttachment,
            };
            const RenderTextureDescriptor depth{
                .extent = {1, 1},
                .format = Depth32Float,
                .usage = RenderAttachment,
            };
            const RenderTextureDescriptor presentation{
                .extent = {1, 1},
                .format = Bgra8Unorm,
                .usage = RenderAttachment,
            };
            return facts.formats.Supports(color) && facts.formats.Supports(depth) && facts.formats.Supports(presentation);
        }

        [[nodiscard]] Error AdmissionError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }

        [[nodiscard]] bool HasIdentityAndLimits(const MetalDeviceFacts &facts) noexcept {
            return facts.adapter.IsValid() && facts.discoveryRevision != 0 && facts.maxBufferLength != 0 &&
                   facts.maxTextureDimension2D != 0;
        }

        [[nodiscard]] bool SupportsRequiredFamily(const MetalDeviceFacts &facts) noexcept {
            return (facts.architecture == MetalHostArchitecture::Arm64 && facts.supportsApple7) ||
                   (facts.architecture == MetalHostArchitecture::X86_64 && facts.supportsMac2);
        }

        [[nodiscard]] Result<void> ValidateDeviceFacts(const MetalDeviceFacts &facts) {
            if (!HasIdentityAndLimits(facts)) {
                return Result<void>::Failure(
                    AdmissionError(MetalBackendErrors::InvalidDeviceFacts, "Metal returned incomplete adapter identity or device limits."));
            }
            if (facts.operatingSystemMajor < 14) {
                return Result<void>::Failure(AdmissionError(MetalBackendErrors::UnsupportedHost, "Metal requires macOS 14.0 or later."));
            }
            if (!SupportsRequiredFamily(facts)) {
                return Result<void>::Failure(
                    AdmissionError(MetalBackendErrors::UnsupportedDeviceFamily,
                                   "The selected Metal device does not satisfy the Apple7 arm64 or Mac2 x86_64 baseline."));
            }
            if (facts.adapter.availability != RenderAdapterAvailability::Available) {
                return Result<void>::Failure(AdmissionError(MetalBackendErrors::AdapterUnavailable,
                                                            "The selected Metal adapter is not available for device creation."));
            }
            if (!facts.commandQueueAvailable) {
                return Result<void>::Failure(AdmissionError(MetalBackendErrors::CommandQueueCreationFailed,
                                                            "The selected Metal device could not create its required command queue."));
            }
            if (!SupportsBaselineFormats(facts)) {
                return Result<void>::Failure(
                    AdmissionError(MetalBackendErrors::RequiredFormatUnsupported,
                                   "The selected Metal device lacks a required color, depth, or presentation format combination."));
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateSelection(const MetalDeviceFacts &facts, const MetalDeviceAdmissionRequest &request) {
            if (!request.adapter) {
                if (request.discoveryRevision != 0) {
                    return Result<void>::Failure(AdmissionError(MetalBackendErrors::InvalidConfig,
                                                                "A Metal discovery revision requires an explicit adapter identity."));
                }
                return Result<void>::Success();
            }
            if (request.discoveryRevision != facts.discoveryRevision) {
                return Result<void>::Failure(AdmissionError(MetalBackendErrors::StaleAdapterSnapshot,
                                                            "The explicit Metal adapter selection uses a stale discovery revision."));
            }
            if (*request.adapter != facts.adapter.id) {
                return Result<void>::Failure(
                    AdmissionError(MetalBackendErrors::AdapterNotFound, "The explicitly selected Metal adapter is no longer present."));
            }
            return Result<void>::Success();
        }
    }  // namespace

    bool MetalFormatCapabilities::Supports(const RenderTextureDescriptor &descriptor) const noexcept {
        // RND-005.3 currently realizes Metal textures with texture2DDescriptor; advertise only that implemented path.
        if (!descriptor.IsValid() || descriptor.dimension != RenderTextureDimension::TwoD) {
            return false;
        }
        const auto formatIndex = static_cast<std::size_t>(descriptor.format);
        if (formatIndex >= usages.size() || descriptor.sampleCount >= std::numeric_limits<std::uint64_t>::digits) {
            return false;
        }
        return HasUsage(usages[formatIndex], descriptor.usage) && (sampleCountMask & (std::uint64_t{1} << descriptor.sampleCount)) != 0;
    }

    bool MetalDeviceCapabilities::IsValid() const noexcept {
        return adapter.IsValid() && adapter.availability == RenderAdapterAvailability::Available && discoveryRevision != 0 &&
               deviceIncarnation != 0 && capabilityRevision != 0 && maxBufferLength != 0 && maxTextureDimension2D != 0 &&
               implemented.backend == RenderBackendId{"metal"};
    }

    bool MetalDeviceCapabilities::SupportsBuffer(const RenderBufferDescriptor &descriptor) const noexcept {
        return IsValid() && implemented.support.Supports(descriptor);
    }

    bool MetalDeviceCapabilities::SupportsTexture(const RenderTextureDescriptor &descriptor) const noexcept {
        return IsValid() && implemented.support.Supports(descriptor);
    }

    Result<MetalDeviceCapabilities> AdmitMetalDevice(const MetalDeviceFacts &facts, const MetalDeviceAdmissionRequest &request) {
        if (const Result<void> deviceFacts = ValidateDeviceFacts(facts); deviceFacts.HasError()) {
            return Result<MetalDeviceCapabilities>::Failure(deviceFacts.ErrorValue());
        }
        if (const Result<void> selection = ValidateSelection(facts, request); selection.HasError()) {
            return Result<MetalDeviceCapabilities>::Failure(selection.ErrorValue());
        }
        if (request.requirePresentation && !facts.adapter.supportsPresentation) {
            return Result<MetalDeviceCapabilities>::Failure(
                AdmissionError(MetalBackendErrors::PresentationUnsupported,
                               "The selected Metal adapter cannot present to the host display."));
        }

        RenderCapabilitySnapshot support{
            .deviceIncarnation = 1,
            .capabilityRevision = 1,
            .synthetic = false,
            .features = {},
            .queues = {.graphics = true, .compute = false, .copy = false, .present = facts.adapter.supportsPresentation},
            .limits = {.maxBufferBytes = facts.maxBufferLength,
                       .maxTextureDimension2D = facts.maxTextureDimension2D,
                       .maxColorAttachments = 8,
                       .maxVertexAttributes = 31,
                       .maxFramesInFlight = 3},
            .formats = {},
        };
        for (const RenderCapability capability :
             {RenderCapability::OffscreenTargets, RenderCapability::BufferResources, RenderCapability::MeshResources,
              RenderCapability::TextureResources, RenderCapability::RenderTargetResources})
            support.features.Enable(capability);
        if (facts.adapter.supportsPresentation)
            support.features.Enable(RenderCapability::Presentation);
        support.formats.usages = facts.formats.usages;
        support.formats.sampleCountMask = facts.formats.sampleCountMask;

        MetalDeviceCapabilities capabilities{
            .adapter = facts.adapter,
            .discoveryRevision = facts.discoveryRevision,
            .deviceIncarnation = 1,
            .capabilityRevision = 1,
            .maxBufferLength = facts.maxBufferLength,
            .maxTextureDimension2D = facts.maxTextureDimension2D,
            .formats = facts.formats,
            .implemented =
                RenderBackendCapabilities{
                    .backend = RenderBackendId{"metal"},
                    .presentsToWindow = facts.adapter.supportsPresentation,
                    .supportsOffscreenTargets = true,
                    .supportsTimestampQueries = false,
                    .supportsCompute = false,
                    .supportsBindlessResources = false,
                    .supportsRayTracing = false,
                    .supportsBufferResources = true,
                    .supportsMeshResources = true,
                    .supportsTextureResources = true,
                    .supportsRenderTargetResources = true,
                    .support = std::move(support),
                },
        };
        return Result<MetalDeviceCapabilities>::Success(std::move(capabilities));
    }
}  // namespace Horo::Render::Detail
