#pragma once

/**
 * @file EditorViewportRenderer.h
 * @brief Backend-neutral editor viewport presentation contract.
 */

#include "EditorRendererErrors.h"
#include "EditorViewportColorTypes.h"
#include "EditorViewportLighting.h"
#include "EditorViewportTextureView.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/DirectionalShadow.h"
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

        /** @brief Activates only explicit genuine cooked color inputs at cold startup. @param inputs Exact decoded generation.
         * @return Typed unsupported on implementations without the actual complete color executor. */
        [[nodiscard]] virtual Result<void> PrepareColorPipeline(const EditorViewportColorInputs &inputs) {
            static_cast<void>(inputs);
            return Result<void>::Failure(MakeError(Render::ExposureGpuErrors::Unsupported));
        }

        /** @brief Selects exact authored exposure settings. @param settings Next settings. @return Admission result. */
        [[nodiscard]] virtual Result<void> RequestColorSettings(const Render::ExposureSettings &settings) {
            static_cast<void>(settings);
            return Result<void>::Failure(MakeError(Render::ExposureGpuErrors::Unsupported));
        }

        /** @brief Supplies actual normalized host clock delta before the existing pre-frame preparation. @param deltaSeconds Selected view
         * seconds. */
        virtual void SetColorFrameDelta(float deltaSeconds) noexcept {
            static_cast<void>(deltaSeconds);
        }

        /** @brief Finishes a prepared candidate only after actual native Present, or cancels it. @param accepted Actual frame acceptance.
         */
        virtual void FinishColorFrame(bool accepted) noexcept {
            static_cast<void>(accepted);
        }

        /** @brief Observes explicit pending/unavailable/reset lifecycle metadata. @return Bounded typed status, no fabricated EV. */
        [[nodiscard]] virtual EditorViewportColorDiagnostics ColorDiagnostics() const noexcept {
            return {};
        }

        /** @brief Requests a validated shadow budget for the next resource boundary.
         * @param profile Explicit product quality, filtering and atlas budget.
         * @return Admission result; the previous complete profile remains active on failure.
         * @details A request cannot mutate an in-flight atlas or silently select another renderer.
         */
        [[nodiscard]] virtual Result<void> RequestDirectionalShadowProfile(const Render::DirectionalShadowProfile &profile) {
            static_cast<void>(profile);
            return Result<void>::Failure(MakeError(RendererErrors::ViewportShadowUnsupported));
        }

        /** @brief Requests the exact frontend/host-admitted lighting recipe at an owner safe point.
         * @param profile Authored budgets and fallback permission.
         * @param selection Exact selected operation and explicit missing-predicate diagnostics.
         * @return Admission result; unsupported implementations preserve their previous ready state. */
        [[nodiscard]] virtual Result<void> RequestLightingProfile(const EditorViewportLightingProfile &profile,
                                                                  const Render::LightPreparationSelection &selection) {
            static_cast<void>(profile);
            static_cast<void>(selection);
            return Result<void>::Failure(MakeError(Render::LightCullingErrors::Unsupported));
        }

        /** @brief Observes exact admitted operation and bounded-degradation counts without allocating or mutating renderer state. */
        [[nodiscard]] virtual EditorViewportLightingDiagnostics LightingDiagnostics() const noexcept {
            return {};
        }

        /** @brief Binds the identity issued by the actual frontend once when this viewport owner is created. */
        [[nodiscard]] Result<void> BindLightingViewIdentity(const Render::RenderViewId view) {
            if (!view.IsValid() || lightingView_.IsValid())
                return Result<void>::Failure(MakeError(Render::LightCullingErrors::InvalidInput));
            lightingView_ = view;
            return Result<void>::Success();
        }

        /** @brief Returns the frontend-scoped view identity retained across this viewport's frames. */
        [[nodiscard]] Render::RenderViewId LightingViewIdentity() const noexcept {
            return lightingView_;
        }

        /** @brief Prepares the final frozen scene after GUI extraction and before native BeginFrame.
         * @param pass Exact immutable descriptor consumed by the subsequent frame.
         * @param view Identity issued once for the actual viewport owner.
         * @return Preparation failure before any frame begins; successful implementations retain only owned data and native leases. */
        [[nodiscard]] virtual Result<void> PrepareLightingFrame(const Render::StaticMeshPassDescriptor &pass,
                                                                const Render::RenderViewId view) {
            static_cast<void>(pass);
            static_cast<void>(view);
            return Result<void>::Success();
        }

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

    private:
        Render::RenderViewId lightingView_;
    };
}  // namespace Horo::Editor
