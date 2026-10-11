#pragma once
/** @file VfxGraphWorkload.h
 * @brief Retained typed VFX graph payloads and immutable per-view instance ordering.
 */
#include "Horo/Runtime/Render/RenderGraph.h"
#include "Horo/Runtime/Render/RenderWorldSnapshot.h"

namespace Horo::Render {
    /** @brief Semantic recipe stage; the renderer owns mapping, never effect data. */
    enum class VfxGraphStage : std::uint8_t {
        Shadow,
        Depth,
        OpaqueForward,
        GBuffer,
        TranslucentForward,
        AdditiveForward
    };

    /** @brief Exact retained instance reference, including renderer-computed canonical sort depth. */
    struct VfxDrawReference final {
        std::uint32_t batch{}, instance{}, depthKey{};
    };

    /** @brief Immutable renderer-owned view order, independent of CPU simulation and snapshot packing order. */
    struct VfxDrawOrder final {
        std::uint64_t view{}, viewGeneration{};
        std::vector<VfxDrawReference> instances;
    };

    /** @brief Actual retained particle payload for one semantic graph pass.
     * @details Shadow/depth/GBuffer/opaque write depth; translucent/additive test completed depth read-only.
     * Color is unexposed scene-linear. Physical execution is explicitly unavailable until native VFX encoding is implemented.
     */
    struct RenderGraphVfx final {
        VfxGraphStage stage{VfxGraphStage::AdditiveForward};
        RenderGraphResourceId color{}, depth{};
        std::shared_ptr<const VfxDrawOrder> order;
        RenderWorldSnapshot snapshot; /**< Exact source lease paired with this order, released with the workload. */
    };
}  // namespace Horo::Render
