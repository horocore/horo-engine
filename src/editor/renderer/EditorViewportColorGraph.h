#pragma once
/** @file EditorViewportColorGraph.h @brief Fixed borrowed graph records appended to the actual viewport scene graph. */
#include "EditorViewportColorPipeline.h"
#include "Horo/Runtime/Render/RenderGraphSynchronization.h"

namespace Horo::Editor {
    /** @brief Owns fixed draw/read/workload borrows for one cold graph variant; it never submits a frame. */
    class EditorViewportColorGraph final {
    public:
        /** @brief Appends meter and analytic output to the same graph as the unexposed scene.
         * @param builder Existing scene graph. @param pipeline Prepared actual color owner. @param queue Actual graphics queue.
         * @param initial Existing bounded imported-state inventory. @param scene Existing scene graph resource.
         * @param display Existing display-linear graph resource. @param displayView Exact display attachment.
         * @param after Existing lit-scene pass. @param extent Exact scene extent.
         * @return Actual analytic pass that must precede overlays, or original graph failure. */
        [[nodiscard]] Result<Render::RenderGraphPassRef> AppendScene(
            Render::RenderGraphBuilder &builder, const EditorViewportColorPipeline &pipeline, Render::RenderQueueId queue,
            std::vector<Render::RenderGraphImportedState> &initial, Render::RenderGraphResourceId scene,
            Render::RenderGraphResourceId display, Render::RenderTextureViewHandle displayView, Render::RenderGraphPassRef after,
            Render::FramebufferExtent extent);
        /** @brief Appends SDR encode after all display-linear overlays. @param builder Same scene builder.
         * @param pipeline Same prepared owner. @param output Existing final graph attachment. @param outputView Exact final view.
         * @param after Final overlay pass. @return Actual final encode pass or original failure. */
        [[nodiscard]] Result<Render::RenderGraphPassRef> AppendEncode(Render::RenderGraphBuilder &builder,
                                                                      const EditorViewportColorPipeline &pipeline,
                                                                      Render::RenderGraphResourceId output,
                                                                      Render::RenderTextureViewHandle outputView,
                                                                      Render::RenderGraphPassRef after);

        /** @brief Returns fixed workload borrows for insertion into the owning frame's ordered record inventory. */
        [[nodiscard]] std::span<const Render::RenderGraphPassWorkload, 3> Workloads() const noexcept {
            return workloads_;
        }

        /** @brief Refreshes only metadata/uniform leases after a reused graph's cold topology was matched. @param pipeline Prepared owner.
         */
        void Refresh(EditorViewportColorPipeline &pipeline) noexcept;
        /** @brief Compares exact previously imported state evidence against the owner's accepted graph history. @param pipeline Actual
         * owner.
         * @return True only when the cached topology's initial states remain exact. */
        [[nodiscard]] bool MatchesInitialStates(const EditorViewportColorPipeline &pipeline) const noexcept;

    private:
        [[nodiscard]] Render::RenderGraphResourceId Buffer(Render::RenderBufferHandle handle);
        [[nodiscard]] Render::RenderGraphResourceId Table(const EditorViewportColorPipeline &pipeline, std::size_t index);
        void Use(Render::RenderGraphPassRef pass, Render::RenderGraphResourceId resource, Render::RenderGraphAccess access,
                 Render::RenderGraphUsageKind kind);
        void Raster(const EditorViewportColorPipeline &pipeline, std::size_t role, Render::RenderGraphPassRef pass,
                    Render::RenderGraphResourceId color, Render::RenderTextureViewHandle view);
        [[nodiscard]] Render::RenderGraphResourceId RasterTexture(const EditorViewportColorPipeline &pipeline, std::size_t role,
                                                                  const Render::MaterialResourceBinding &binding);
        void RasterInputs(const EditorViewportColorPipeline &pipeline, std::size_t role, Render::RenderGraphPassRef pass);
        const EditorViewportColorPipeline *pipeline_{};
        std::array<Render::RenderGraphLogicalState, 8> bufferInitial_;
        Render::RenderGraphLogicalState tableInitial_;
        Render::RenderGraphBuilder *builder_{};
        std::vector<Render::RenderGraphImportedState> *initial_{};
        Render::RenderQueueId queue_;
        Render::FramebufferExtent extent_;
        Render::RenderGraphResourceId scene_;
        Render::RenderGraphResourceId display_;
        Render::RenderGraphResourceId exposure_;
        std::array<Render::RenderGraphPassWorkload, 3> workloads_;
        std::array<std::array<Render::RenderGraphMaterialDraw, 1>, 2> draws_;
        std::array<std::array<Render::RenderGraphMaterialRead, 6>, 2> reads_;
        std::array<std::pair<Render::RenderBufferHandle, Render::RenderGraphResourceId>, 8> buffers_;
        std::size_t bufferCount_{};
    };
}  // namespace Horo::Editor
