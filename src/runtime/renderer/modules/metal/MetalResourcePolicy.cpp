#include "MetalResourcePolicy.h"

#include "MetalRenderBackendErrors.h"

#include <bit>
#include <limits>
#include <optional>
#include <string>

namespace Horo::Render::Detail {
    namespace {  // NOSONAR(cpp:S1000) File-local Metal policy details intentionally have internal linkage.
        constexpr std::uint64_t SharedBufferCompatibility = 1;
        constexpr std::uint64_t PrivateBufferCompatibility = 2;
        constexpr std::uint64_t PrivateTextureCompatibility = 3;

        [[nodiscard]] Error PolicyError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }

        [[nodiscard]] Result<RenderMemoryCostPlan> PlanMemory(const std::size_t payload, const MetalResourceRequirements requirements,
                                                              const std::uint64_t compatibility) {
            if (payload == 0 || requirements.size < payload || requirements.alignment == 0 ||
                (requirements.alignment & (requirements.alignment - 1U)) != 0) {
                return Result<RenderMemoryCostPlan>::Failure(
                    PolicyError(MetalBackendErrors::ResourceCreationFailed, "Metal returned invalid heap memory requirements."));
            }
            return Result<RenderMemoryCostPlan>::Success({.memoryClass = RenderMemoryClass::PersistentDevice,
                                                          .allocationClass = RenderMemoryAllocationClass::Suballocated,
                                                          .provenance = RenderMemoryCostProvenance::Exact,
                                                          .compatibility = RenderMemoryCompatibilityId{compatibility},
                                                          .payloadBytes = payload,
                                                          .requiredBytes = requirements.size,
                                                          .alignment = requirements.alignment});
        }

        [[nodiscard]] bool ValidUploadLayoutRequest(const RenderTextureDescriptor &descriptor, const std::size_t rowAlignment) noexcept {
            return descriptor.IsValid() && descriptor.dimension == RenderTextureDimension::TwoD && descriptor.sampleCount == 1 &&
                   RenderTextureBaseLevelByteSize(descriptor).has_value() && RenderTextureTexelBytes(descriptor.format).has_value() &&
                   std::has_single_bit(rowAlignment);
        }

        [[nodiscard]] std::optional<std::size_t> CheckedProduct(const std::size_t left, const std::size_t right) noexcept {
            if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left)
                return std::nullopt;
            return left * right;
        }

        [[nodiscard]] std::optional<std::size_t> AlignedSize(const std::size_t value, const std::size_t alignment) noexcept {
            if (value > std::numeric_limits<std::size_t>::max() - (alignment - 1U))
                return std::nullopt;
            return ((value + alignment - 1U) / alignment) * alignment;
        }

        /** @brief Reports whether placement classification matches its queried plan. */
        [[nodiscard]] bool MatchesPlanClassification(const RenderMemoryCostPlan &plan, const RenderMemoryPlacement &placement) noexcept {
            return placement.memoryClass == plan.memoryClass && placement.allocationClass == plan.allocationClass &&
                   placement.provenance == plan.provenance && placement.compatibility == plan.compatibility;
        }

        /** @brief Reports whether placement sizes and alignment match their queried plan. */
        [[nodiscard]] bool MatchesPlanLayout(const RenderMemoryCostPlan &plan, const RenderMemoryPlacement &placement) noexcept {
            const bool aligned = plan.alignment != 0 && placement.offsetBytes % plan.alignment == 0;
            return placement.payloadBytes == plan.payloadBytes && placement.requiredBytes == plan.requiredBytes && aligned;
        }
    }  // namespace

    /** @copydoc MetalBufferStorage */
    MetalResourceStorage MetalBufferStorage(const RenderBufferDescriptor &descriptor) noexcept {
        return descriptor.access == RenderBufferAccess::HostVisible ? MetalResourceStorage::Shared : MetalResourceStorage::Private;
    }

    /** @copydoc MetalTextureStorage */
    MetalResourceStorage MetalTextureStorage(const RenderTextureDescriptor &) noexcept {
        return MetalResourceStorage::Private;
    }

    /** @copydoc PlanMetalBufferMemory */
    Result<RenderMemoryCostPlan> PlanMetalBufferMemory(const RenderBufferDescriptor &descriptor,
                                                       const MetalResourceRequirements requirements) {
        if (!descriptor.IsValid()) {
            return Result<RenderMemoryCostPlan>::Failure(
                PolicyError(MetalBackendErrors::InvalidConfig, "Metal buffer memory requirement request is invalid."));
        }
        const std::uint64_t compatibility =
            MetalBufferStorage(descriptor) == MetalResourceStorage::Shared ? SharedBufferCompatibility : PrivateBufferCompatibility;
        return PlanMemory(descriptor.byteSize, requirements, compatibility);
    }

    /** @copydoc PlanMetalTextureMemory */
    Result<RenderMemoryCostPlan> PlanMetalTextureMemory(const RenderTextureDescriptor &descriptor,
                                                        const MetalResourceRequirements requirements) {
        const auto payload = RenderTextureBaseLevelByteSize(descriptor);
        if (!payload.has_value() || descriptor.dimension != RenderTextureDimension::TwoD) {
            return Result<RenderMemoryCostPlan>::Failure(
                PolicyError(MetalBackendErrors::InvalidConfig, "Metal texture memory requirement request is invalid."));
        }
        return PlanMemory(*payload, requirements, PrivateTextureCompatibility);
    }

    /** @copydoc PlanMetalTextureUpload */
    Result<MetalTextureUploadLayout> PlanMetalTextureUpload(const RenderTextureDescriptor &descriptor, const std::size_t rowAlignment) {
        if (!ValidUploadLayoutRequest(descriptor, rowAlignment)) {
            return Result<MetalTextureUploadLayout>::Failure(
                PolicyError(MetalBackendErrors::InvalidConfig, "Metal texture upload layout request is invalid."));
        }
        const auto tightRowBytes = CheckedProduct(descriptor.extent.width, *RenderTextureTexelBytes(descriptor.format));
        const auto paddedRowBytes = tightRowBytes.has_value() ? AlignedSize(*tightRowBytes, rowAlignment) : std::nullopt;
        const auto tightImageBytes = tightRowBytes.has_value() ? CheckedProduct(*tightRowBytes, descriptor.extent.height) : std::nullopt;
        const auto paddedImageBytes = paddedRowBytes.has_value() ? CheckedProduct(*paddedRowBytes, descriptor.extent.height) : std::nullopt;
        const auto stagingBytes = paddedImageBytes.has_value() ? CheckedProduct(*paddedImageBytes, descriptor.layerCount) : std::nullopt;
        if (!tightImageBytes.has_value() || !stagingBytes.has_value()) {
            return Result<MetalTextureUploadLayout>::Failure(
                PolicyError(MetalBackendErrors::ResourceCreationFailed, "Metal texture upload layout is not representable."));
        }
        return Result<MetalTextureUploadLayout>::Success({.tightRowBytes = *tightRowBytes,
                                                          .tightImageBytes = *tightImageBytes,
                                                          .paddedRowBytes = *paddedRowBytes,
                                                          .paddedImageBytes = *paddedImageBytes,
                                                          .stagingBytes = *stagingBytes});
    }

    /** @copydoc ValidateMetalMeshBindings */
    Result<void> ValidateMetalMeshBindings(const RenderMeshDescriptor &descriptor, const RenderBufferUsage vertexUsage,
                                           const RenderBufferUsage indexUsage) {
        if (!descriptor.IsValid()) {
            return Result<void>::Failure(PolicyError(MetalBackendErrors::InvalidConfig, "Metal mesh descriptor is invalid."));
        }
        if (!HasBufferUsage(vertexUsage, RenderBufferUsage::Vertex) || !HasBufferUsage(indexUsage, RenderBufferUsage::Index)) {
            return Result<void>::Failure(PolicyError(MetalBackendErrors::UnsupportedResourceOperation,
                                                     "Metal mesh buffers lack their required vertex or index binding usage."));
        }
        return Result<void>::Success();
    }

    /** @copydoc MetalTextureAspectMatches */
    bool MetalTextureAspectMatches(const RenderTextureFormat format, const RenderTextureAspect aspect) noexcept {
        using enum RenderTextureFormat;
        using enum RenderTextureAspect;
        switch (format) {
            case Depth24Stencil8:
            case Depth32FloatStencil8:
                return aspect == Depth || aspect == Stencil || aspect == DepthStencil;
            case Depth16Unorm:
            case Depth32Float:
                return aspect == Depth;
            default:
                return aspect == Color;
        }
    }

    /** @copydoc ValidateMetalResourcePlacement */
    Result<void> ValidateMetalResourcePlacement(const RenderMemoryCostPlan &plan, const RenderMemoryPlacement &placement) {
        if (const bool matches =
                plan.IsValid() && placement.IsValid() && MatchesPlanClassification(plan, placement) && MatchesPlanLayout(plan, placement);
            !matches) {
            return Result<void>::Failure(
                PolicyError(MetalBackendErrors::InvalidConfig, "Metal resource placement does not match its queried requirements."));
        }
        return Result<void>::Success();
    }

    /** @copydoc ValidateMetalHeapReuse */
    Result<void> ValidateMetalHeapReuse(const MetalHeapState &heap, const RenderMemoryPlacement &placement,
                                        const MetalResourceStorage storage) {
        if (const bool storageValid = heap.storage == MetalResourceStorage::Shared || heap.storage == MetalResourceStorage::Private;
            !storageValid || !heap.compatibility.IsValid() || heap.backingBytes == 0 || !placement.IsValid() ||
            heap.backingBytes != placement.backingBytes || heap.compatibility != placement.compatibility || heap.storage != storage) {
            return Result<void>::Failure(
                PolicyError(MetalBackendErrors::InvalidConfig, "Metal placement conflicts with its existing backing heap."));
        }
        return Result<void>::Success();
    }

    /** @copydoc RetainMetalHeapResource */
    Result<void> RetainMetalHeapResource(MetalHeapState &heap) {
        if (heap.backingBytes == 0 || heap.liveResources == std::numeric_limits<std::size_t>::max()) {
            return Result<void>::Failure(
                PolicyError(MetalBackendErrors::ResourceCreationFailed, "Metal heap resource tracking capacity is exhausted."));
        }
        ++heap.liveResources;
        return Result<void>::Success();
    }

    /** @copydoc ReleaseMetalHeapResource */
    bool ReleaseMetalHeapResource(MetalHeapState &heap) noexcept {
        if (heap.liveResources == 0)
            return false;
        --heap.liveResources;
        return heap.liveResources == 0;
    }
}  // namespace Horo::Render::Detail
