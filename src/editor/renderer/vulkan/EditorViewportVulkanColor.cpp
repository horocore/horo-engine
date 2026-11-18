#include "ColorOutputPackage.h"
#include "EditorViewportColorPipeline.h"
#include "EditorViewportRendererVulkan.h"
#include "RenderFrontendResourceAccess.h"

#include <utility>

namespace Horo::Editor {
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
        auto candidate = std::make_unique<EditorViewportColorPipeline>(frontend_, resources);
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
        const auto bound = color_->PrepareBindings(resources_.ColorTextureView(), resources_.DisplayTextureView());
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
    void EditorViewportRendererVulkan::FinishColorFrame(bool) noexcept {
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
