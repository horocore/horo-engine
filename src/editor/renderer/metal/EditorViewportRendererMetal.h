#pragma once

#include "Horo/Runtime/Render/LightCullingKernel.h"
#include "Horo/Runtime/Render/ShaderHotReloadTarget.h"
#include "editor/renderer/EditorViewportRenderer.h"
#include "runtime/renderer/modules/metal/MetalBackendModule.h"

#include <memory>

namespace Horo::Editor {
    /** @brief Metal editor adapter that renders backend-neutral editor scene instances into an offscreen target. */
    class EditorViewportRendererMetal final : public IEditorViewportRenderer, public Render::IShaderHotReloadTarget {
    public:
        /** @brief Borrows the frontend resource owner and initialized runtime Metal bridge used by editor composition. */
        EditorViewportRendererMetal(Render::RenderFrontend &frontend, Render::MetalEditorGraphicsBridge &graphicsBridge) noexcept;
        ~EditorViewportRendererMetal() override;

        EditorViewportRendererMetal(const EditorViewportRendererMetal &) = delete;
        EditorViewportRendererMetal &operator=(const EditorViewportRendererMetal &) = delete;

        /** @brief Creates Metal pipeline and depth state; mesh resources are uploaded on first use. */
        [[nodiscard]] Result<void> Initialize();

        /** @brief Admits the complete actual cooked clustered family and culling kernel on the cold graphics owner.
         * @details All borrowed manifests and batches expire on return; failed preparation retains the last accepted family. */
        /** @brief Creates the actual CPU-forward-selected raster graph from a complete cooked generic family. */
        [[nodiscard]] Result<void> PrepareBaselineLighting(std::span<const Render::ShaderHotReloadProgramView> family,
                                                           const EditorViewportLightingProfile &profile);
        [[nodiscard]] Result<void> PrepareClusteredLighting(std::span<const Render::ShaderHotReloadProgramView> family,
                                                            const Render::CookedLightCullingKernel &kernel,
                                                            const EditorViewportLightingProfile &profile);

        /** @brief Releases all Metal objects; repeated calls are safe. */
        void Shutdown() noexcept;
        [[nodiscard]] Result<Render::ShaderHotReloadTargetState> CaptureState() const override;
        [[nodiscard]] Result<std::unique_ptr<Render::IShaderHotReloadCandidate>> Prepare(
            std::span<const Render::ShaderHotReloadProgramView> programs, Render::ShaderHotReloadTargetState expected,
            const CancellationToken &cancellation = {}) override;
        [[nodiscard]] Result<Render::ShaderHotReloadTargetState> Adopt(std::unique_ptr<Render::IShaderHotReloadCandidate> candidate,
                                                                       Render::ShaderHotReloadTargetState expected) override;

        /** @copydoc IEditorViewportRenderer::PrepareColorPipeline */
        [[nodiscard]] Result<void> PrepareColorPipeline(const EditorViewportColorInputs &inputs) override;
        /** @copydoc IEditorViewportRenderer::RequestColorSettings */
        [[nodiscard]] Result<void> RequestColorSettings(const Render::ExposureSettings &settings) override;
        /** @copydoc IEditorViewportRenderer::SetColorFrameDelta */
        void SetColorFrameDelta(float deltaSeconds) noexcept override;
        /** @copydoc IEditorViewportRenderer::FinishColorFrame */
        void FinishColorFrame(bool accepted) noexcept override;
        /** @copydoc IEditorViewportRenderer::ColorDiagnostics */
        [[nodiscard]] EditorViewportColorDiagnostics ColorDiagnostics() const noexcept override;
        [[nodiscard]] Result<void> ExecutePreparedFrame(Render::RenderFrameScope &frame,
                                                        std::span<const Render::RenderPassDescriptor> passes) override;
        void RequestExtent(EditorViewportExtent extent) noexcept override;
        [[nodiscard]] Result<void> RequestLightingProfile(const EditorViewportLightingProfile &profile,
                                                          const Render::LightPreparationSelection &selection) override;
        [[nodiscard]] EditorViewportLightingDiagnostics LightingDiagnostics() const noexcept override;
        [[nodiscard]] Result<void> PrepareLightingFrame(const Render::StaticMeshPassDescriptor &pass, Render::RenderViewId view) override;

        [[nodiscard]] Result<void> RequestDirectionalShadowProfile(const Render::DirectionalShadowProfile &profile) override;

        void RequestGrid(const EditorViewportGridOptions &options) noexcept override;
        void RequestLightVisualizer(const EditorViewportLightVisualizerOptions &options) noexcept override;
        [[nodiscard]] EditorViewportExtent RequestedExtent() const noexcept override;
        [[nodiscard]] Math::ClipDepthRange ClipDepthRange() const noexcept override;
        [[nodiscard]] Result<void> ExecuteStaticMeshPass(const Render::StaticMeshPassDescriptor &descriptor) override;
        [[nodiscard]] Result<std::optional<Render::RenderTargetHandle>> PrepareResources(Render::RenderFrontend &frontend,
                                                                                         const Render::RenderSceneView &scene) override;
        [[nodiscard]] EditorViewportTextureView TextureView() const noexcept override;
        [[nodiscard]] bool IsReady() const noexcept override;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Editor
