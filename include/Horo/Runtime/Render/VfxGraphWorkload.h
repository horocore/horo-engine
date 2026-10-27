#pragma once
/** @file VfxGraphWorkload.h
 * @brief Retained typed VFX graph payloads and immutable per-view instance ordering.
 */
#include "Horo/Runtime/Render/RenderGraph.h"
#include "Horo/Runtime/Render/RenderWorldSnapshot.h"

namespace Horo::Render {
    class PreparedVfxFrameSlot;
    /** @brief Existing global scene draw-call ceiling, shared by host admission and cold native encoding storage. */
    inline constexpr std::uint32_t MaximumVfxGraphDraws = 12'288;
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
        std::uint32_t batch{};
        std::uint32_t instance{};
        std::uint32_t depthKey{};
    };

    /** @brief One exact contiguous canonical-order chunk, retained through the existing graph completion lease. */
    struct VfxPreparedDraw final {
        std::uint32_t firstOrder{};
        std::uint32_t instanceCount{};
        RenderMeshHandle mesh;
        RenderGraphResourceId vertices;
        RenderGraphResourceId indices;
        RenderGraphResourceId parameters;
        RenderGraphResourceId sampledTexture; /**< Optional single baseline color sample, explicitly declared in the graph. */
        std::uint64_t revision{};
        std::shared_ptr<PreparedVfxFrameSlot> slot; /**< Public methods are read-only; shared ownership blocks further publication. */
    };

    /** @brief Immutable renderer-owned view order, independent of CPU simulation and snapshot packing order. */
    struct VfxDrawOrder final {
        std::uint64_t view{};
        std::uint64_t viewGeneration{};
        std::vector<VfxDrawReference> instances;
        std::vector<VfxPreparedDraw> draws; /**< Preparation-reserved capacity; only the renderer populates issued orders. */
        bool nativePrepared{false};         /**< Complete native binding transaction; empty/partial plans never imply execution. */
        bool preparingDraws{false};         /**< Synchronous owner-only transaction; recursive preparation is rejected. */
    };

    /** @brief Actual retained particle payload for one semantic graph pass.
     * @details Shadow/depth/GBuffer/opaque write depth; translucent/additive test completed depth read-only.
     * Color is unexposed scene-linear. Physical execution is explicitly unavailable until native VFX encoding is implemented.
     */
    struct RenderGraphVfx final {
        VfxGraphStage stage{VfxGraphStage::AdditiveForward};
        RenderGraphResourceId color{};
        RenderGraphResourceId depth{};
        std::shared_ptr<const VfxDrawOrder> order;
        RenderWorldSnapshot snapshot; /**< Exact source lease paired with this order, released with the workload. */
        RenderTargetHandle target;    /**< Optional plan-only; native forward execution requires the exact prepared color/depth target. */
    };

    /** @brief Exact frontend-resolved native target, pinned through the same graph completion lease. */
    struct RenderGraphTargetInstance final {
        RenderTargetHandle target;
        std::uint64_t instance{};
    };
}  // namespace Horo::Render
