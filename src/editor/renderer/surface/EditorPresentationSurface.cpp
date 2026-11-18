#include "EditorPresentationSurface.h"

#include "Horo/Runtime/Render/RenderPresentationOutput.h"
#include "Horo/Runtime/Render/RenderSurfaceLifecycleErrors.h"

#include <limits>
#include <utility>

namespace Horo::Editor {
    /** @copydoc EditorPresentationSurface::Create */
    Result<EditorPresentationSurface> EditorPresentationSurface::Create(const std::uint64_t owner, EditorSurfaceRealizer &realizer) {
        auto created = Render::RenderSurfaceLifecycle::Create(owner);
        if (created.HasError())
            return Result<EditorPresentationSurface>::Failure(created.ErrorValue());
        return Result<EditorPresentationSurface>::Success(EditorPresentationSurface{std::move(created).Value(), realizer});
    }

    /** @copydoc EditorPresentationSurface::EditorPresentationSurface */
    EditorPresentationSurface::EditorPresentationSurface(Render::RenderSurfaceLifecycle surface, EditorSurfaceRealizer &realizer) noexcept
        : surface_(std::move(surface)), realizer_(&realizer) {}

    /** @copydoc EditorPresentationSurface::Begin */
    Result<Render::RenderSurfaceTransition> EditorPresentationSurface::Begin(const Render::RenderSurfaceCommandKind kind,
                                                                             std::optional<Render::RenderSurfaceConfiguration> target) {
        if (sequence_ == (std::numeric_limits<std::uint64_t>::max)())
            return Result<Render::RenderSurfaceTransition>::Failure(
                MakeError(Render::RenderSurfaceLifecycleErrors::RevisionExhausted, "Host surface sequence is exhausted."));
        if (const auto queued = surface_.Queue({++sequence_, kind, std::move(target)}); queued.HasError())
            return Result<Render::RenderSurfaceTransition>::Failure(queued.ErrorValue());
        return surface_.BeginFrameBoundary();
    }

    /** @copydoc EditorPresentationSurface::Update */
    Result<Render::RenderSurfaceSnapshot> EditorPresentationSurface::Update(const std::optional<Render::FramebufferExtent> extent,
                                                                            const Render::ResolvedPresentMode mode,
                                                                            const EditorSurfaceAdmission admission) {
        auto observed = Observe(extent, mode, admission, EditorSurfaceRecovery::Retry, observedDisplayRevision_);
        if (observed.HasError())
            return Result<Render::RenderSurfaceSnapshot>::Failure(observed.ErrorValue());
        if (observed.Value().nativeFailure)
            return Result<Render::RenderSurfaceSnapshot>::Failure(*observed.Value().nativeFailure);
        return Result<Render::RenderSurfaceSnapshot>::Success(std::move(observed).Value().snapshot);
    }

    /** @copydoc EditorPresentationSurface::Observe */
    Result<EditorSurfaceObservation> EditorPresentationSurface::Observe(const std::optional<Render::FramebufferExtent> extent,
                                                                        const Render::ResolvedPresentMode mode,
                                                                        const EditorSurfaceAdmission admission,
                                                                        const EditorSurfaceRecovery recovery,
                                                                        const std::uint64_t displayRevision) {
        auto current = surface_.Snapshot();
        if (current.HasError())
            return Result<EditorSurfaceObservation>::Failure(current.ErrorValue());
        if (const auto admitted = ValidateAdmission(mode, admission); admitted.HasError())
            return Result<EditorSurfaceObservation>::Failure(admitted.ErrorValue());
        if (displayRevision == 0 || displayRevision < observedDisplayRevision_)
            return Result<EditorSurfaceObservation>::Failure(
                MakeError(Render::RenderSurfaceLifecycleErrors::InvalidCommand, "Window display revision is missing or stale."));
        if (extent && !extent->IsValid())
            return Result<EditorSurfaceObservation>::Failure(MakeError(Render::RenderSurfaceLifecycleErrors::InvalidCommand,
                                                                       "A drawable observation with pixels requires a valid extent."));
        observedDisplayRevision_ = displayRevision;
        const auto target = extent ? std::optional{Render::RenderSurfaceConfiguration{*extent, mode, displayRevision}} : std::nullopt;
        bool replacedPending = false;
        if (pending_) {
            if (pending_->command.target == target)
                return CompletePending();
            if (auto cancelled = CancelPending(); cancelled.HasError())
                return Result<EditorSurfaceObservation>::Failure(cancelled.ErrorValue());
            current = surface_.Snapshot();
            if (current.HasError())
                return Result<EditorSurfaceObservation>::Failure(current.ErrorValue());
            replacedPending = true;
        }
        if (!target)
            return Suspend(current.Value());
        if (current.Value().state == Render::RenderSurfaceState::Lost && recovery == EditorSurfaceRecovery::Defer && !replacedPending)
            return Result<EditorSurfaceObservation>::Success({current.Value(), std::nullopt});
        if (current.Value().state == Render::RenderSurfaceState::Ready && current.Value().active == target)
            return Result<EditorSurfaceObservation>::Success({current.Value(), std::nullopt});
        return Realize(*target, current.Value().state);
    }

    /** @copydoc EditorPresentationSurface::ValidateAdmission */
    Result<void> EditorPresentationSurface::ValidateAdmission(const Render::ResolvedPresentMode mode,
                                                              const EditorSurfaceAdmission admission) const {
        if (closed_ || admission != EditorSurfaceAdmission::Idle)
            return Result<void>::Failure(MakeError(Render::RenderSurfaceLifecycleErrors::InvalidState,
                                                   "Surface work requires an open, idle host without cancellation."));
        if (admittedMode_ && *admittedMode_ != mode)
            return Result<void>::Failure(MakeError(Render::RenderSurfaceLifecycleErrors::InvalidCommand,
                                                   "Present-mode changes require fresh native negotiation before surface realization."));
        return Result<void>::Success();
    }

    /** @copydoc EditorPresentationSurface::Suspend */
    Result<EditorSurfaceObservation> EditorPresentationSurface::Suspend(const Render::RenderSurfaceSnapshot &current) {
        if (current.state == Render::RenderSurfaceState::Suspended || current.state == Render::RenderSurfaceState::Lost)
            return Result<EditorSurfaceObservation>::Success({current, std::nullopt});
        const auto kind = current.state == Render::RenderSurfaceState::Unattached ? Render::RenderSurfaceCommandKind::Attach
                                                                                  : Render::RenderSurfaceCommandKind::Suspend;
        auto transition = Begin(kind, std::nullopt);
        if (transition.HasError())
            return Result<EditorSurfaceObservation>::Failure(transition.ErrorValue());
        auto completed = surface_.Complete(transition.Value(), Render::RenderSurfaceRealization::Suspended);
        if (completed.HasError())
            return Result<EditorSurfaceObservation>::Failure(completed.ErrorValue());
        return Result<EditorSurfaceObservation>::Success({std::move(completed).Value(), std::nullopt});
    }

    /** @copydoc EditorPresentationSurface::Realize */
    Result<EditorSurfaceObservation> EditorPresentationSurface::Realize(const Render::RenderSurfaceConfiguration &target,
                                                                        const Render::RenderSurfaceState state) {
        auto kind = Render::RenderSurfaceCommandKind::Resize;
        if (state == Render::RenderSurfaceState::Unattached)
            kind = Render::RenderSurfaceCommandKind::Attach;
        else if (state == Render::RenderSurfaceState::Lost)
            kind = Render::RenderSurfaceCommandKind::Recover;
        auto transition = Begin(kind, target);
        if (transition.HasError())
            return Result<EditorSurfaceObservation>::Failure(transition.ErrorValue());
        admittedMode_ = target.presentMode;
        pending_ = transition.Value();
        return CompletePending();
    }

    /** @copydoc EditorPresentationSurface::CancelPending */
    Result<void> EditorPresentationSurface::CancelPending() {
        if (!pending_)
            return Result<void>::Success();
        const auto completed = surface_.Complete(*pending_, Render::RenderSurfaceRealization::Lost);
        if (completed.HasError())
            return Result<void>::Failure(completed.ErrorValue());
        pending_.reset();
        return Result<void>::Success();
    }

    /** @copydoc EditorPresentationSurface::CompletePending */
    Result<EditorSurfaceObservation> EditorPresentationSurface::CompletePending() {
        const auto realized = realizer_->Resize(pending_->command.target->extent);
        if (realized.HasError() &&
            realized.ErrorValue().domain.Value() == Render::RenderPresentationErrors::DrawableUnavailable.domain.Value() &&
            realized.ErrorValue().code.Value() == Render::RenderPresentationErrors::DrawableUnavailable.code.Value()) {
            const auto snapshot = surface_.Snapshot();
            return snapshot.HasError() ? Result<EditorSurfaceObservation>::Failure(snapshot.ErrorValue())
                                       : Result<EditorSurfaceObservation>::Success({snapshot.Value(), std::nullopt});
        }
        auto completed = surface_.Complete(*pending_, realized.HasValue() ? Render::RenderSurfaceRealization::Ready
                                                                          : Render::RenderSurfaceRealization::Lost);
        if (completed.HasError())
            return Result<EditorSurfaceObservation>::Failure(realized.HasError() ? WithCause(completed.ErrorValue(), realized.ErrorValue())
                                                                                 : completed.ErrorValue());
        pending_.reset();
        return Result<EditorSurfaceObservation>::Success(
            {std::move(completed).Value(), realized.HasError() ? std::optional{realized.ErrorValue()} : std::nullopt});
    }

    /** @copydoc EditorPresentationSurface::Close */
    Result<Render::RenderSurfaceSnapshot> EditorPresentationSurface::Close(const EditorSurfaceAdmission admission) {
        auto current = surface_.Snapshot();
        if (current.HasError() || closed_)
            return current;
        if (admission == EditorSurfaceAdmission::FrameActive)
            return Result<Render::RenderSurfaceSnapshot>::Failure(
                MakeError(Render::RenderSurfaceLifecycleErrors::InvalidState, "Cancel the active frame before closing its surface."));
        if (auto cancelled = CancelPending(); cancelled.HasError())
            return Result<Render::RenderSurfaceSnapshot>::Failure(cancelled.ErrorValue());
        current = surface_.Snapshot();
        if (current.HasError())
            return current;
        // An unattached host has no surface generation to close.
        if (current.Value().state == Render::RenderSurfaceState::Unattached) {
            closed_ = true;
            return current;
        }
        auto transition = Begin(Render::RenderSurfaceCommandKind::Close, std::nullopt);
        if (transition.HasError())
            return Result<Render::RenderSurfaceSnapshot>::Failure(transition.ErrorValue());
        auto completed = surface_.Complete(transition.Value(), Render::RenderSurfaceRealization::Unattached);
        if (completed.HasValue())
            closed_ = true;
        return completed;
    }

    /** @copydoc EditorPresentationSurface::Snapshot */
    Result<Render::RenderSurfaceSnapshot> EditorPresentationSurface::Snapshot() const {
        return surface_.Snapshot();
    }
}  // namespace Horo::Editor
