#pragma once

/**
 * @file MetalResourcePolicy.h
 * @brief Native-free Metal resource storage, requirement, and placement policy.
 */

#include "Horo/Runtime/Render/RenderBackend.h"

namespace Horo::Render::Detail {
    /** @brief Backend-private Metal storage policy independent of Objective-C native types. */
    enum class MetalResourceStorage : std::uint8_t {
        Shared,
        Private,
    };

    /** @brief Native size and alignment returned by Metal heap requirement queries. */
    struct MetalResourceRequirements {
        std::size_t size{0};
        std::size_t alignment{0};
    };

    /** @brief Native-free row and image layout for one private Metal texture upload. */
    struct MetalTextureUploadLayout {
        std::size_t tightRowBytes{0};
        std::size_t tightImageBytes{0};
        std::size_t paddedRowBytes{0};
        std::size_t paddedImageBytes{0};
        std::size_t stagingBytes{0};
    };

    /** @brief Native-free lifecycle state paired with one backend-private Metal heap. */
    struct MetalHeapState {
        MetalResourceStorage storage{MetalResourceStorage::Private};
        RenderMemoryCompatibilityId compatibility;
        std::size_t backingBytes{0};
        std::size_t liveResources{0};
    };

    /** @brief Selects storage for a generic buffer without assuming unified memory. */
    [[nodiscard]] MetalResourceStorage MetalBufferStorage(const RenderBufferDescriptor &descriptor) noexcept;

    /** @brief Selects private resident storage for generic textures. */
    [[nodiscard]] MetalResourceStorage MetalTextureStorage(const RenderTextureDescriptor &descriptor) noexcept;

    /** @brief Converts exact native buffer requirements into a pool-compatible cost plan. */
    [[nodiscard]] Result<RenderMemoryCostPlan> PlanMetalBufferMemory(const RenderBufferDescriptor &descriptor,
                                                                     MetalResourceRequirements requirements);

    /** @brief Converts exact native texture requirements into a pool-compatible cost plan. */
    [[nodiscard]] Result<RenderMemoryCostPlan> PlanMetalTextureMemory(const RenderTextureDescriptor &descriptor,
                                                                      MetalResourceRequirements requirements);

    /** @brief Plans an aligned staging layout for one tightly packed texture payload. */
    [[nodiscard]] Result<MetalTextureUploadLayout> PlanMetalTextureUpload(const RenderTextureDescriptor &descriptor,
                                                                          std::size_t rowAlignment);

    /** @brief Validates the typed buffer bindings required by a Metal mesh. */
    [[nodiscard]] Result<void> ValidateMetalMeshBindings(const RenderMeshDescriptor &descriptor, RenderBufferUsage vertexUsage,
                                                         RenderBufferUsage indexUsage);

    /** @brief Reports whether a backend-neutral aspect is compatible with a Metal texture format. */
    [[nodiscard]] bool MetalTextureAspectMatches(RenderTextureFormat format, RenderTextureAspect aspect) noexcept;

    /** @brief Validates an admitted placement against the exact Metal requirement plan. */
    [[nodiscard]] Result<void> ValidateMetalResourcePlacement(const RenderMemoryCostPlan &plan, const RenderMemoryPlacement &placement);

    /** @brief Validates that a placement can reuse an existing Metal heap. */
    [[nodiscard]] Result<void> ValidateMetalHeapReuse(const MetalHeapState &heap, const RenderMemoryPlacement &placement,
                                                      MetalResourceStorage storage);

    /** @brief Adds one live resource to a validated heap without counter wrap. */
    [[nodiscard]] Result<void> RetainMetalHeapResource(MetalHeapState &heap);

    /** @brief Retires one live heap resource and reports whether the heap became empty. */
    [[nodiscard]] bool ReleaseMetalHeapResource(MetalHeapState &heap) noexcept;
}  // namespace Horo::Render::Detail
