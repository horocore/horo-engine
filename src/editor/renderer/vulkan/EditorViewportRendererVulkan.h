#pragma once
/** @file EditorViewportRendererVulkan.h
 * @brief Selected Vulkan viewport using canonical cooked material, frontend resources and frame submission. */
#include "EditorViewportForwardLighting.h"
#include "EditorVulkanViewportShader.h"
#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/MultiviewMotionHistory.h"
#include "Horo/Runtime/Render/NativeReconstructionKernel.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/ShaderHotReloadTarget.h"
#include "Horo/Runtime/Render/UiPaintProgram.h"
#include "Horo/Runtime/Render/UiRenderComposition.h"
#include "VulkanEditorGraphicsBridge.h"
#include "editor/renderer/EditorGraphViewportImageCapture.h"
#include "editor/renderer/EditorViewportResources.h"
#include "editor/renderer/grid/ViewportOverlayTriangles.h"
#include "host/EditorVulkanXrViewportAccess.h"

#include <array>
#include <thread>

namespace Horo::Editor {
    class EditorViewportColorPipeline;

    class EditorViewportRendererVulkan final : public IEditorViewportRenderer,
                                               public Render::Detail::IVulkanEditorDeviceChild,
                                               public Render::IShaderHotReloadTarget {
    public:
        EditorViewportRendererVulkan(Render::RenderFrontend &frontend, Render::Detail::VulkanEditorGraphicsBridge &bridge,
                                     EditorVulkanViewportCook cooked) noexcept;
        /** @brief Installs the explicit validated scene-linear shader and Vulkan manual color package before resources or frames.
         * @param scene Exact scene-linear cook. @param inputs Selected decoded color package.
         * @param settings Authored manual exposure settings.
         * @param spatial Optional exact cook already verified by the explicit installed host inventory.
         * @return Success only after complete native material/color admission, or typed failure. */
        [[nodiscard]] Result<void> PrepareManualColor(EditorVulkanSceneLinearCook scene, const EditorViewportColorInputs &inputs,
                                                      const Render::ExposureSettings &settings,
                                                      const Render::CookedNativeReconstructionKernel *spatial = nullptr);

        /** @brief Prepares both guide shaders and their native pipelines as one cold viewport generation. */
        struct TemporalGuideCook final {
            std::span<const std::uint8_t> payload;
            std::span<const std::uint8_t> reflection;
        };

        /** @brief Admits both native guide pipelines before frame recording or publishing viewport targets.
         * @param linearDepth Cooked linear depth payload and its exact reflected layout.
         * @param motion Cooked motion payload and its exact reflected layout.
         * @return Success only when both pipelines and their material tables are ready. */
        [[nodiscard]] Result<void> PrepareTemporalGuidePrograms(const TemporalGuideCook &linearDepth, const TemporalGuideCook &motion);
        ~EditorViewportRendererVulkan() override;
        /** @copydoc IEditorViewportRenderer::RequestImageCapture */
        [[nodiscard]] Result<std::uint64_t> RequestImageCapture(EditorViewportImageRegion region,
                                                                std::function<bool()> authorized) override;
        /** @copydoc IEditorViewportRenderer::ReadImageCapture */
        [[nodiscard]] std::optional<EditorViewportImageSnapshot> ReadImageCapture(std::uint64_t id) override;
        /** @copydoc IEditorViewportRenderer::CancelImageCapture */
        bool CancelImageCapture(std::uint64_t id) noexcept override;
        /** @copydoc IEditorViewportRenderer::RequestColorSettings */
        [[nodiscard]] Result<void> RequestColorSettings(const Render::ExposureSettings &settings) override;
        /** @copydoc IEditorViewportRenderer::SetColorFrameDelta */
        void SetColorFrameDelta(float deltaSeconds) noexcept override;
        /** @copydoc IEditorViewportRenderer::FinishColorFrame */
        void FinishColorFrame(bool presented) noexcept override;
        /** @copydoc IEditorViewportRenderer::ColorDiagnostics */
        [[nodiscard]] EditorViewportColorDiagnostics ColorDiagnostics() const noexcept override;
        [[nodiscard]] Result<void> Initialize();

        /** @brief Borrows one canvas recipe during cold preparation. The renderer retains its own native resources. */
        struct ViewportCanvasPreparation final {
            const Render::UiPaintProgramDescriptor &paint;
            const Runtime::Ui::UiRenderSnapshot &prototype;
            std::span<const Render::RenderTextureHandle> textures;
            const Render::CookedUiPaintKernel &writer;
            const EditorVulkanWorldCanvasCook &consumer;
            Render::UiRenderCompositionSpace space;
            Render::RenderGraphLimits limits;
            bool overlay{};
        };

        /** @brief Prepares a sampled canvas and the exact ordinary viewport scene in one owned graph per frame slot.
         * @param pass Exact viewport target, dimensions and renderable mesh topology.
         * @param request Current UI prototype, validated shaders, composition space and graph limits for the selected viewport.
         * @param cancellation Cancelled source admission fence.
         * @return Exact preparation outcome. A failed replacement preserves the previous preparation while its inputs remain current.
         * @pre On the graphics owner, outside an active frame. The ordinary base output is required.
         */
        [[nodiscard]] Result<void> PrepareViewportCanvas(const Render::StaticMeshPassDescriptor &pass,
                                                         const ViewportCanvasPreparation &request,
                                                         const CancellationToken &cancellation = {});
        /** @brief Observes the selected device's scene canvas route independently of primary-output UI support. */
        [[nodiscard]] bool SupportsViewportCanvas() const noexcept;
        /** @brief Observes whether the selected base output supports a separate sampled canvas for the current frame. */
        [[nodiscard]] bool CanComposeViewportCanvas() const noexcept;
        /** @brief Releases a retired canvas recipe on the render owner lane before acquiring a frame. */
        void ReleaseViewportCanvas() noexcept;

        /** @brief Produces an exact submission for an already prepared renderer-owned program and acquired frame slot. */
        using ViewportCanvasSource = std::function<Result<Render::UiRenderSubmission>(const Render::UiPaintProgram &, std::uint32_t)>;

        /** @brief Encodes an already prepared canvas and viewport scene before GUI composition.
         * @param frame Exact active renderer frame.
         * @param pass Actual matching viewport scene with current cameras and meshes.
         * @param placement Current World-from-canvas matrix. Camera mode consumes no World placement.
         * @param source Synchronous borrower providing canonical UI geometry and its admitted bindings for the selected slot.
         * @return Scene execution result. Presentation and interactive receipts remain the host's responsibility.
         */
        [[nodiscard]] Result<void> ExecuteViewportCanvas(Render::RenderFrameScope &frame, const Render::StaticMeshPassDescriptor &pass,
                                                         const std::optional<Math::Mat4> &placement, const ViewportCanvasSource &source);
        /** @brief Uses ordinary viewport and primary-output admission with a canvas in its one scene graph.
         * @param frame Exact acquired frame. @param passes Ordinary prepared viewport and output descriptors.
         * @param placement Current World plane converted to renderer-local quad coordinates, if applicable.
         * @param source Exact per-slot canvas source. @return Executed scene or a typed failure with frame admission canceled. */
        [[nodiscard]] Result<void> ExecutePreparedCanvasFrame(Render::RenderFrameScope &frame,
                                                              std::span<const Render::RenderPassDescriptor> passes,
                                                              const std::optional<Math::Mat4> &placement,
                                                              const ViewportCanvasSource &source);
        [[nodiscard]] Result<void> EnableCameraCrossFade() override;
        [[nodiscard]] EditorViewportCrossFadeCapacity CameraCrossFadeCapacity() const noexcept override;
        [[nodiscard]] Result<void> PrepareCameraCrossFadeFrame(const std::optional<EditorViewportCameraCrossFade> &crossFade) override;
        /** @brief Cold-prepares the actual canonical multiview program before selected XR can publish Ready.
         * @param cooked Same admitted compiler/reflection output loaded from the explicit packaged asset root.
         * @param program Explicitly selected baseline or separate optional environment-lit variant.
         * @return Exact interface/native/material error, without changing ordinary desktop pipeline selection.
         * @pre Initialized selected Vulkan owner, before any interactive frame or graph recording.
         */
        [[nodiscard]] Result<void> PrepareXrProgram(EditorVulkanMultiviewCook cooked,
                                                    EditorVulkanMultiviewProgram program = EditorVulkanMultiviewProgram::Projection);
        /** @brief Prepares the packaged color-only comfort program before any XR image graph exists.
         * @param cooked Exact offline shader and reflected binding pair.
         * @return Resident program or the original shader or resource failure.
         * @pre Initialized Vulkan viewport, owner lane and cold frame state. */
        [[nodiscard]] Result<void> PrepareXrComfortProgram(EditorVulkanXrComfortCook cooked);
        /** @brief Retains the optional packaged motion program before any rendering begins.
         * @param cooked Optional decoded
         * bundle from the selected packaged root.
         */
        void StageXrMotionProgram(EditorVulkanMultiviewCook cooked) noexcept;
        /** @brief Stages the optional packaged stereo depth coverage cook before Play records frames. */
        void StageXrCoverageProgram(EditorVulkanMultiviewCook cooked) noexcept;
        /** @brief Stages the exact artifact after its runtime package and reflected closed compute layout are validated. */
        void StageXrCoverageReductionProgram(EditorVulkanCoverageReductionCook cooked) noexcept;
        /** @brief Prepares the selected same-device pipeline for the exact native/renderer format pair.
         * @param motion Admitted
         * signed floating-point color attachment.
         * @param depth Admitted sampled forward depth attachment.
         * @return
         * True for a resident program, false without an installed bundle, or an original admission error.
         * @pre Cold selected XR
         * scene preparation before graph or external image acquisition.
         */
        [[nodiscard]] Result<bool> PrepareXrMotionPipeline(Render::RenderTextureFormat motion, Render::RenderTextureFormat depth);
        /** @brief Borrows the actual selected device for cold same-session XR image translation.
         * @return Current same-bridge facts or original typed failure. No device/queue/loader is created.
         */
        [[nodiscard]] Result<Render::Detail::VulkanEditorDevice> BorrowXrDevice() const;
        /** @brief Cold-compiles the actual two-layer eye graph over the SAME canonical resident mesh owner.
         * @param meshes Complete canonical scene mesh resources; no desktop camera or target is synthesized.
         * @param color Exact native allocation admitted with sampled first-eye mirror usage before acquisition.
         * @param acquired Exact native acquisition state. @param requiredFinal Exact native release state.
         * @param
         * retainPreviousDepth Current optional feature policy requests the compatible depth predecessor.
         * @param auxiliary Optional exact acquired/release shapes for the native motion and depth passes.
         * @return Success or original resource/graph/tuple failure; partial cold resources remain owned for teardown.
         * @pre The previous XR scene, scene/UI/GUI references and native acquisitions have retired. This operation
         * must finish before interactive admission; no first-frame compilation or allocation fallback is allowed.
         */
        [[nodiscard]] Result<void> PrepareXrGraph(std::span<const EditorViewportMeshResourceView> meshes,
                                                  const Render::RenderTextureDescriptor &color,
                                                  const Render::RenderGraphLogicalState &acquired,
                                                  const Render::RenderGraphLogicalState &requiredFinal, bool retainPreviousDepth = false,
                                                  const EditorVulkanXrViewportAccess::AuxiliaryPlan *auxiliary = nullptr,
                                                  std::size_t coverageDrawCapacity = 0);
        /** @brief Admits a canvas variant at a cold boundary for the prepared ordinary projection. */
        [[nodiscard]] Result<bool> PrepareXrCanvas(std::span<const EditorViewportMeshResourceView> meshes,
                                                   const EditorVulkanXrViewportAccess::CanvasPreparation &canvas);
        /** @brief Binds a fresh native eye acquisition to the cold graph before the SAME RenderFrameScope begins.
         * @param color Fresh imported acquisition. @param eyeViews Exact current per-layer frontend views.
         * @param publication Canonical Scene/frame/origin/view and actual device/capability evidence.
         * @param views Complete actual runtime-ordered view matrices. @param instances SAME canonical geometry projection.
         * @param motion Optional exact native prediction. @param motionObjects Stable object IDs aligned with instances.
         * @param alpha Explicit color policy for the selected canonical XR program.
         * @param auxiliary Exact auxiliary imports, when the selected frame has compatible history.
         * @param lighting Optional same-frame renderer-owned environment input, borrowed only for the bind.
         * @return Success or original stale/capacity/tuple failure; no compilation, heap allocation or native wait.
         * @pre Previous acquisition's scene AND GUI tail references have retired. A failed bind cannot be executed.
         */
        [[nodiscard]] Result<void> BindXrFrame(Render::RenderTextureHandle color, std::span<const Render::RenderTextureViewHandle> eyeViews,
                                               const Render::MultiviewPublication &publication,
                                               std::span<const Render::MultiviewSceneView> views,
                                               std::span<const Render::RenderStaticMeshInstance> instances,
                                               const Render::MultiviewMotionFrame *motion,
                                               std::span<const Render::RenderMotionObjectId> motionObjects, EditorXrProjectionAlpha alpha,
                                               const EditorVulkanXrViewportAccess::AuxiliaryImages *auxiliary = nullptr,
                                               const Render::MultiviewEnvironmentLighting *lighting = nullptr,
                                               std::span<const Render::MultiviewRenderRectangle> rectangles = {},
                                               const EditorVulkanXrViewportAccess::CanvasPlacement *canvas = nullptr,
                                               float vignetteOpacity = 0.0F);
        /** @brief Ends current projection evidence and admits exact native projection into transform history.
         * @pre Same frame's successful native compositor end on the render owner lane.
         */
        void CompleteXrMotion(const std::optional<Render::MultiviewMotionFrame> &frame, bool projected) noexcept;
        /** @brief Executes the bound eye scene through the SAME canonical frame and primary-output/UI/GUI tail.
         * @param frame Existing App scope. @param output Existing desktop output preparation; mirror is composed later by GUI.
         * @return Original admission/encoding error, preserving actual external acquisition retirement obligations.
         * @post Consumes this binding once. Desktop presentation never proves native compositor submission.
         */
        [[nodiscard]] Result<EditorVulkanXrViewportAccess::ProjectionSubmission> ExecuteXrFrame(
            Render::RenderFrameScope &frame, const Render::PrimaryOutputAttachment &output,
            const EditorVulkanXrViewportAccess::CanvasSource *canvas = nullptr);
        /** @brief Returns only the exact admitted current first-eye sample, never a desktop-camera fallback.
         * @param frame Exact App bind frame. @param expected Exact retained acquired first-eye generation.
         * @return Typed GUI binding or empty when no fresh XR frame is bound/retained by its native owner.
         * @pre The App retains the acquisition through BOTH scene and GUI tail completion; this value proves no release.
         */
        [[nodiscard]] EditorViewportTextureView XrMirrorView(std::uint64_t frame, Render::RenderTextureViewHandle expected) const noexcept;
        /** @brief Returns the selected XR logical GUI image without any acquired sampled view.
         * @return Reserved CPU-draw identity while this owner retains cold XR preparation, otherwise empty.
         * @details This projection does not change physical IsReady or prove a current native frame. Failed
         * preparation and retirement debt retain XR selection instead of silently exposing the desktop camera.
         * Only the exact acquired XrMirrorView may resolve this identity during GUI composition. Owner lane.
         */
        [[nodiscard]] EditorViewportTextureView XrLogicalProjection() const noexcept;
        /** @brief Revokes the synchronous acquired-eye borrow without releasing native resources or selection intent.
         * @pre Render-owner lane; actual image/scene/GUI retirement remains with the canonical lease owner.
         */
        void RevokeXrMirror() const noexcept;
        /** @brief Polls the actual canonical graphics queue without waiting or declaring image/GUI completion.
         * @return Original observation error or successful zero-time queue poll. Image release still requires
         * the external acquisition owner's actual scene AND GUI retirement/final-state evidence.
         */
        [[nodiscard]] Result<void> PollXrQueue();
        [[nodiscard]] Result<Render::ShaderHotReloadTargetState> CaptureState() const override;
        [[nodiscard]] Result<std::unique_ptr<Render::IShaderHotReloadCandidate>> Prepare(
            std::span<const Render::ShaderHotReloadProgramView> programs, Render::ShaderHotReloadTargetState expected,
            const CancellationToken &cancellation = {}) override;
        [[nodiscard]] Result<Render::ShaderHotReloadTargetState> Adopt(std::unique_ptr<Render::IShaderHotReloadCandidate> candidate,
                                                                       Render::ShaderHotReloadTargetState expected) override;
        void Shutdown() noexcept;
        void ReleaseNative() noexcept override;
        [[nodiscard]] Result<void> ExecutePreparedFrame(Render::RenderFrameScope &frame,
                                                        std::span<const Render::RenderPassDescriptor> passes) override;
        [[nodiscard]] Result<void> ExecuteStaticMeshPass(const Render::StaticMeshPassDescriptor &) override;
        [[nodiscard]] Result<std::optional<Render::RenderTargetHandle>> PrepareResources(Render::RenderFrontend &frontend,
                                                                                         const Render::RenderSceneView &scene) override;
        void RequestExtent(EditorViewportExtent extent) noexcept override;
        void RequestGrid(const EditorViewportGridOptions &options) noexcept override;
        [[nodiscard]] Result<void> RequestDirectionalShadowProfile(const Render::DirectionalShadowProfile &profile) override;
        /** @copydoc IEditorViewportRenderer::PrepareLightingFrame */
        [[nodiscard]] Result<void> PrepareLightingFrame(const Render::StaticMeshPassDescriptor &pass, Render::RenderViewId view) override;
        [[nodiscard]] Result<void> RequestLightingProfile(const EditorViewportLightingProfile &profile,
                                                          const Render::LightPreparationSelection &selection) override;

        [[nodiscard]] EditorViewportLightingDiagnostics LightingDiagnostics() const noexcept override {
            return forwardLighting_.Diagnostics();
        }

        void RequestLightVisualizer(const EditorViewportLightVisualizerOptions &options) noexcept override;
        [[nodiscard]] EditorViewportExtent RequestedExtent() const noexcept override;
        [[nodiscard]] Math::ClipDepthRange ClipDepthRange() const noexcept override;
        [[nodiscard]] EditorViewportTextureView TextureView() const noexcept override;
        [[nodiscard]] bool IsReady() const noexcept override;

    private:
        friend class EditorVulkanXrViewportAccess;
        struct XrProjectionGraph;
        struct ViewportRasterGraphs;
        struct ViewportCanvasGraph;
        /** @brief Prepares one complete canvas graph, optionally with the exact admitted image copy. */
        [[nodiscard]] Result<std::shared_ptr<ViewportCanvasGraph>> BuildViewportCanvasGraph(
            const ViewportCanvasPreparation &request, const CancellationToken &cancellation,
            const EditorGraphViewportImageCapture::Copy *capture);

        /** @brief Borrows canvas recording inputs only while the exact prepared scene phase executes. */
        struct CanvasFrame final {
            const std::optional<Math::Mat4> &placement;
            const ViewportCanvasSource &source;
        };

        /** @brief Keeps validation, native output admission and cancellation identical for both viewport scene recipes. */
        [[nodiscard]] Result<void> ExecutePreparedSceneFrame(Render::RenderFrameScope &frame,
                                                             std::span<const Render::RenderPassDescriptor> passes,
                                                             const CanvasFrame *canvas);
        /** @brief Compiles owner-slot shadow/scene/overlay graphs only when exact resource topology changes. */
        [[nodiscard]] Result<void> PrepareViewportRasterGraphs(const Render::StaticMeshPassDescriptor &pass);
        /** @brief Prepares a bounded optional capture variant without disturbing the normal raster graph. */
        void PrepareImageCaptureGraph();
        /** @brief Records the one cold graph matching this actual native slot and frozen viewport resources. */
        [[nodiscard]] Result<void> ExecuteViewportRasterGraph(Render::RenderFrameScope &frame, const Render::StaticMeshPassDescriptor &pass,
                                                              std::uint32_t slot, bool hasOverlay,
                                                              const Render::DirectionalShadowFrame *shadow,
                                                              const std::optional<Render::DirectionalShadowFrame> *sourceShadow = nullptr);
        std::shared_ptr<ViewportRasterGraphs> viewportRasterGraphs_;
        /** @brief Returns the current desktop presentation attachment, including the post-color encode output. */
        [[nodiscard]] EditorGraphViewportImageCapture::Source ImageSource() const noexcept;
        EditorGraphViewportImageCapture imageCapture_;
        std::shared_ptr<ViewportCanvasGraph> viewportCanvasGraph_;
        struct XrGraph;
        /** @copydoc EditorVulkanXrViewportAccess::TakePremultipliedProjection */
        [[nodiscard]] std::optional<EditorXrPremultipliedProjection> TakeXrPremultipliedProjection(
            const Render::MultiviewPublication &publication, Render::RenderTextureHandle color) noexcept;
        /** @brief Uploads exact adjacent motion matrices to the selected signed pass's cold uniform slots. */
        [[nodiscard]] Result<void> WriteXrMotionDraws(Render::FrameToken frame, const XrGraph &graph);
        /** @brief Writes the admitted rectangle and opacity into the exact eye slot. */
        [[nodiscard]] Result<void> WriteXrComfort(Render::FrameToken frame, std::size_t eye,
                                                  const Render::MultiviewRenderRectangle &rectangle, float opacity) const;
        /** @brief Realizes the optional packaged coverage program for the owning Scene. */
        [[nodiscard]] Result<void> PrepareXrCoveragePipeline();
        /** @brief Drops only cold scene graph/depth resources after their canonical scene/GUI lease has drained. */
        void ReleaseXrGraph() noexcept;
        /** @brief Drops graph-held sampled descriptor generations before their device goes away. */
        void ReleaseXrCoverageMaterials() noexcept;
        /** @brief Unwinds optional cold coverage topology, material pins and owned scratch reservations. */
        [[nodiscard]] Result<void> RetireXrCoverage();
        /** @brief Retires cold graph/depth storage with retryable typed errors and exact successful-prefix bookkeeping.
         * @return Original frontend release error, retaining unreleased owners, or complete retirement.
         * @pre Owner lane, no active frame, and actual native acquisition scene/GUI retirement proved by the App.
         */
        [[nodiscard]] Result<void> RetireXrGraph();
        static constexpr std::size_t MaximumUniformBytes = 4096;

        struct DrawSlot {
            Render::RenderBufferHandle buffer;
            std::uint64_t instance{};
            std::uint64_t materialIdentity{};
            std::uint64_t materialRevision{};
            std::shared_ptr<const Render::ResidentMaterialBinding> material;
        };

        static constexpr std::size_t MaximumOverlayVertices = 9744;

        struct OverlaySlot {
            DrawSlot uniforms;
            Render::RenderBufferHandle vertices, indices;
            Render::RenderMeshHandle mesh;
            std::uint64_t vertexInstance{};
        };

        struct FrameSlot {
            std::array<DrawSlot, Render::RenderMaterialPass::MaximumDraws> draws;
            OverlaySlot overlay;
            std::array<std::array<DrawSlot, Render::RenderMaterialPass::MaximumDraws>, Render::MaximumDirectionalShadowCascades> shadows;
        };

        static constexpr std::uint32_t CrossFadeShadowPasses = (Render::RenderGraphMaterialRaster::MaximumRetainedPasses - 3) / 2;
        using FrameSlots = std::array<FrameSlot, 8>;

        struct EmptyGraph {
            Render::CompiledRenderGraphExecution graph;
            std::array<Render::RenderGraphPassWorkload, 1> workloads;
        };

        /** @brief One cooked-program admission and its dependent material resources, released after device drain. */
        struct ShaderResources {
            std::vector<std::uint8_t> payload;
            Render::ReflectedShaderArtifact metadata;
            Render::RenderShaderModuleHandle vertex, fragment;
            Render::RenderPipelineHandle pipeline, overlayPipeline;
            Render::MaterialBindingLayout layout;
            std::unique_ptr<Render::MaterialBindingTable> materials;
            Render::RenderSamplerHandle shadowSampler;
            bool depthOnly{};
            std::size_t parameterBytes{}; /**< Exact logical packing extent, excluding native block tail padding. */
            std::size_t uniformBytes{};   /**< Selected backend's allocation, binding and full-upload extent. */
        };

        /** @brief Owns both viewport guide programs, including partially admitted candidates. */
        struct TemporalGuidePrograms final {
            explicit TemporalGuidePrograms(EditorViewportRendererVulkan &viewport) noexcept;
            ~TemporalGuidePrograms();
            TemporalGuidePrograms(const TemporalGuidePrograms &) = delete;
            TemporalGuidePrograms &operator=(const TemporalGuidePrograms &) = delete;

            /** @brief Grows all buffered frames together while preserving previously valid draw slots on failure. */
            [[nodiscard]] Result<void> PrepareDraws(std::size_t count);
            /** @brief Uploads the exact unjittered first-frame guide bindings for the selected viewport slot. */
            [[nodiscard]] Result<void> UploadSeed(Render::FrameToken frame, std::uint32_t slot,
                                                  const Render::StaticMeshPassDescriptor &pass) const;

            struct GuideDraw final {
                DrawSlot linearDepth;
                DrawSlot motion;
            };

            EditorViewportRendererVulkan &owner;
            ShaderResources linearDepth;
            ShaderResources motion;
            std::array<std::vector<GuideDraw>, std::tuple_size_v<FrameSlots>> draws;

        private:
            [[nodiscard]] Result<void> GrowFrame(std::uint32_t slot, std::size_t count);
            /** @brief Packs and uploads both guides for one draw without allocating or retaining scene data. */
            [[nodiscard]] Result<void> UploadSeedDraw(Render::FrameToken frame, const GuideDraw &draw, const Math::Mat4 &view,
                                                      const Render::RenderMotionCameraPair &camera, const Math::Mat4 &localToWorld) const;
            void RetireDrawsAfter(std::size_t keep) noexcept;
        };

        /** @brief Owns one immutable canvas consumer and its slot-local mesh bindings. */
        struct CanvasQuadProgram final {
            static constexpr std::size_t StereoViews = 2;
            explicit CanvasQuadProgram(EditorViewportRendererVulkan &viewport, std::uint32_t viewCount);
            ~CanvasQuadProgram();
            CanvasQuadProgram(const CanvasQuadProgram &) = delete;
            CanvasQuadProgram &operator=(const CanvasQuadProgram &) = delete;

            EditorViewportRendererVulkan &owner;
            ShaderResources shader;
            Render::RenderSamplerHandle sampler;
            Render::RenderMeshHandle mesh;
            Render::RenderBufferHandle vertices, indices;
            std::array<std::array<OverlaySlot, StereoViews>, std::tuple_size_v<FrameSlots>> slots;
            std::array<Render::RenderTextureViewHandle, std::tuple_size_v<FrameSlots>> sampledViews;
            Render::UiRenderCompositionSpace space{};
            const std::uint32_t viewCount;
        };

        /** @brief Creates the closed color/depth pipeline and immutable quad outside frame recording. */
        [[nodiscard]] Result<std::unique_ptr<CanvasQuadProgram>> CreateCanvasQuadProgram(const EditorVulkanWorldCanvasCook &cook,
                                                                                         Render::UiRenderCompositionSpace space,
                                                                                         Render::RenderTextureFormat colorFormat,
                                                                                         std::uint32_t viewCount = 1);
        /** @brief Admits one distinct producer surface and frozen material for this graph slot. */
        [[nodiscard]] Result<void> BindCanvasQuadSource(CanvasQuadProgram &program, std::uint32_t slot, Render::RenderTextureHandle canvas);
        /** @brief Uploads the current camera/World matrix to the exact acquired frame's pre-bound slot. */
        [[nodiscard]] Result<void> WriteCanvasQuad(Render::FrameToken frame, const CanvasQuadProgram &program, std::uint32_t slot,
                                                   const Render::StaticMeshPassDescriptor &pass,
                                                   const std::optional<Math::Mat4> &placement) const;
        /** @brief Uploads exact Vulkan clip matrices into distinct material bindings that share the slot's sampled surface. */
        [[nodiscard]] Result<void> WriteCanvasQuadViews(Render::FrameToken frame, const CanvasQuadProgram &program, std::uint32_t slot,
                                                        std::span<const Math::Mat4> clips) const;
        /** @brief Packs one finite clip-from-canvas matrix into the already admitted native binding. */
        [[nodiscard]] Result<void> WriteCanvasQuadClip(Render::FrameToken frame, const ShaderResources &program, const DrawSlot &draw,
                                                       const Math::Mat4 &clip) const;
        /** @brief Creates the closed top-left UV quad shared by all program slots. */
        [[nodiscard]] Result<void> CreateCanvasQuadMesh(CanvasQuadProgram &program);

        struct FullscreenTriangle final {
            Render::RenderBufferHandle vertices, indices;
            Render::RenderMeshHandle mesh;
        };

        struct XrComfortProgram final {
            ShaderResources shader;
            FullscreenTriangle triangle;
            std::array<DrawSlot, 2> eyes;
        };

        /** @brief Creates the position-only fullscreen triangle shared by authored overlay programs. */
        [[nodiscard]] Result<void> CreateFullscreenTriangle(FullscreenTriangle &triangle);
        /** @brief Retires the valid prefix of the fullscreen allocation, including on cold setup failure. */
        void RetireFullscreenTriangle(FullscreenTriangle &triangle) noexcept;
        /** @brief Creates the exact color-only pipeline and one uniform for each eye. */
        [[nodiscard]] Result<void> CreateXrComfortResources(XrComfortProgram &program);
        /** @brief Releases the program and any partially allocated resources. */
        void RetireXrComfortProgram(XrComfortProgram &program) noexcept;

        /** @brief Owns the distinct sampled fullscreen program and immutable resources used by the cross-fade pass. */
        struct CameraCrossFadeProgram final {
            ShaderResources shader;
            FullscreenTriangle triangle;
            std::array<DrawSlot, 8> draws;
            Render::RenderTextureViewHandle boundSource;
        };

        [[nodiscard]] Result<void> PrepareCameraCrossFadeProgram(CameraCrossFadeProgram &program);
        [[nodiscard]] Result<void> BindCameraCrossFadeSource();
        [[nodiscard]] Result<void> PrepareCameraCrossFadeScene(std::size_t draws);
        void RetireCameraCrossFadeProgram(CameraCrossFadeProgram &program) noexcept;
        [[nodiscard]] Result<void> PrepareCameraCrossFadeSlots(std::span<FrameSlot> slots, const ShaderResources &scene,
                                                               const ShaderResources &shadow, std::size_t first, std::size_t count);
        void RetireCameraCrossFadeSlots(std::span<FrameSlot> slots, std::size_t first, std::size_t count,
                                        const ShaderResources *scene = nullptr, const ShaderResources *shadow = nullptr) noexcept;

        /** @brief Stateless cold program construction and reflected packing; all owned storage stays in this viewport. */
        struct ProgramResources final {
            /** @brief Admits the desktop program and its overlay against one frontend and frame capacity. */
            [[nodiscard]] static Result<void> CreatePipeline(Render::RenderFrontend &frontend, ShaderResources &program,
                                                             std::uint32_t frames, bool displayLinearOverlay = false);
            /** @brief Creates native shader stages into the same retained program, preserving partial prefixes. */
            [[nodiscard]] static Result<void> CreateProgramStages(Render::RenderFrontend &frontend, ShaderResources &program,
                                                                  const char *fragmentEntry = "PSMain", const char *vertexEntry = "VSMain");
            /** @brief Realizes the exact dedicated scene-linear Vulkan material. */
            [[nodiscard]] static Result<void> CreateSceneLinearPipeline(Render::RenderFrontend &frontend, ShaderResources &program,
                                                                        std::uint32_t frames);
            /** @brief Realizes an exact temporal guide pipeline with the existing native material layout owner. */
            [[nodiscard]] static Result<void> CreateTemporalGuidePipeline(Render::RenderFrontend &frontend, ShaderResources &program,
                                                                          EditorVulkanTemporalInput input, const TemporalGuideCook &cook,
                                                                          std::uint32_t frames);
            /** @brief Creates a pipeline using exact selected-device and cooked-program identity. */
            [[nodiscard]] static Result<Render::RenderPipelineHandle> CreatePipelineState(Render::RenderFrontend &frontend,
                                                                                          const ShaderResources &program,
                                                                                          const Render::ShaderManifest &manifest,
                                                                                          const Render::RenderGraphicsPipelineState &state);
            /** @brief Admits logical and native uniform extents and owns one bounded material table. */
            [[nodiscard]] static Result<void> CreateMaterialLayout(Render::RenderFrontend &frontend, ShaderResources &program,
                                                                   const Render::NormalizedShaderReflection &reflection,
                                                                   std::uint32_t frames);
            /** @brief Creates the bounded desktop draw slots without replacing their owner. */
            [[nodiscard]] static Result<void> CreateDrawSlots(Render::RenderFrontend &frontend, const ShaderResources &program,
                                                              std::span<FrameSlot> slots, std::uint32_t frames,
                                                              Render::RenderTextureViewHandle atlas = {});
            /** @brief Creates one exact program buffer and material generation during cold admission. */
            [[nodiscard]] static Result<void> CreateUniformSlot(Render::RenderFrontend &frontend, const ShaderResources &program,
                                                                DrawSlot &draw, std::uint64_t material,
                                                                Render::RenderPipelineHandle pipeline,
                                                                Render::RenderTextureViewHandle atlas = {},
                                                                std::span<const Render::MaterialResourceBinding> sampled = {});
            [[nodiscard]] static Result<void> BindUniformSlot(const ShaderResources &program, DrawSlot &draw,
                                                              Render::RenderPipelineHandle pipeline, Render::RenderTextureViewHandle atlas,
                                                              std::span<const Render::MaterialResourceBinding> sampled = {});
            static void ReleaseMaterials(std::span<FrameSlot> slots) noexcept;
            /** @brief Packs one finite reflected parameter into caller-owned bounded storage. */
            [[nodiscard]] static Result<void> WriteParameter(const Render::MaterialBindingLayout &layout, std::span<std::byte> bytes,
                                                             Render::ShaderParameterId parameter, std::span<const float> values);
            /** @brief Packs the admitted canonical light arrays into the same reflected layout. */
            [[nodiscard]] static Result<void> WriteLighting(const Render::MaterialBindingLayout &layout, std::span<std::byte> bytes,
                                                            const Render::RenderSceneView &scene);
            /** @brief Writes the current draw through the existing bridge using the program ABI and viewport extent. */
            [[nodiscard]] static Result<void> WriteDraw(const Render::Detail::VulkanEditorGraphicsBridge &bridge,
                                                        const ShaderResources &program, EditorViewportExtent extent,
                                                        Render::FrameToken frame, const DrawSlot &slot,
                                                        const Render::RenderStaticMeshInstance &instance,
                                                        const Render::RenderSceneView &scene, const Render::DirectionalShadowFrame *shadow);
            [[nodiscard]] static Result<void> WriteShadowParameters(const ShaderResources &program, std::span<std::byte> bytes,
                                                                    const Render::RenderSceneView &scene,
                                                                    const Render::DirectionalShadowFrame *shadow);
        };

        /** @brief Polls the exact selected device's last reader before reusing a color uniform. */
        [[nodiscard]] static Result<EditorViewportColorReadiness> ObserveColorBuffer(void *context, Render::RenderBufferHandle buffer);
        /** @brief Writes one complete idle native uniform through the selected device owner. */
        [[nodiscard]] static Result<void> WriteColorBuffer(void *context, Render::RenderBufferHandle buffer,
                                                           std::span<const std::byte> bytes);
        /** @brief Realizes one explicit Vulkan manual ColorOutput owner with the selected device's preparation ports. */
        [[nodiscard]] Result<std::unique_ptr<EditorViewportColorPipeline>> MakeViewportColor(const EditorViewportColorInputs &inputs,
                                                                                             const Render::ExposureSettings &settings);
        /** @brief Prepares actual sampled attachment bindings and a free native-backed uniform pair before BeginFrame. */
        [[nodiscard]] Result<void> PrepareViewportColor(Render::RenderViewId view, Render::FramebufferExtent extent);
        /** @brief Realizes an admitted explicit spatial kernel on the viewport's direct aliased graphics/compute queue. */
        [[nodiscard]] Result<std::shared_ptr<Render::IResidentNativeReconstructionKernel>> PrepareSpatialKernel(
            const Render::CookedNativeReconstructionKernel *kernel);
        /** @brief Validates exact selected viewport resources and projection for the active same-extent graph. */
        [[nodiscard]] Result<Render::NativeReconstructionDispatchPlan> PrepareSpatialDispatch(
            Render::FrameToken frame, const Render::StaticMeshPassDescriptor &pass) const;
        /** @brief Publishes only the exact last recorded color graph ticket and records its acceptance owner. */
        void RecordViewportColor(Render::RenderTimelinePoint signal) noexcept;
        /** @brief Returns the selected scene program alongside the independently owned display overlay. */
        [[nodiscard]] const ShaderResources &SceneProgram() const noexcept;
        struct ProgramGeneration;
        [[nodiscard]] Result<void> PrepareProgram(ProgramGeneration &candidate);
        [[nodiscard]] Result<void> PrepareShadowProgram(ShaderResources &program, std::span<FrameSlot> slots);
        [[nodiscard]] Result<void> CreateShadowSlots(const ShaderResources &program, std::span<FrameSlot> slots, std::uint32_t cascades);
        [[nodiscard]] Result<void> BindShadowAtlas();
        [[nodiscard]] Result<void> PrepareShadowDraws(Render::FrameToken frame, std::uint32_t slot, const Render::RenderSceneView &scene,
                                                      const Render::DirectionalShadowFrame &shadow);
        [[nodiscard]] Result<void> WriteShadowDraw(Render::FrameToken frame, const DrawSlot &draw,
                                                   const Render::RenderStaticMeshInstance &instance,
                                                   const Render::DirectionalShadowFrame &shadow, std::uint32_t cascade) const;
        [[nodiscard]] Result<void> ExecuteShadowedViewport(Render::RenderFrameScope &frame, const Render::StaticMeshPassDescriptor &pass,
                                                           std::size_t draws, const Render::DirectionalShadowFrame *shadow);

        /** @brief Builds overlay geometry and bindings into an unpublished complete program generation. */
        [[nodiscard]] Result<void> CreateOverlaySlots(ShaderResources &program, std::span<FrameSlot> slots);
        /** @brief Atomically exchanges the complete desktop program and per-frame consumers on the cold owner lane. */
        void AdoptProgram(ProgramGeneration &candidate) noexcept;
        /** @brief Drops only this program's discovery; accepted material/submission leases retain native resources. */
        void RetireProgram(ShaderResources &program, std::span<FrameSlot> slots) noexcept;
        void RetireDraw(DrawSlot &draw) noexcept;
        void ReleaseProgramObjects(ShaderResources &program) noexcept;
        /** @brief Drops desktop material and guide owners when native recording stops. */
        void ReleaseDesktopPrograms() noexcept;
        /** @brief Finishes backend preparation for an already published viewport target. */
        [[nodiscard]] Result<void> PrepareReadyViewport(std::size_t draws);
        void RetireXrProgram() noexcept;
        [[nodiscard]] Result<void> CreateOverlayMesh(OverlaySlot &slot);
        void ReleaseOverlaySlots() noexcept;
        [[nodiscard]] Result<bool> PrepareOverlay(Render::FrameToken frame, OverlaySlot &slot,
                                                  const Render::StaticMeshPassDescriptor &pass);
        [[nodiscard]] Result<void> WriteOverlayUniforms(Render::FrameToken frame, const OverlaySlot &slot,
                                                        const Render::RenderCameraView &camera, Render::FramebufferExtent extent,
                                                        std::span<const float> palette);
        [[nodiscard]] Result<void> CreateEmptyGraph();
        /** @brief Submits the cached empty-scene clear through the exact current frontend frame. */
        [[nodiscard]] Result<void> ExecuteEmptyViewport(Render::RenderFrameScope &frame, Render::ClearColor clearColor);
        [[nodiscard]] Result<std::uint32_t> PrepareViewportSlot(Render::FrameToken frame,
                                                                const Render::StaticMeshPassDescriptor &pass) const;
        [[nodiscard]] Result<void> PrepareShadowFrame(Render::FrameToken frame, std::uint32_t slot,
                                                      const Render::StaticMeshPassDescriptor &pass);
        [[nodiscard]] Result<std::size_t> PrepareViewportOverlay(Render::FrameToken frame, std::uint32_t slot,
                                                                 const Render::StaticMeshPassDescriptor &pass);
        [[nodiscard]] Result<void> PrepareViewportDraws(Render::FrameToken frame, std::uint32_t slot,
                                                        const Render::StaticMeshPassDescriptor &pass);
        [[nodiscard]] Result<void> PrepareCameraCrossFadeDraws(Render::FrameToken frame, std::uint32_t slot,
                                                               const Render::RenderSceneView &scene,
                                                               const std::optional<Render::DirectionalShadowFrame> &shadow,
                                                               float destinationWeight);
        [[nodiscard]] Result<void> ExecuteCameraCrossFade(Render::RenderFrameScope &frame, const Render::StaticMeshPassDescriptor &pass,
                                                          std::uint32_t slot, bool hasOverlay, const EditorViewportCameraCrossFade &fade);
        [[nodiscard]] Result<void> ExecuteViewport(Render::RenderFrameScope &frame, const Render::StaticMeshPassDescriptor &pass);
        [[nodiscard]] Result<void> PrepareViewportOutput(Render::RenderFrameScope &frame, const Render::PrimaryOutputAttachment &output,
                                                         const Render::StaticMeshPassDescriptor *viewport) const;

        [[nodiscard]] Result<void> WriteForwardDraw(Render::FrameToken frame, const DrawSlot &draw,
                                                    const Render::RenderStaticMeshInstance &instance, const Render::RenderSceneView &scene,
                                                    const std::optional<Render::DirectionalShadowFrame> &shadow);
        std::array<Render::RenderLight, Render::MaximumForwardLights> forwardDrawLights_;
        EditorViewportForwardLighting forwardLighting_;
        const std::thread::id owner_{std::this_thread::get_id()};
        Render::RenderFrontend &frontend_;
        Render::Detail::VulkanEditorGraphicsBridge &bridge_;
        std::unique_ptr<EditorViewportColorPipeline> color_;
        std::shared_ptr<Render::IResidentNativeReconstructionKernel> spatial_;
        std::unique_ptr<TemporalGuidePrograms> temporalGuides_;

        struct ColorSubmission final {
            Render::RenderTimelinePoint signal;
            Render::RenderTextureViewHandle output;
            std::uint64_t deviceIncarnation{};
        };

        ColorSubmission pendingColorSubmission_;
        Render::RenderTextureViewHandle acceptedColorView_;
        float colorFrameDelta_{};
        ShaderResources sceneLinearShader_;
        ShaderResources shader_;
        std::optional<EditorVulkanCameraCrossFadeCook> crossFadeCook_;
        CameraCrossFadeProgram crossFadeProgram_;
        std::unique_ptr<FrameSlots> crossFadeSourceSlots_;

        struct PreparedCameraCrossFade final {
            EditorViewportCameraCrossFade fade;
            Render::RenderTargetHandle target;
        };

        std::optional<PreparedCameraCrossFade> preparedCrossFade_;
        std::uint64_t crossFadeGeneration_{};
        std::uint32_t crossFadeShadowBudget_{};
        std::size_t crossFadeDrawCount_{};
        std::optional<std::size_t> declinedCrossFadeDrawCount_;
        bool crossFadeSceneReady_{};
        ShaderResources shadowShader_;
        ShaderResources xrShader_;
        XrComfortProgram xrComfort_;
        bool xrEnvironmentLighting_{};
        std::optional<EditorVulkanMultiviewCook> xrMotionCook_;
        std::optional<EditorVulkanMultiviewCook> xrCoverageCook_;
        std::vector<std::uint8_t> xrCoverageReductionArtifact_;
        std::optional<std::array<Render::MultiviewRenderRectangle, 2>> xrCoverageProbe_;
        ShaderResources xrMotionShader_;
        ShaderResources xrCoverageShader_;
        std::array<DrawSlot, Render::RenderMaterialPass::MaximumDraws> xrMotionDrawSlots_;
        std::optional<std::pair<Render::RenderTextureFormat, Render::RenderTextureFormat>> xrMotionFormats_;
        std::shared_ptr<XrGraph> xrGraph_;
        std::array<std::array<DrawSlot, Render::RenderMaterialPass::MaximumDraws>, 8> xrDrawSlots_;
        EditorViewportResources resources_;
        Render::Detail::VulkanEditorDevice device_;
        std::array<FrameSlot, 8> slots_;
        std::array<Render::RenderMaterialDraw, Render::RenderMaterialPass::MaximumDraws> preparedDraws_;
        std::array<std::array<Render::RenderMaterialDraw, Render::RenderMaterialPass::MaximumDraws>,
                   Render::MaximumDirectionalShadowCascades>
            preparedShadowDraws_;
        Render::RenderTextureViewHandle boundShadowAtlas_;
        std::optional<Render::DirectionalShadowFrame> preparedShadow_;
        std::array<Render::MeshVertex, MaximumOverlayVertices> overlayVertices_;
        std::optional<EmptyGraph> emptyGraph_;
        Render::RenderTextureHandle emptyGraphTexture_;
        EditorViewportExtent requestedExtent_;
        EditorViewportGridOptions requestedGrid_;
        EditorViewportLightVisualizerOptions requestedLightVisualizer_;
        std::uint64_t liveGeneration_{};
        bool initialized_{};
        bool childAttached_{};
        bool recordedEver_{};
    };
}  // namespace Horo::Editor
