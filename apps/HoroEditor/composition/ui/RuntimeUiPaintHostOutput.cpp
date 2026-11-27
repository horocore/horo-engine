#include "RuntimeUiPaintHostInternal.h"

#include <limits>
#include <span>

namespace Horo::Application::Internal {
    using namespace Runtime::Ui;

    namespace {
        using Detail::RuntimeUiPaintSource;

        /** @brief Finds the requested screen output, excluding World canvases. */
        const UiCanvasViewportEvidence *SelectOutput(const RuntimeUiPaintSource *source,
                                                     const std::optional<UiCanvasViewportEvidence> &main,
                                                     const std::optional<UiCanvasViewportEvidence> &camera) {
            if (!source || source->worldCanvas)
                return nullptr;
            const auto &selected = source->sceneCanvas ? camera : main;
            return selected ? &*selected : nullptr;
        }

        /** @brief Validates both outputs against the same live source inventory before publication. */
        Result<void> ValidateOutputs(const std::optional<UiCanvasViewportEvidence> &main,
                                     const std::optional<UiCanvasViewportEvidence> &camera,
                                     std::span<RuntimeUiPaintSource *const> sources) {
            if ((main && !main->IsValid()) || (camera && !camera->IsValid()))
                return Result<void>::Failure(MakeError(UiErrors::CanvasSpaceInvalid));
            for (const auto *source : sources) {
                const auto *evidence = SelectOutput(source, main, camera);
                if (!evidence)
                    continue;
                const auto resolved = ResolveUiScreenCanvasWithEvidence(source->output.canvas, *evidence);
                if (resolved.HasError())
                    return Result<void>::Failure(resolved.ErrorValue());
            }
            return Result<void>::Success();
        }

        /** @brief Coalesces an admitted viewport and invalidates the frame when evidence changes. */
        void StageOutput(RuntimeUiPaintSource &source, const UiCanvasViewportEvidence &evidence) {
            const auto &latest = source.output.pending ? source.output.pending : source.output.current;
            if (latest == evidence)
                return;
            source.output.pending = evidence;
            source.output.awaitingPresentation = true;
            source.ReleaseFrame();
        }
    }  // namespace

    /** @copydoc RuntimeUiPaintPort::UpdateOutput */
    Result<void> RuntimeUiPaintPort::UpdateOutput(const UiCanvasViewportEvidence &evidence) const {
        if (!state_ || !state_->Available() || state_->dispatching)
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        auto effective = evidence;
        effective.userUiScale = state_->accessibility.scale;
        effective.minimumControlTargetDips = state_->accessibility.minimumControlTargetDips;
        return state_->QueueOutputs(effective, {});
    }

    /** @copydoc RuntimeUiPaintPort::UpdateCameraOutput */
    Result<void> RuntimeUiPaintPort::UpdateCameraOutput(const UiCanvasPixelExtent extent) const {
        if (!state_ || !state_->Available() || state_->dispatching || !state_->output)
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        auto evidence = *state_->output;
        evidence.pixelExtent = extent;
        evidence.safeAreaInsets = {};
        return state_->QueueOutputs({}, evidence);
    }

    /** @copydoc Detail::RuntimeUiPaintHostState::QueueOutputs */
    Result<void> Detail::RuntimeUiPaintHostState::QueueOutputs(const std::optional<UiCanvasViewportEvidence> &main,
                                                               const std::optional<UiCanvasViewportEvidence> &camera) {
        const auto sources = Sources();
        if (const auto validated = ValidateOutputs(main, camera, sources); validated.HasError())
            return validated;
        if (main)
            output = main;
        if (camera)
            cameraOutput = camera;
        for (auto *source : sources)
            if (const auto *evidence = SelectOutput(source, main, camera))
                StageOutput(*source, *evidence);
        return Result<void>::Success();
    }

    namespace Detail {
        /** @copydoc RuntimeUiPaintSource::ApplyOutput */
        Result<void> RuntimeUiPaintSource::ApplyOutput() {
            if (!output.pending)
                return Result<void>::Success();
            const auto resolved = ResolveUiScreenCanvasWithEvidence(output.canvas, *output.pending);
            if (resolved.HasError())
                return Result<void>::Failure(resolved.ErrorValue());
            auto viewport = output.viewport;
            if (viewport.canvas.Value() == std::numeric_limits<std::uint64_t>::max() ||
                viewport.policy.Value() == std::numeric_limits<std::uint64_t>::max() ||
                (viewport.presentation && viewport.presentation->revision.Value() == std::numeric_limits<std::uint64_t>::max()))
                return Result<void>::Failure(MakeError(UiErrors::GenerationExhausted));
            const auto &canvas = resolved.Value();
            viewport.constraints = {{0, 0}, {canvas.logicalExtent.width, canvas.logicalExtent.height}};
            viewport.content = {{0, 0}, {canvas.logicalExtent.width, canvas.logicalExtent.height}};
            viewport.fontScale = canvas.fontScale;
            viewport.minimumControlTargetDips = output.pending->minimumControlTargetDips;
            viewport.canvas = UiLayoutCanvasRevision::Create(viewport.canvas.Value() + 1).Value();
            viewport.policy = UiLayoutPolicyRevision::Create(viewport.policy.Value() + 1).Value();
            if (viewport.presentation)
                viewport.presentation->revision =
                    UiPresentationEvidenceRevision::Create(viewport.presentation->revision.Value() + 1).Value();
            if (const auto admitted = animation->SetViewport(viewport); admitted.HasError())
                return admitted;
            output.viewport = viewport;
            output.current = output.pending;
            output.pending.reset();
            programs.policy.projection = canvas;
            ReleaseFrame();
            refresh = true;
            return Result<void>::Success();
        }
    }  // namespace Detail
}  // namespace Horo::Application::Internal
