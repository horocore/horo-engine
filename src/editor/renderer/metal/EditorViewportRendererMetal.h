#pragma once

#include "editor/renderer/EditorViewportRenderer.h"
#include "runtime/renderer/modules/metal/MetalBackendModule.h"

#include <memory>

namespace Horo::Editor {
    /** @brief Metal editor adapter that renders backend-neutral editor scene instances into an offscreen target. */
    class EditorViewportRendererMetal final : public IEditorViewportRenderer {
    public:
        /** @brief Borrows the frontend resource owner and initialized runtime Metal bridge used by editor composition. */
        EditorViewportRendererMetal(Render::RenderFrontend &frontend, Render::MetalEditorGraphicsBridge &graphicsBridge) noexcept;
        ~EditorViewportRendererMetal() override;

        EditorViewportRendererMetal(const EditorViewportRendererMetal &) = delete;
        EditorViewportRendererMetal &operator=(const EditorViewportRendererMetal &) = delete;

        /** @brief Creates Metal pipeline and depth state; mesh resources are uploaded on first use. */
        [[nodiscard]] Result<void> Initialize();

        /** @brief Releases all Metal objects; repeated calls are safe. */
        void Shutdown() noexcept;

        [[nodiscard]] Result<void> ExecutePreparedFrame(Render::RenderFrameScope &frame,
                                                        std::span<const Render::RenderPassDescriptor> passes) override;
        void RequestExtent(EditorViewportExtent extent) noexcept override;
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
