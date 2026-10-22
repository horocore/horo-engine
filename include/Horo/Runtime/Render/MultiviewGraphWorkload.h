#pragma once
/** @file MultiviewGraphWorkload.h
 * @brief Canonical indexed multiview draw workload using graph resources and existing material leases.
 */
#include "Horo/Runtime/Render/MultiviewSceneExtraction.h"
#include "Horo/Runtime/Render/RenderGraphExecution.h"

#include <memory>
#include <span>

namespace Horo::Render {
    class ResidentMaterialBinding;
    struct RenderGraphExecutionRequest;

    /** @brief Complete per-view render rectangle within an exact imported attachment layer. */
    struct MultiviewRenderRectangle final {
        std::uint32_t x{}, y{}, width{}, height{};
        /** @brief Checks non-empty overflow-safe containment. @param allocation Native attachment extent.
         * @return True when the active rectangle fits. */
        [[nodiscard]] bool Fits(FramebufferExtent allocation) const noexcept;
        auto operator<=>(const MultiviewRenderRectangle &) const noexcept = default;
    };

    /** @brief One ordered view's graph-local color/depth and exact image subresource mapping.
     * @details The image-import owner supplies these graph identities after validating the acquired-image lease.
     * Naming an imported graph resource does not itself import, retain or release a foreign native image.
     */
    struct MultiviewGraphTarget final {
        RenderGraphResourceId color;
        RenderGraphResourceId depth;       /**< Invalid means no depth attachment. */
        RenderTextureViewHandle colorView; /**< Exact prepared single-layer view of color. */
        RenderTextureViewHandle depthView; /**< Exact prepared single-layer view when depth is present. */
        MultiviewRenderRectangle rectangle;
        std::uint32_t arrayLayer{};
        std::uint32_t depthArrayLayer{}; /**< Depth mapping is independent of color; instanced routing requires agreement. */
    };

    /** @brief One visible canonical instance's ready resident mesh/material and exact graph buffer dependencies. */
    struct MultiviewGraphGeometry final {
        std::uint32_t instanceIndex{};
        RenderGraphResourceId vertices;
        RenderGraphResourceId indices;
        RenderGraphResourceId parameters;
        std::shared_ptr<const ResidentMaterialBinding> material;
    };

    /** @brief Explicit primitive-work admission in addition to instance/view/draw-count bounds. */
    struct MultiviewNativeWorkLimits final {
        std::uint32_t maximumIndicesPerDraw{1'048'576};
        std::uint64_t maximumExpandedIndices{16'777'216};

        /** @brief Checks finite non-zero capacities against fixed ceilings. @return True for admitted limits. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maximumIndicesPerDraw > 0 && maximumIndicesPerDraw <= 16'777'216 && maximumExpandedIndices > 0 &&
                   maximumExpandedIndices <= 536'870'912;
        }
    };

    /** @brief One atomic bounded whole-frame scene workload; all admission precedes encoding any view.
     * @details All spans are borrowed only for synchronous graph admission. The canonical frontend completion lease
     * retains the exact meshes/materials; native command retention uses HORO-326's existing ownership and timelines.
     * Color/depth are cleared exactly once per view and stored; this operation does not publish XR completion itself.
     */
    struct RenderGraphMultiview final {
        std::shared_ptr<const MultiviewSceneSnapshot> scene;
        MultiviewPublication active;
        std::span<const MultiviewGraphTarget> targets;
        std::span<const MultiviewGraphGeometry> geometry;
        ClearColor clearColor{0.035F, 0.045F, 0.070F, 1.0F};
        float depthClear{1.0F};
        MultiviewNativeWorkLimits limits;
    };

    /** @brief Exact frontend-resolved resident mesh generation, additional to the graph's typed buffer bindings. */
    struct RenderGraphMeshInstance final {
        RenderMeshHandle mesh;
        std::uint64_t instance{};
    };

    /** @brief Exact frontend-resolved attachment-view generation, retained through the same graph completion lease. */
    struct RenderGraphTextureViewInstance final {
        RenderTextureViewHandle view;
        std::uint64_t instance{};
    };

    /** @brief Validates publication, ordered targets, draw generations and exhaustive declared semantic graph uses.
     * @param request Complete canonical graph request.
     * @param pass Exact retained graphics pass.
     * @param workload Bounded borrowed multiview scene submission.
     * @return Success or typed invalid/stale/capacity/unsupported failure, before any native state changes. */
    [[nodiscard]] Result<void> ValidateMultiviewGraphWorkload(const RenderGraphExecutionRequest &request,
                                                              const RenderGraphExecutionPass &pass, const RenderGraphMultiview &workload);
}  // namespace Horo::Render
