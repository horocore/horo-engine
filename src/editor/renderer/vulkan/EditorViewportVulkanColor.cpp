#include "ColorOutputPackage.h"
#include "EditorViewportColorPipeline.h"
#include "EditorViewportRendererVulkan.h"
#include "RenderFrontendResourceAccess.h"

#include <algorithm>
#include <utility>

namespace Horo::Editor {
    /** @copydoc EditorViewportRendererVulkan::PrepareSpatialKernel */
    Result<std::shared_ptr<Render::IResidentNativeReconstructionKernel>> EditorViewportRendererVulkan::PrepareSpatialKernel(
        const Render::CookedNativeReconstructionKernel *kernel) {
        using namespace Render;
        using Outcome = Result<std::shared_ptr<IResidentNativeReconstructionKernel>>;
        if (!kernel)
            return Outcome::Success({});
        if (kernel->entry != NativeReconstructionEntry::Spatial ||
            !frontend_.Capabilities().support.features.Supports(RenderCapability::NativeReconstruction))
            return Outcome::Failure(MakeError(NativeReconstructionErrors::Unsupported));
        const auto assigned = Detail::RenderFrontendResourceAccess::QueueAssignments(frontend_);
        if (assigned.HasError())
            return Outcome::Failure(assigned.ErrorValue());
        const auto compute = std::ranges::find(assigned.Value(), RenderQueueRole::Compute, &RenderQueueAssignment::role);
        if (compute == assigned.Value().end() || compute->queue != device_.graphicsIdentity)
            return Outcome::Failure(MakeError(NativeReconstructionErrors::Unsupported));
        return frontend_.RealizeNativeReconstructionKernel(*kernel);
    }

    /** @copydoc EditorViewportRendererVulkan::PrepareSpatialDispatch */
    Result<Render::NativeReconstructionDispatchPlan> EditorViewportRendererVulkan::PrepareSpatialDispatch(
        Render::FrameToken frame, const Render::StaticMeshPassDescriptor &pass) const {
        using namespace Render;
        using Outcome = Result<NativeReconstructionDispatchPlan>;
        const auto scene = resources_.SceneColorDescriptor();
        const auto output = resources_.SpatialColorDescriptor();
        // Display overlays currently load the exact scene depth at the display extent.
        if (scene.extent != pass.extent || output.extent != scene.extent)
            return Outcome::Failure(MakeError(ReconstructionErrors::IncompatibleOutput));
        const auto &capabilities = frontend_.Capabilities().support;
        const auto view = LightingViewIdentity();
        const auto color = resources_.ColorTexture();
        const ReconstructionFrameRequest
            request{.kind = ReconstructionKind::Spatial,
                    .colorStage = ReconstructionColorStage::PreExposed,  // The scene is rendered at unity scale; manual exposure follows.
                    .compatibility = {.view = view,
                                      .renderExtent = scene.extent,
                                      .targetExtent = output.extent,
                                      .deviceGeneration = capabilities.deviceIncarnation},
                    .resourceOwner = color.owner,
                    .timing = {.frameId = frame.value},
                    .projection = pass.scene.camera.projection,
                    .color = {color, scene, view, frame.value},
                    .output = {.descriptor = output,
                               .lifetime = RenderGraphResourceClass::Persistent,
                               .writeUsage = RenderGraphUsageKind::Storage,
                               .resident = resources_.SpatialTexture()}};
        return PrepareNativeReconstructionDispatch(request, capabilities, {});
    }

    /** @copydoc EditorViewportRendererVulkan::MakeViewportColor */
    Result<std::unique_ptr<EditorViewportColorPipeline>> EditorViewportRendererVulkan::MakeViewportColor(
        const EditorViewportColorInputs &inputs, const Render::ExposureSettings &settings) {
        using Outcome = Result<std::unique_ptr<EditorViewportColorPipeline>>;
        if (!inputs.package || inputs.package->target.backend != Render::ShaderTargetBackend::Vulkan || !bridge_.Port())
            return Outcome::Failure(MakeError(Render::ExposureGpuErrors::Unsupported));
        const auto output = frontend_.PresentationOutput();
        if (output.HasError())
            return Outcome::Failure(output.ErrorValue());
        const auto resources = EditorViewportColorResourcePort{this, &ObserveColorBuffer, &WriteColorBuffer};
        auto candidate = std::make_unique<EditorViewportColorPipeline>(frontend_, resources, device_.frames);
        if (const auto prepared = candidate->Initialize(inputs); prepared.HasError())
            return Outcome::Failure(prepared.ErrorValue());
        if (const auto requested = candidate->RequestSettings(settings); requested.HasError())
            return Outcome::Failure(requested.ErrorValue());
        return Outcome::Success(std::move(candidate));
    }

    /** @copydoc EditorViewportRendererVulkan::ObserveColorBuffer */
    Result<EditorViewportColorReadiness> EditorViewportRendererVulkan::ObserveColorBuffer(void *context,
                                                                                          Render::RenderBufferHandle buffer) {
        auto &owner = *static_cast<EditorViewportRendererVulkan *>(context);
        const auto instance = Render::Detail::RenderFrontendResourceAccess::BackendInstance(owner.frontend_, buffer);
        using Outcome = Result<EditorViewportColorReadiness>;
        if (instance.HasError())
            return Outcome::Failure(instance.ErrorValue());
        const auto ready = owner.bridge_.Port()->ObservePreparationBuffer(instance.Value());
        if (ready.HasError())
            return Outcome::Failure(ready.ErrorValue());
        return Outcome::Success(ready.Value() ? EditorViewportColorReadiness::Ready : EditorViewportColorReadiness::Pending);
    }

    /** @copydoc EditorViewportRendererVulkan::WriteColorBuffer */
    Result<void> EditorViewportRendererVulkan::WriteColorBuffer(void *context, Render::RenderBufferHandle buffer,
                                                                std::span<const std::byte> bytes) {
        auto &owner = *static_cast<EditorViewportRendererVulkan *>(context);
        const auto instance = Render::Detail::RenderFrontendResourceAccess::BackendInstance(owner.frontend_, buffer);
        if (instance.HasError())
            return Result<void>::Failure(instance.ErrorValue());
        return owner.bridge_.Port()->WritePreparationUniform(instance.Value(), bytes);
    }

    /** @copydoc EditorViewportRendererVulkan::SceneProgram */
    const EditorViewportRendererVulkan::ShaderResources &EditorViewportRendererVulkan::SceneProgram() const noexcept {
        return color_ ? sceneLinearShader_ : shader_;
    }

    /** @copydoc EditorViewportRendererVulkan::PrepareViewportColor */
    Result<void> EditorViewportRendererVulkan::PrepareViewportColor(Render::RenderViewId view, Render::FramebufferExtent extent) {
        if (const auto ready = color_->Prepare(view, extent, colorFrameDelta_); ready.HasError())
            return ready;
        const auto bound = color_->PrepareBindings(spatial_ ? resources_.SpatialTextureView() : resources_.ColorTextureView(),
                                                   resources_.DisplayTextureView());
        if (bound.HasError())
            color_->Finish(false);
        return bound;
    }

    /** @copydoc EditorViewportRendererVulkan::RecordViewportColor */
    void EditorViewportRendererVulkan::RecordViewportColor(Render::RenderTimelinePoint signal) noexcept {
        pendingColorSubmission_ = {signal, resources_.PresentationTextureView(), frontend_.Capabilities().support.deviceIncarnation};
        color_->Encoded();
    }

    /** @copydoc EditorViewportRendererVulkan::FinishColorFrame */
    void EditorViewportRendererVulkan::FinishColorFrame(const bool presented) noexcept {
        if (temporalGuides_)
            temporalGuides_->Finish(presented);
        if (const auto signal = imageCapture_.UnconfirmedSignal(); signal.IsValid()) {
            const auto *port = bridge_.Port();
            bool accepted = false;
            if (port) {
                const auto submitted = port->WasSubmissionAccepted(signal, frontend_.Capabilities().support.deviceIncarnation);
                accepted = submitted.HasValue() && submitted.Value();
            }
            imageCapture_.Finish(accepted);
        }
        const auto submitted = std::exchange(pendingColorSubmission_, {});
        if (!color_)
            return;
        if (!submitted.signal.IsValid() || !bridge_.Port()) {
            color_->Finish(false);
            return;
        }
        const auto accepted = bridge_.Port()->WasSubmissionAccepted(submitted.signal, submitted.deviceIncarnation);
        const bool committed = accepted.HasValue() && accepted.Value();
        color_->Finish(committed);
        if (committed)
            acceptedColorView_ = submitted.output;
    }

    /** @copydoc EditorViewportRendererVulkan::RequestColorSettings */
    Result<void> EditorViewportRendererVulkan::RequestColorSettings(const Render::ExposureSettings &settings) {
        if (owner_ != std::this_thread::get_id() || !color_)
            return Result<void>::Failure(MakeError(Render::ExposureGpuErrors::Unsupported));
        return color_->RequestSettings(settings);
    }

    /** @copydoc EditorViewportRendererVulkan::SetColorFrameDelta */
    void EditorViewportRendererVulkan::SetColorFrameDelta(float deltaSeconds) noexcept {
        colorFrameDelta_ = deltaSeconds;
    }

    /** @copydoc EditorViewportRendererVulkan::ColorDiagnostics */
    EditorViewportColorDiagnostics EditorViewportRendererVulkan::ColorDiagnostics() const noexcept {
        return color_ ? color_->Diagnostics() : EditorViewportColorDiagnostics{};
    }
}  // namespace Horo::Editor
