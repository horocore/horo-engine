#include "EditorViewportRendererVulkan.h"

#include "EditorViewportColorPipeline.h"
#include "Horo/Runtime/Render/ShaderCompilerPipelineErrors.h"
#include "RenderFrontendResourceAccess.h"
#include "editor/renderer/EditorRendererErrors.h"

#include <algorithm>
#include <limits>

namespace Horo::Editor {
    namespace {
        bool IsViewportReloadFamily(const std::span<const Render::ShaderHotReloadProgramView> programs) {
            return programs.size() == 2 && programs[0].id.value == 1 && programs[1].id.value == 3 && programs[0].manifest &&
                   programs[0].compiled && programs[1].manifest && programs[1].compiled;
        }

        Result<EditorVulkanViewportCook> ReloadViewportCook(const Render::ShaderCompilationBatch &batch) {
            if (batch.artifacts.size() != 1 || !batch.artifacts.front().reflection)
                return Result<EditorVulkanViewportCook>::Failure(MakeError(Render::ShaderCompilerPipelineErrors::InvalidAdapterOutput));
            const auto &artifact = batch.artifacts.front();
            const auto valid = Render::ValidateReflectedShaderArtifact(artifact, *artifact.reflection);
            if (valid.HasError())
                return Result<EditorVulkanViewportCook>::Failure(valid.ErrorValue());
            const auto metadata = Render::EncodeReflectedShaderArtifact(*artifact.reflection);
            if (metadata.HasError())
                return Result<EditorVulkanViewportCook>::Failure(metadata.ErrorValue());
            return LoadEditorVulkanViewportCook(artifact.payload, metadata.Value());
        }

        Result<EditorVulkanShadowCook> ReloadShadowCook(const Render::ShaderCompilationBatch &batch) {
            if (batch.artifacts.size() != 1 || !batch.artifacts.front().reflection)
                return Result<EditorVulkanShadowCook>::Failure(MakeError(Render::ShaderCompilerPipelineErrors::InvalidAdapterOutput));
            const auto &artifact = batch.artifacts.front();
            if (auto valid = Render::ValidateReflectedShaderArtifact(artifact, *artifact.reflection); valid.HasError())
                return Result<EditorVulkanShadowCook>::Failure(valid.ErrorValue());
            const auto encoded = Render::EncodeReflectedShaderArtifact(*artifact.reflection);
            if (encoded.HasError())
                return Result<EditorVulkanShadowCook>::Failure(encoded.ErrorValue());
            return LoadEditorVulkanShadowCook(artifact.payload, encoded.Value());
        }

        /** @brief Stable private GUI name; exact view generation is always carried separately by the existing viewport owner. */
        Result<std::uintptr_t> ViewportImage(const Render::RenderFrontend &frontend, Render::RenderTextureViewHandle view) {
            const auto ready = Render::Detail::RenderFrontendResourceAccess::BackendInstance(frontend, view);
            if (ready.HasError())
                return Result<std::uintptr_t>::Failure(ready.ErrorValue());
            return Result<std::uintptr_t>::Success(std::numeric_limits<std::uintptr_t>::max());
        }
    }  // namespace

    /** @brief Rollback owner for a complete unpublished desktop program and every dependent per-frame resource. */
    struct EditorViewportRendererVulkan::ProgramGeneration final : Render::IShaderHotReloadCandidate {
        explicit ProgramGeneration(EditorViewportRendererVulkan &target) noexcept : owner(target) {}

        ~ProgramGeneration() {
            if (crossFadeSlots)
                owner.RetireCameraCrossFadeSlots(*crossFadeSlots, 0, owner.crossFadeDrawCount_);
            owner.RetireProgram(program, slots);
            owner.RetireProgram(shadowProgram, {});
            owner.RetireProgram(sceneLinearProgram, {});
        }

        EditorViewportRendererVulkan &owner;
        Render::ShaderHotReloadTargetState expected;
        ShaderResources program;
        ShaderResources shadowProgram;
        ShaderResources sceneLinearProgram;
        std::array<FrameSlot, 8> slots;
        std::unique_ptr<FrameSlots> crossFadeSlots;
    };

    Result<void> EditorViewportRendererVulkan::PrepareProgram(ProgramGeneration &candidate) {
        const bool displayLinear = !candidate.sceneLinearProgram.payload.empty();
        if (const auto pipeline = ProgramResources::CreatePipeline(frontend_, candidate.program, device_.frames, displayLinear);
            pipeline.HasError())
            return pipeline;
        if (displayLinear)
            if (const auto scene = ProgramResources::CreateSceneLinearPipeline(frontend_, candidate.sceneLinearProgram, device_.frames);
                scene.HasError())
                return scene;
        const auto &sceneProgram = displayLinear ? candidate.sceneLinearProgram : candidate.program;
        if (const auto slots =
                ProgramResources::CreateDrawSlots(frontend_, sceneProgram, candidate.slots, device_.frames, resources_.ShadowTextureView());
            slots.HasError())
            return slots;
        if (auto overlay = CreateOverlaySlots(candidate.program, candidate.slots); overlay.HasError())
            return overlay;
        if (auto shadow = PrepareShadowProgram(candidate.shadowProgram, candidate.slots); shadow.HasError())
            return shadow;
        if (crossFadeSourceSlots_) {
            candidate.crossFadeSlots = std::make_unique<FrameSlots>();
            return PrepareCameraCrossFadeSlots(*candidate.crossFadeSlots, sceneProgram, candidate.shadowProgram, 0, crossFadeDrawCount_);
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::PrepareManualColor */
    Result<void> EditorViewportRendererVulkan::PrepareManualColor(EditorVulkanSceneLinearCook scene,
                                                                  const EditorViewportColorInputs &inputs,
                                                                  const Render::ExposureSettings &settings,
                                                                  const Render::CookedNativeReconstructionKernel *spatial) {
        if (!initialized_ || recordedEver_ || color_ || resources_.ColorTexture().IsValid() ||
            !frontend_.Capabilities().supportsMaterialRasterGraph || scene.payload.empty() || settings.mode != Render::ExposureMode::Manual)
            return Result<void>::Failure(MakeError(Render::ExposureGpuErrors::Unsupported));
        auto nativeSpatial = PrepareSpatialKernel(spatial);
        if (nativeSpatial.HasError())
            return Result<void>::Failure(nativeSpatial.ErrorValue());
        auto color = MakeViewportColor(inputs, settings);
        if (color.HasError())
            return Result<void>::Failure(color.ErrorValue());
        auto candidate = std::make_unique<ProgramGeneration>(*this);
        candidate->program.payload = shader_.payload;
        candidate->program.metadata = shader_.metadata;
        candidate->shadowProgram.payload = shadowShader_.payload;
        candidate->shadowProgram.metadata = shadowShader_.metadata;
        candidate->sceneLinearProgram.payload = std::move(scene.payload);
        candidate->sceneLinearProgram.metadata = std::move(scene.metadata);
        if (const auto prepared = PrepareProgram(*candidate); prepared.HasError())
            return prepared;
        if (const auto enabled = resources_.EnableColorPipeline(spatial != nullptr); enabled.HasError())
            return enabled;
        AdoptProgram(*candidate);
        color_ = std::move(color).Value();
        spatial_ = std::move(nativeSpatial).Value();
        return Result<void>::Success();
    }

    /** @copydoc EditorViewportRendererVulkan::AdoptProgram */
    void EditorViewportRendererVulkan::AdoptProgram(ProgramGeneration &candidate) noexcept {
        viewportCanvasGraph_.reset();
        viewportRasterGraphs_.reset();
        std::swap(shader_, candidate.program);
        std::swap(shadowShader_, candidate.shadowProgram);
        std::swap(sceneLinearShader_, candidate.sceneLinearProgram);
        std::swap(slots_, candidate.slots);
        std::swap(crossFadeSourceSlots_, candidate.crossFadeSlots);
        boundShadowAtlas_ = resources_.ShadowTextureView();
    }

    void EditorViewportRendererVulkan::RetireDraw(DrawSlot &draw) noexcept {
        draw.material.reset();
        if (draw.buffer.IsValid())
            static_cast<void>(frontend_.ReleaseBuffer(draw.buffer));
        draw = {};
    }

    /** @copydoc EditorViewportRendererVulkan::RetireProgram */
    void EditorViewportRendererVulkan::RetireProgram(ShaderResources &program, std::span<FrameSlot> slots) noexcept {
        for (auto &slot : slots) {
            for (auto &draw : slot.draws)
                RetireDraw(draw);
            for (auto &cascade : slot.shadows)
                for (auto &draw : cascade)
                    RetireDraw(draw);
            auto &overlay = slot.overlay;
            overlay.uniforms.material.reset();
            if (overlay.mesh.IsValid())
                static_cast<void>(frontend_.ReleaseMesh(overlay.mesh));
            for (const auto buffer : {overlay.uniforms.buffer, overlay.vertices, overlay.indices})
                if (buffer.IsValid())
                    static_cast<void>(frontend_.ReleaseBuffer(buffer));
            slot = {};
        }
        if (program.materials)
            static_cast<void>(program.materials->Shutdown());
        program.materials.reset();
        ReleaseProgramObjects(program);
        program = {};
    }

    void EditorViewportRendererVulkan::ReleaseProgramObjects(ShaderResources &program) noexcept {
        for (const auto pipeline : {program.pipeline, program.overlayPipeline})
            if (pipeline.IsValid())
                static_cast<void>(frontend_.ReleaseGraphicsPipeline(pipeline));
        if (program.shadowSampler.IsValid())
            static_cast<void>(frontend_.ReleaseSampler(program.shadowSampler));
        for (const auto shaderModule : {program.vertex, program.fragment})
            if (shaderModule.IsValid())
                static_cast<void>(frontend_.ReleaseShaderModule(shaderModule));
    }

    /** @copydoc EditorViewportRendererVulkan::EditorViewportRendererVulkan */
    EditorViewportRendererVulkan::EditorViewportRendererVulkan(Render::RenderFrontend &frontend,
                                                               Render::Detail::VulkanEditorGraphicsBridge &bridge,
                                                               EditorVulkanViewportCook cooked) noexcept
        : frontend_(frontend), bridge_(bridge), shader_{.payload = std::move(cooked.payload), .metadata = std::move(cooked.metadata)},
          crossFadeCook_(std::move(cooked.cameraCrossFade)),
          resources_(frontend, {.resolveImage = &ViewportImage,
                                .meshBufferAccess = Render::RenderBufferAccess::HostVisible,
                                .prepareDirectionalShadow = true}) {
        if (cooked.shadow) {
            shadowShader_.payload = std::move(cooked.shadow->payload);
            shadowShader_.metadata = std::move(cooked.shadow->metadata);
        }
    }

    EditorViewportRendererVulkan::~EditorViewportRendererVulkan() {
        Shutdown();
    }

    /** @copydoc EditorViewportRendererVulkan::Initialize */
    Result<void> EditorViewportRendererVulkan::Initialize() {
        if (owner_ != std::this_thread::get_id() || initialized_ || !bridge_.Port())
            return Result<void>::Failure(MakeError(RendererErrors::ViewportNotInitialized));
        const auto device = bridge_.Port()->QueryDevice();
        if (device.HasError())
            return Result<void>::Failure(device.ErrorValue());
        device_ = device.Value();
        if (device_.frames == 0 || device_.frames > slots_.size())
            return Result<void>::Failure(MakeError(RendererErrors::ViewportGeometryCreationFailed));
        const auto attached = bridge_.Port()->AttachDeviceChild(*this);
        if (attached.HasError())
            return attached;
        childAttached_ = true;
        auto candidate = std::make_unique<ProgramGeneration>(*this);
        candidate->program.payload = shader_.payload;
        candidate->program.metadata = shader_.metadata;
        candidate->shadowProgram.payload = shadowShader_.payload;
        candidate->shadowProgram.metadata = shadowShader_.metadata;
        if (const auto prepared = PrepareProgram(*candidate); prepared.HasError()) {
            Shutdown();
            return prepared;
        }
        AdoptProgram(*candidate);
        initialized_ = true;
        ++liveGeneration_;
        return Result<void>::Success();
    }

    /** @copydoc Render::IShaderHotReloadTarget::CaptureState */
    Result<Render::ShaderHotReloadTargetState> EditorViewportRendererVulkan::CaptureState() const {
        using Outcome = Result<Render::ShaderHotReloadTargetState>;
        const auto boundary = Render::Detail::RenderFrontendResourceAccess::RequirePreparationBoundary(frontend_);
        if (boundary.HasError())
            return Outcome::Failure(boundary.ErrorValue());
        if (color_)
            return Outcome::Failure(MakeError(Render::ExposureGpuErrors::Unsupported));
        if (!initialized_ || !childAttached_ || !shader_.pipeline.IsValid())
            return Outcome::Failure(MakeError(RendererErrors::ViewportNotInitialized));
        return Outcome::Success({Render::ShaderTargetBackend::Vulkan, boundary.Value().value, liveGeneration_});
    }

    /** @copydoc Render::IShaderHotReloadTarget::Prepare */
    Result<std::unique_ptr<Render::IShaderHotReloadCandidate>> EditorViewportRendererVulkan::Prepare(
        const std::span<const Render::ShaderHotReloadProgramView> programs, const Render::ShaderHotReloadTargetState expected,
        const CancellationToken &cancellation) {
        using Outcome = Result<std::unique_ptr<Render::IShaderHotReloadCandidate>>;
        const auto state = CaptureState();
        if (state.HasError())
            return Outcome::Failure(state.ErrorValue());
        if (state.Value() != expected || !IsViewportReloadFamily(programs))
            return Outcome::Failure(MakeError(Render::ShaderCompilerPipelineErrors::InvalidRequest));
        if (cancellation.IsCancellationRequested())
            return Outcome::Failure(MakeError(Render::ShaderCompilerPipelineErrors::CancellationRequested));
        auto cooked = ReloadViewportCook(*programs.front().compiled);
        if (cooked.HasError())
            return Outcome::Failure(cooked.ErrorValue());
        auto shadowCook = ReloadShadowCook(*programs[1].compiled);
        if (shadowCook.HasError())
            return Outcome::Failure(shadowCook.ErrorValue());
        auto candidate = std::make_unique<ProgramGeneration>(*this);
        candidate->shadowProgram.payload = shadowCook.Value().payload;
        candidate->shadowProgram.metadata = shadowCook.Value().metadata;
        candidate->expected = expected;
        candidate->program.payload = cooked.Value().payload;
        candidate->program.metadata = cooked.Value().metadata;
        if (const auto prepared = PrepareProgram(*candidate); prepared.HasError())
            return Outcome::Failure(prepared.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Outcome::Failure(MakeError(Render::ShaderCompilerPipelineErrors::CancellationRequested));
        return Outcome::Success(std::move(candidate));
    }

    /** @copydoc Render::IShaderHotReloadTarget::Adopt */
    Result<Render::ShaderHotReloadTargetState> EditorViewportRendererVulkan::Adopt(
        std::unique_ptr<Render::IShaderHotReloadCandidate> candidate, const Render::ShaderHotReloadTargetState expected) {
        using Outcome = Result<Render::ShaderHotReloadTargetState>;
        const auto state = CaptureState();
        if (state.HasError())
            return Outcome::Failure(state.ErrorValue());
        auto *generation = dynamic_cast<ProgramGeneration *>(candidate.get());
        if (state.Value() != expected || !generation || &generation->owner != this || generation->expected != expected ||
            liveGeneration_ == std::numeric_limits<std::uint64_t>::max())
            return Outcome::Failure(MakeError(Render::ShaderCompilerPipelineErrors::InvalidRequest));
        AdoptProgram(*generation);
        ++liveGeneration_;
        return Outcome::Success({expected.backend, expected.deviceIncarnation, liveGeneration_});
    }

    void EditorViewportRendererVulkan::ProgramResources::ReleaseMaterials(std::span<FrameSlot> slots) noexcept {
        for (auto &slot : slots) {
            for (auto &draw : slot.draws)
                draw.material.reset();
            slot.overlay.uniforms.material.reset();
            for (auto &cascade : slot.shadows)
                for (auto &draw : cascade)
                    draw.material.reset();
        }
    }

    /** @copydoc EditorViewportRendererVulkan::ReleaseNative */
    void EditorViewportRendererVulkan::ReleaseNative() noexcept {
        if (owner_ != std::this_thread::get_id())
            std::terminate();
        viewportCanvasGraph_.reset();
        viewportRasterGraphs_.reset();
        preparedCrossFade_.reset();
        if (crossFadeSourceSlots_)
            ProgramResources::ReleaseMaterials(*crossFadeSourceSlots_);
        for (auto &draw : crossFadeProgram_.draws)
            draw.material.reset();
        if (crossFadeProgram_.shader.materials)
            static_cast<void>(crossFadeProgram_.shader.materials->Shutdown());
        crossFadeProgram_.shader.materials.reset();
        for (auto &draw : preparedDraws_)
            draw = {};
        for (auto &cascade : preparedShadowDraws_)
            for (auto &draw : cascade)
                draw = {};
        FinishColorFrame(false);
        color_.reset();
        spatial_.reset();
        acceptedColorView_ = {};
        ProgramResources::ReleaseMaterials(slots_);
        if (sceneLinearShader_.materials)
            static_cast<void>(sceneLinearShader_.materials->Shutdown());
        sceneLinearShader_.materials.reset();
        if (shader_.materials)
            static_cast<void>(shader_.materials->Shutdown());
        shader_.materials.reset();
        if (shadowShader_.materials)
            static_cast<void>(shadowShader_.materials->Shutdown());
        shadowShader_.materials.reset();
        for (auto &slot : xrDrawSlots_)
            for (auto &draw : slot)
                draw.material.reset();
        if (xrShader_.materials)
            static_cast<void>(xrShader_.materials->Shutdown());
        xrShader_.materials.reset();
        for (auto &draw : xrMotionDrawSlots_)
            draw.material.reset();
        if (xrMotionShader_.materials)
            static_cast<void>(xrMotionShader_.materials->Shutdown());
        xrMotionShader_.materials.reset();
        for (auto &eye : xrComfort_.eyes)
            eye.material.reset();
        if (xrComfort_.shader.materials)
            static_cast<void>(xrComfort_.shader.materials->Shutdown());
        xrComfort_.shader.materials.reset();
        ReleaseXrCoverageMaterials();
        initialized_ = false;
        childAttached_ = false;
        recordedEver_ = false;
    }

    /** @copydoc EditorViewportRendererVulkan::Shutdown */
    void EditorViewportRendererVulkan::Shutdown() noexcept {
        if (owner_ != std::this_thread::get_id() || (childAttached_ && recordedEver_))
            std::terminate();
        if (childAttached_ && bridge_.Port())
            bridge_.Port()->DetachDeviceChild(*this);
        ReleaseNative();
        ReleaseXrGraph();
        emptyGraph_.reset();
        emptyGraphTexture_ = {};
        resources_.Shutdown();
        RetireCameraCrossFadeProgram(crossFadeProgram_);
        if (crossFadeSourceSlots_) {
            RetireCameraCrossFadeSlots(*crossFadeSourceSlots_, 0, crossFadeDrawCount_);
            crossFadeSourceSlots_.reset();
        }
        crossFadeDrawCount_ = 0;
        crossFadeShadowBudget_ = 0;
        declinedCrossFadeDrawCount_.reset();
        crossFadeSceneReady_ = false;
        auto source = std::move(shader_.payload);
        auto metadata = std::move(shader_.metadata);
        RetireProgram(shader_, slots_);
        RetireProgram(shadowShader_, {});
        RetireProgram(sceneLinearShader_, {});
        shader_.payload = std::move(source);
        shader_.metadata = std::move(metadata);
        RetireXrProgram();
        RetireXrComfortProgram(xrComfort_);
        for (auto &draw : xrMotionDrawSlots_)
            RetireDraw(draw);
        RetireProgram(xrMotionShader_, {});
        RetireProgram(xrCoverageShader_, {});
        xrMotionFormats_.reset();
        xrMotionCook_.reset();
        xrCoverageCook_.reset();
        xrCoverageReductionArtifact_.clear();
        xrCoverageProbe_.reset();
    }

    void EditorViewportRendererVulkan::RetireXrProgram() noexcept {
        for (auto &slot : xrDrawSlots_)
            for (auto &draw : slot) {
                if (draw.buffer.IsValid())
                    static_cast<void>(frontend_.ReleaseBuffer(draw.buffer));
                draw = {};
            }
        if (xrShader_.pipeline.IsValid())
            static_cast<void>(frontend_.ReleaseGraphicsPipeline(xrShader_.pipeline));
        if (xrShader_.vertex.IsValid())
            static_cast<void>(frontend_.ReleaseShaderModule(xrShader_.vertex));
        if (xrShader_.fragment.IsValid())
            static_cast<void>(frontend_.ReleaseShaderModule(xrShader_.fragment));
        xrShader_ = {};
    }

    /** @copydoc IEditorViewportRenderer::PrepareResources */
    Result<std::optional<Render::RenderTargetHandle>> EditorViewportRendererVulkan::PrepareResources(Render::RenderFrontend &frontend,
                                                                                                     const Render::RenderSceneView &scene) {
        if (&frontend != &frontend_ || owner_ != std::this_thread::get_id() || !initialized_)
            return Result<std::optional<Render::RenderTargetHandle>>::Failure(MakeError(RendererErrors::ViewportNotInitialized));
        const auto prepared = resources_.Prepare(scene, requestedExtent_);
        if (prepared.HasError())
            return prepared;
        if (prepared.Value() && !color_ && resources_.ColorTexture() != emptyGraphTexture_) {
            if (const auto clear = CreateEmptyGraph(); clear.HasError())
                return Result<std::optional<Render::RenderTargetHandle>>::Failure(clear.ErrorValue());
        }
        if (prepared.Value()) {
            if (auto bound = BindShadowAtlas(); bound.HasError())
                return Result<std::optional<Render::RenderTargetHandle>>::Failure(bound.ErrorValue());
            if (auto bound = BindCameraCrossFadeSource(); bound.HasError())
                return Result<std::optional<Render::RenderTargetHandle>>::Failure(bound.ErrorValue());
            if (auto budget = PrepareCameraCrossFadeScene(scene.instances.size()); budget.HasError())
                return Result<std::optional<Render::RenderTargetHandle>>::Failure(budget.ErrorValue());
        }
        return prepared;
    }

    void EditorViewportRendererVulkan::RequestExtent(EditorViewportExtent extent) noexcept {
        requestedExtent_ = extent;
    }

    void EditorViewportRendererVulkan::RequestGrid(const EditorViewportGridOptions &options) noexcept {
        requestedGrid_ = options;
    }

    void EditorViewportRendererVulkan::RequestLightVisualizer(const EditorViewportLightVisualizerOptions &options) noexcept {
        requestedLightVisualizer_ = options;
    }

    EditorViewportExtent EditorViewportRendererVulkan::RequestedExtent() const noexcept {
        // Panel requests are next-boundary intent; this frame must keep the complete published target's extent.
        const auto allocated = resources_.AllocatedExtent();
        return requestedExtent_.IsValid() && allocated.IsValid() ? allocated : requestedExtent_;
    }

    Math::ClipDepthRange EditorViewportRendererVulkan::ClipDepthRange() const noexcept {
        return Math::ClipDepthRange::ZeroToOne;
    }

    EditorViewportTextureView EditorViewportRendererVulkan::TextureView() const noexcept {
        if (const auto projection = XrLogicalProjection(); projection.IsValid())
            return projection;
        const auto view = resources_.PresentationTextureView();
        if (!resources_.IsReady() || (color_ && view != pendingColorSubmission_.output && view != acceptedColorView_))
            return {};
        return {.textureId = resources_.ImageIdentity(), .sampledView = view};
    }

    bool EditorViewportRendererVulkan::IsReady() const noexcept {
        return initialized_ && resources_.IsReady();
    }

    Result<void> EditorViewportRendererVulkan::ExecuteStaticMeshPass(const Render::StaticMeshPassDescriptor &) {
        return Result<void>::Failure(MakeError(RendererErrors::ViewportInvalidScene,
                                               "This selected viewport requires its exact frontend prepared-frame material submission."));
    }
}  // namespace Horo::Editor
