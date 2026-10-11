#pragma once

/** @file RenderWorldSnapshot.h
 * @brief Immutable frame-owned CPU VFX output shared with renderer graph scheduling.
 */
#include "Horo/Runtime/Render/StandardPbrMaterial.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace Horo::Vfx {
    class VfxRenderExtractor;
}

namespace Horo::Render {
    /** @brief Cooked semantic blend class; contains no recipe or backend selection. */
    enum class VfxParticlePass : std::uint8_t {
        Opaque,
        Masked,
        Translucent,
        Additive,
        Count
    };
    /** @brief Cooked geometry interpretation of packed instances. */
    enum class VfxPrimitiveTopology : std::uint8_t {
        CameraFacingBillboard,
        RibbonStrip,
        InstancedMesh,
        Count
    };

    /** @brief Stable scene/effect/emitter identity retained independently of live simulation storage. */
    struct VfxBatchId final {
        std::uint64_t scene{};
        std::uint32_t effectSlot{}, effectGeneration{}, emitterSlot{}, emitterGeneration{};
        auto operator<=>(const VfxBatchId &) const noexcept = default;
    };

    /** @brief Packed immutable render-origin-relative instance; color remains scene-linear. */
    struct VfxRenderInstance final {
        std::uint64_t particle{}; /**< Stable simulation identity, never a dense storage index. */
        Math::Vec3 position{};
        Math::Vec2 size{};
        float rotation{};
        std::uint32_t packedColor{};
    };

    /** @brief Contiguous complete-emitter slice, validated before snapshot publication. */
    struct VfxRenderBatch final {
        VfxBatchId id{};
        MaterialRuntimeId material{};
        RenderMeshHandle mesh{}; /**< Required for InstancedMesh; unused for billboard/ribbon. */
        VfxParticlePass blend{VfxParticlePass::Additive};
        VfxPrimitiveTopology topology{VfxPrimitiveTopology::CameraFacingBillboard};
        bool deferredCompatible{}; /**< Cooked material supports the selected GBuffer contract. */
        bool shadowCaster{};       /**< Only opaque/masked mesh may request the standard shadow pass. */
        std::uint32_t firstInstance{}, instanceCount{};
    };

    /** @brief Preallocated storage; only extraction may populate an unleased slot. */
    class RenderWorldFrameData final {
    private:
        friend class Horo::Vfx::VfxRenderExtractor;
        friend class RenderWorldSnapshot;
        std::uint64_t scene_{}, frame_{}, originRevision_{};
        std::vector<VfxRenderBatch> batches_;
        std::vector<VfxRenderInstance> instances_;
    };

    /** @brief Copyable immutable lease that survives simulation, scene and extractor destruction.
     * @details Slots are reused only after all leases release. Consumers may read copies concurrently;
     * copies of one shared_ptr object itself must be externally serialized. No live-world pointer is retained.
     */
    class RenderWorldSnapshot final {
    public:
        /** @brief Returns scene incarnation or zero for an empty lease. */
        [[nodiscard]] std::uint64_t Scene() const noexcept {
            return data_ ? data_->scene_ : 0;
        }

        /** @brief Returns one-based captured frame number or zero. */
        [[nodiscard]] std::uint64_t Frame() const noexcept {
            return data_ ? data_->frame_ : 0;
        }

        /** @brief Returns captured render-origin revision or zero. */
        [[nodiscard]] std::uint64_t OriginRevision() const noexcept {
            return data_ ? data_->originRevision_ : 0;
        }

        /** @brief Returns retained complete-emitter batches. @return Immutable frame-owned slice. */
        [[nodiscard]] std::span<const VfxRenderBatch> Batches() const noexcept {
            return data_ ? std::span{data_->batches_} : std::span<const VfxRenderBatch>{};
        }

        /** @brief Returns retained packed particle data. @return Immutable frame-owned slice. */
        [[nodiscard]] std::span<const VfxRenderInstance> Instances() const noexcept {
            return data_ ? std::span{data_->instances_} : std::span<const VfxRenderInstance>{};
        }

    private:
        friend class Horo::Vfx::VfxRenderExtractor;
        std::shared_ptr<const RenderWorldFrameData> data_;
    };
}  // namespace Horo::Render
