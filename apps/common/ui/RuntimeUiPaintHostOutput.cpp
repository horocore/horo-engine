#include "RuntimeUiPaintHostInternal.h"

#include <limits>

namespace Horo::Application::Internal {
    using namespace Runtime::Ui;

    /** @copydoc RuntimeUiPaintPort::UpdateOutput */
    Result<void> RuntimeUiPaintPort::UpdateOutput(const UiCanvasViewportEvidence &evidence) const {
        if (!state_ || !state_->Available() || state_->dispatching)
            return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if (!evidence.IsValid())
            return Result<void>::Failure(MakeError(UiErrors::CanvasSpaceInvalid));
        const auto sources = state_->Sources();
        for (const auto *source : sources) {
            if (!source)
                continue;
            const auto resolved = ResolveUiScreenCanvasWithEvidence(source->output.canvas, evidence);
            if (resolved.HasError())
                return Result<void>::Failure(resolved.ErrorValue());
        }
        state_->output = evidence;
        for (auto *source : sources) {
            if (!source || source->output.pending == evidence || (!source->output.pending && source->output.current == evidence))
                continue;
            source->output.pending = evidence;
            source->output.awaitingPresentation = true;
            source->ReleaseFrame();
        }
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
