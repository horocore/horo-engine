#pragma once
/** @file OpenXRMultiviewGraph.h
 * @brief Non-installed XR acquisition-to-canonical-render-graph host composition.
 */
#include "Horo/Runtime/Render/Mesh.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "Horo/XR/XRMultiviewScene.h"
#include "OpenXRSwapchain.h"

#include <memory>

namespace Horo::Render {
    class RenderFrontend;
    class RenderFrameScope;
}  // namespace Horo::Render

namespace Horo::XR::OpenXRInternal {
    /** @brief Ready canonical mesh dependencies and material, in visible draw order. */
    struct OpenXRMultiviewGeometry final {
        std::uint32_t instanceIndex{};
        Render::RenderMeshDescriptor mesh;
        std::shared_ptr<const Render::ResidentMaterialBinding> material;
    };

    /** @brief Already prepared single-layer attachment views in runtime view order. */
    struct OpenXRMultiviewTargetViews final {
        Render::RenderTextureViewHandle color;
        Render::RenderTextureViewHandle depth;
    };

    /** @brief Complete borrowed creation inputs; limits remain part of the same admitted request. */
    struct OpenXRMultiviewGraphRequest final {
        const XRMultiviewScene &scene;
        const XRMultiviewOwnerState &active;
        std::span<OpenXRSwapchain *const> owners;
        std::span<const OpenXRMultiviewGeometry> geometry;
        std::span<const OpenXRMultiviewTargetViews> views;
        Render::RenderQueueId queue;
        Render::MultiviewNativeWorkLimits limits{};
    };

    /** @brief Prepared host graph borrowing exact swapchain owners through synchronous admission.
     * @details Create allocates bounded CPU graph storage outside an active frame. Meshes, material
     * parameter buffers and attachment views must already be ready. Caller retains their logical handles
     * through Execute. The frontend's existing completion lease then pins all native backing.
     * No XR frame, queue, timeline, image release, native allocation or shutdown authority is added.
     * All methods and destruction run on the frontend/control owner thread. Swapchain owners and frontend
     * outlive this object. Destruction does not release an acquisition; the host follows PollRelease/Close.
     */
    class OpenXRMultiviewGraph final {
    public:
        ~OpenXRMultiviewGraph();
        OpenXRMultiviewGraph(OpenXRMultiviewGraph &&) noexcept;
        OpenXRMultiviewGraph &operator=(OpenXRMultiviewGraph &&) noexcept;
        OpenXRMultiviewGraph(const OpenXRMultiviewGraph &) = delete;
        OpenXRMultiviewGraph &operator=(const OpenXRMultiviewGraph &) = delete;
        /** @brief Resolves current acquisitions and compiles one complete graphics pass.
         * @param frontend Actual selected resource/device owner. @param request Complete ready borrowed inputs and work limits.
         * @return Prepared graph or typed owner/resource/stale/allocation failure without native work.
         */
        [[nodiscard]] static Result<OpenXRMultiviewGraph> Create(Render::RenderFrontend &frontend,
                                                                 const OpenXRMultiviewGraphRequest &request);
        /** @brief Convenience composition with default finite work limits; all input owners remain borrowed. */
        [[nodiscard]] static Result<OpenXRMultiviewGraph> Create(Render::RenderFrontend &frontend, const XRMultiviewScene &scene,
                                                                 const XRMultiviewOwnerState &active,
                                                                 std::span<OpenXRSwapchain *const> owners,
                                                                 std::span<const OpenXRMultiviewGeometry> geometry,
                                                                 std::span<const OpenXRMultiviewTargetViews> views,
                                                                 Render::RenderQueueId queue);
        /** @brief Revalidates actual owner acquisitions and executes through the canonical submission API.
         * @param frame Exact active frontend frame. @param active Current owner/publication facts.
         * @param submission Host scheduler's actual queue order, waits and signal.
         * @return Admission/encoding result. Success is submitted work, never GPU/XR completion evidence.
         */
        [[nodiscard]] Result<void> Execute(Render::RenderFrameScope &frame, const XRMultiviewOwnerState &active,
                                           const Render::RenderQueueSubmission &submission);
        /** @brief Returns immutable compiled evidence for inspection. @return Exact prepared plan. */
        [[nodiscard]] const Render::CompiledRenderGraphExecution &Graph() const noexcept;

    private:
        /** @brief Releases compiled CPU graph storage before its borrowed-input metadata, without releasing any XR acquisition. */
        void Reset() noexcept;
        struct Impl;
        explicit OpenXRMultiviewGraph(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::XR::OpenXRInternal
