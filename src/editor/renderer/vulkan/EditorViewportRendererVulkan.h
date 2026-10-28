#pragma once
/** @file EditorViewportRendererVulkan.h
 * @brief Selected Vulkan viewport using canonical cooked material, frontend resources and frame submission. */
#include "EditorVulkanViewportShader.h"
#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/MultiviewSceneExtraction.h"
#include "Horo/Runtime/Render/RenderFrontend.h"
#include "VulkanEditorGraphicsBridge.h"
#include "editor/renderer/EditorViewportResources.h"
#include "editor/renderer/grid/ViewportOverlayTriangles.h"

#include <array>
#include <thread>

namespace Horo::Editor {
    class EditorViewportRendererVulkan final : public IEditorViewportRenderer, public Render::Detail::IVulkanEditorDeviceChild {
    public:
        EditorViewportRendererVulkan(Render::RenderFrontend &frontend, Render::Detail::VulkanEditorGraphicsBridge &bridge,
                                     EditorVulkanViewportCook cooked) noexcept;
        ~EditorViewportRendererVulkan() override;
        [[nodiscard]] Result<void> Initialize();
        /** @brief Cold-prepares the actual canonical multiview program before selected XR can publish Ready.
         * @param cooked Same admitted compiler/reflection output loaded from the explicit packaged asset root.
         * @return Exact interface/native/material error, without changing ordinary desktop pipeline selection.
         * @pre Initialized selected Vulkan owner, before any interactive frame or graph recording.
         */
        [[nodiscard]] Result<void> PrepareXrProgram(EditorVulkanMultiviewCook cooked);
        /** @brief Borrows the actual selected device for cold same-session XR image translation.
         * @return Current same-bridge facts or original typed failure. No device/queue/loader is created.
         */
        [[nodiscard]] Result<Render::Detail::VulkanEditorDevice> BorrowXrDevice() const;
        /** @brief Cold-compiles the actual two-layer eye graph over the SAME canonical resident mesh owner.
         * @param meshes Complete canonical scene mesh resources; no desktop camera or target is synthesized.
         * @param color Exact native allocation admitted with sampled first-eye mirror usage before acquisition.
         * @param acquired Exact native acquisition state. @param requiredFinal Exact native release state.
         * @return Success or original resource/graph/tuple failure; partial cold resources remain owned for teardown.
         * @pre The previous XR scene, scene/UI/GUI references and native acquisitions have retired. This operation
         * must finish before interactive admission; no first-frame compilation or allocation fallback is allowed.
         */
        [[nodiscard]] Result<void> PrepareXrGraph(std::span<const EditorViewportMeshResourceView> meshes,
                                                  const Render::RenderTextureDescriptor &color,
                                                  const Render::RenderGraphLogicalState &acquired,
                                                  const Render::RenderGraphLogicalState &requiredFinal);
        /** @brief Binds a fresh native eye acquisition to the cold graph before the SAME RenderFrameScope begins.
         * @param color Fresh imported acquisition. @param eyeViews Exact current per-layer frontend views.
         * @param publication Canonical Scene/frame/origin/view and actual device/capability evidence.
         * @param views Complete actual runtime-ordered view matrices. @param instances SAME canonical geometry projection.
         * @return Success or original stale/capacity/tuple failure; no compilation, heap allocation or native wait.
         * @pre Previous acquisition's scene AND GUI tail references have retired. A failed bind cannot be executed.
         */
        [[nodiscard]] Result<void> BindXrFrame(Render::RenderTextureHandle color,
                                               const std::array<Render::RenderTextureViewHandle, 2> &eyeViews,
                                               const Render::MultiviewPublication &publication,
                                               std::span<const Render::MultiviewSceneView> views,
                                               std::span<const Render::RenderStaticMeshInstance> instances);
        /** @brief Executes the bound eye scene through the SAME canonical frame and primary-output/UI/GUI tail.
         * @param frame Existing App scope. @param output Existing desktop output preparation; mirror is composed later by GUI.
         * @return Original admission/encoding error, preserving actual external acquisition retirement obligations.
         * @post Consumes this binding once. Desktop presentation never proves native compositor submission.
         */
        [[nodiscard]] Result<void> ExecuteXrFrame(Render::RenderFrameScope &frame, const Render::PrimaryOutputAttachment &output);
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
        void RevokeXrMirror() noexcept;
        /** @brief Polls the actual canonical graphics queue without waiting or declaring image/GUI completion.
         * @return Original observation error or successful zero-time queue poll. Image release still requires
         * the external acquisition owner's actual scene AND GUI retirement/final-state evidence.
         */
        [[nodiscard]] Result<void> PollXrQueue();
        void Shutdown() noexcept;
        void ReleaseNative() noexcept override;
        [[nodiscard]] Result<void> ExecutePreparedFrame(Render::RenderFrameScope &frame,
                                                        std::span<const Render::RenderPassDescriptor> passes) override;
        [[nodiscard]] Result<void> ExecuteStaticMeshPass(const Render::StaticMeshPassDescriptor &) override;
        [[nodiscard]] Result<std::optional<Render::RenderTargetHandle>> PrepareResources(Render::RenderFrontend &frontend,
                                                                                         const Render::RenderSceneView &scene) override;
        void RequestExtent(EditorViewportExtent extent) noexcept override;
        void RequestGrid(const EditorViewportGridOptions &options) noexcept override;
        void RequestLightVisualizer(const EditorViewportLightVisualizerOptions &options) noexcept override;
        [[nodiscard]] EditorViewportExtent RequestedExtent() const noexcept override;
        [[nodiscard]] Math::ClipDepthRange ClipDepthRange() const noexcept override;
        [[nodiscard]] EditorViewportTextureView TextureView() const noexcept override;
        [[nodiscard]] bool IsReady() const noexcept override;

    private:
        friend class EditorVulkanXrViewportAccess;
        struct XrGraph;
        /** @brief Drops only cold scene graph/depth resources after their canonical scene/GUI lease has drained. */
        void ReleaseXrGraph() noexcept;
        /** @brief Retires cold graph/depth storage with retryable typed errors and exact successful-prefix bookkeeping.
         * @return Original frontend release error, retaining unreleased owners, or complete retirement.
         * @pre Owner lane, no active frame, and actual native acquisition scene/GUI retirement proved by the App.
         */
        [[nodiscard]] Result<void> RetireXrGraph();
        static constexpr std::size_t MaximumUniformBytes = 4096;

        struct DrawSlot {
            Render::RenderBufferHandle buffer;
            std::uint64_t instance{};
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
        };

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
            std::size_t parameterBytes{}; /**< Exact logical packing extent, excluding native block tail padding. */
            std::size_t uniformBytes{};   /**< Selected backend's allocation, binding and full-upload extent. */
        };

        /** @brief Stateless cold program construction and reflected packing; all owned storage stays in this viewport. */
        struct ProgramResources final {
            /** @brief Admits the desktop program and its overlay against one frontend and frame capacity. */
            [[nodiscard]] static Result<void> CreatePipeline(Render::RenderFrontend &frontend, ShaderResources &program,
                                                             std::uint32_t frames);
            /** @brief Creates native shader stages into the same retained program, preserving partial prefixes. */
            [[nodiscard]] static Result<void> CreateProgramStages(Render::RenderFrontend &frontend, ShaderResources &program);
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
                                                              std::span<FrameSlot> slots, std::uint32_t frames);
            /** @brief Creates one exact program buffer and material generation during cold admission. */
            [[nodiscard]] static Result<void> CreateUniformSlot(Render::RenderFrontend &frontend, const ShaderResources &program,
                                                                DrawSlot &draw, std::uint64_t material,
                                                                Render::RenderPipelineHandle pipeline);
            /** @brief Packs one finite reflected parameter into caller-owned bounded storage. */
            [[nodiscard]] static Result<void> WriteParameter(const Render::MaterialBindingLayout &layout, std::span<std::byte> bytes,
                                                             Render::ShaderParameterId parameter, std::span<const float> values);
            /** @brief Packs the admitted canonical light arrays into the same reflected layout. */
            [[nodiscard]] static Result<void> WriteLighting(const Render::MaterialBindingLayout &layout, std::span<std::byte> bytes,
                                                            const Render::RenderSceneView &scene);
            /** @brief Writes the current draw through the existing bridge using the program ABI and viewport extent. */
            [[nodiscard]] static Result<void> WriteDraw(Render::Detail::VulkanEditorGraphicsBridge &bridge, const ShaderResources &program,
                                                        EditorViewportExtent extent, Render::FrameToken frame, const DrawSlot &slot,
                                                        const Render::RenderStaticMeshInstance &instance,
                                                        const Render::RenderSceneView &scene);
        };

        [[nodiscard]] Result<void> CreateOverlaySlots();
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
        [[nodiscard]] Result<void> ExecuteViewport(Render::RenderFrameScope &frame, const Render::StaticMeshPassDescriptor &pass);
        const std::thread::id owner_{std::this_thread::get_id()};
        Render::RenderFrontend &frontend_;
        Render::Detail::VulkanEditorGraphicsBridge &bridge_;
        ShaderResources shader_;
        ShaderResources xrShader_;
        std::shared_ptr<XrGraph> xrGraph_;
        std::array<std::array<DrawSlot, Render::RenderMaterialPass::MaximumDraws>, 8> xrDrawSlots_;
        EditorViewportResources resources_;
        Render::Detail::VulkanEditorDevice device_;
        std::array<FrameSlot, 8> slots_;
        std::array<Render::RenderMaterialDraw, Render::RenderMaterialPass::MaximumDraws> preparedDraws_;
        std::array<Render::MeshVertex, MaximumOverlayVertices> overlayVertices_;
        std::optional<EmptyGraph> emptyGraph_;
        Render::RenderTextureHandle emptyGraphTexture_;
        EditorViewportExtent requestedExtent_;
        EditorViewportGridOptions requestedGrid_;
        EditorViewportLightVisualizerOptions requestedLightVisualizer_;
        bool initialized_{};
        bool childAttached_{};
        bool recordedEver_{};
    };
}  // namespace Horo::Editor
