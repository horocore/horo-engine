#pragma once

/**
 * @file EditorViewportRenderer.h
 * @brief Backend-neutral editor viewport presentation contract.
 */

#include "EditorViewportTextureView.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/RenderBackend.h"
#include "editor/renderer/EditorViewportScene.h"

#include <optional>

namespace Horo::Render {
    class RenderFrontend;
    class RenderFrameScope;
}  // namespace Horo::Render

namespace Horo::Editor {
    /** @brief Pixel extent requested by the editor viewport panel. */
    struct EditorViewportExtent {
        std::uint32_t width{0};
        std::uint32_t height{0};

        /** @brief Reports whether the extent can back a render target. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return width > 0 && height > 0;
        }

        [[nodiscard]] constexpr bool operator==(const EditorViewportExtent &) const noexcept = default;
    };

    /** @brief Per-frame editor grid presentation options requested by the viewport panel. */
    struct EditorViewportGridOptions {
        bool visible{true};
        float targetMinorSpacingPixels{48.0F};
        float targetLineWidthPixels{1.5F};
    };

    /** @brief Selected Light influence visualizer requested by the viewport panel. */
    struct EditorViewportLightVisualizerOptions {
        std::optional<Render::RenderLight> selectedLight;
    };

    /**
     * @brief Editor-private adapter that renders one viewport image with the selected graphics backend.
     *
     * Implementations are selected by the editor composition root together with the renderer backend.
     * The returned texture identity is consumed only by the matching editor GUI renderer bridge; it is
     * not a public RHI resource handle. Calls are serialized on the render-capable UI thread.
     */
    class IEditorViewportRenderer : public Render::IStaticMeshPassExecutor {
    public:
        virtual ~IEditorViewportRenderer() = default;

        /** @brief Executes one prepared editor frame through the existing frontend frame/submission owner.
         * @param frame Exact active frontend scope. @param passes Frozen borrowed scene/output descriptors.
         * @return Original execution failure; no second frame, backend selection or submission authority is created.
         * @details Borrowed GL/Metal peers forward to the current static-mesh recipe; explicit peers use the same
         * frame's material/graph operations. GUI composition and presentation remain subsequent host phases. */
        [[nodiscard]] virtual Result<void> ExecutePreparedFrame(Render::RenderFrameScope &frame,
                                                                std::span<const Render::RenderPassDescriptor> passes) = 0;

        /**
         * @brief Synchronizes backend-neutral viewport resources at a frame boundary.
         * @param frontend Selected frontend that owns resident resource identity.
         * @param scene Synchronously borrowed CPU source snapshot.
         * @return Newly ready viewport target when this integration owns one, otherwise no replacement.
         */
        [[nodiscard]] virtual Result<std::optional<Render::RenderTargetHandle>> PrepareResources(Render::RenderFrontend &frontend,
                                                                                                 const Render::RenderSceneView &scene) {
            static_cast<void>(frontend);
            static_cast<void>(scene);
            return Result<std::optional<Render::RenderTargetHandle>>::Success(std::nullopt);
        }

        /**
         * @brief Records the panel's desired render-target extent for the current frame and next resource boundary.
         * @param extent Physical-pixel extent, or an invalid extent when the panel cannot render.
         */
        virtual void RequestExtent(EditorViewportExtent extent) noexcept = 0;

        /** @brief Records the panel's world-grid presentation options for the current frame. */
        virtual void RequestGrid(const EditorViewportGridOptions &options) noexcept = 0;

        /** @brief Records the selected Light visualizer requested for the next viewport pass. */
        virtual void RequestLightVisualizer(const EditorViewportLightVisualizerOptions &options) noexcept = 0;

        /** @brief Returns the active target extent when this frame has a panel request, otherwise an invalid extent. */
        [[nodiscard]] virtual EditorViewportExtent RequestedExtent() const noexcept = 0;

        /** @brief Returns the clip-depth convention used by this renderer's viewport projection. */
        [[nodiscard]] virtual Math::ClipDepthRange ClipDepthRange() const noexcept = 0;

        /** @brief Returns the matching GUI bridge texture view, including backend-specific orientation. */
        [[nodiscard]] virtual EditorViewportTextureView TextureView() const noexcept = 0;

        /** @brief Reports whether a complete viewport image is available for presentation. */
        [[nodiscard]] virtual bool IsReady() const noexcept = 0;
    };
}  // namespace Horo::Editor
