#include "EditorSurfaceLifecycleQualification.h"

#include "Horo/Runtime/Render/RenderSurfaceLifecycleErrors.h"

namespace Horo::Editor {
    namespace {
        /** @brief Keeps qualification failures typed without modifying native context state. */
        Error QualificationError(const char *message) {
            return MakeError(Render::RenderSurfaceLifecycleErrors::InvalidOutcome, message);
        }

        /** @brief Injects one bounded window notification through the real SDL event queue. */
        Result<void> PushWindowEvent(const SDL_EventType type, const SDL_WindowID owner) {
            SDL_Event event{};
            event.window = SDL_WindowEvent{.type = type, .windowID = owner};
            if (!SDL_PushEvent(&event))
                return Result<void>::Failure(QualificationError("Qualification window event could not be queued."));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc EditorSurfaceLifecycleQualification::EditorSurfaceLifecycleQualification */
    EditorSurfaceLifecycleQualification::EditorSurfaceLifecycleQualification(const bool enabled) noexcept : enabled_(enabled) {}

    /** @copydoc EditorSurfaceLifecycleQualification::BeforeResize */
    Result<void> EditorSurfaceLifecycleQualification::BeforeResize() {
        if (!enabled_)
            return Result<void>::Success();
        ++resizeCalls_;
        if (!retainedContext_)
            retainedContext_ = SDL_GL_GetCurrentContext();
        if (stage_ != Stage::Recovery && stage_ != Stage::Close)
            return Result<void>::Failure(WithCause(QualificationError("Injected retained-context surface loss."),
                                                   QualificationError("Injected native resize cause.")));
        return Result<void>::Success();
    }

    /** @copydoc EditorSurfaceLifecycleQualification::BeforeEvents */
    Result<void> EditorSurfaceLifecycleQualification::BeforeEvents(SDL_Window &window) {
        using enum Stage;
        if (!enabled_)
            return Result<void>::Success();
        frameWork_ = 0;
        frameOrderValid_ = true;
        SDL_PumpEvents();
        // This opt-in process isolates scripted window notifications from compositor timing.
        SDL_FlushEvents(SDL_EVENT_WINDOW_FIRST, SDL_EVENT_WINDOW_LAST);
        const auto owner = SDL_GetWindowID(&window);
        if (stage_ == ForeignEvent)
            return PushWindowEvent(SDL_EVENT_WINDOW_EXPOSED, 0);
        if (stage_ == FailedRetry || stage_ == Recovery) {
            if (const auto restored = PushWindowEvent(SDL_EVENT_WINDOW_RESTORED, owner); restored.HasError())
                return restored;
            return PushWindowEvent(SDL_EVENT_WINDOW_EXPOSED, owner);
        }
        if (stage_ == Close) {
            const auto closed = PushWindowEvent(SDL_EVENT_WINDOW_CLOSE_REQUESTED, owner);
            closeQueued_ = closed.HasValue();
            return closed;
        }
        return Result<void>::Success();
    }

    /** @copydoc EditorSurfaceLifecycleQualification::ObserveFailure */
    Result<void> EditorSurfaceLifecycleQualification::ObserveFailure(const Error &error) {
        if (!enabled_)
            return Result<void>::Success();
        ++failureCalls_;
        if (error.domain.Value() != Render::RenderSurfaceLifecycleErrors::InvalidOutcome.domain.Value() ||
            error.code.Value() != Render::RenderSurfaceLifecycleErrors::InvalidOutcome.code.Value() ||
            error.message != "Injected retained-context surface loss." || !error.cause.Get() ||
            error.cause.Get()->message != "Injected native resize cause.")
            return Result<void>::Failure(QualificationError("Real host lost the exact surface failure or its cause."));
        return Result<void>::Success();
    }

    /** @copydoc EditorSurfaceLifecycleQualification::RecordFrameWork */
    void EditorSurfaceLifecycleQualification::RecordFrameWork(const FrameWork work) noexcept {
        if (!enabled_)
            return;
        using enum FrameWork;
        unsigned required{};
        switch (work) {
            case GuiBegin:
                required = static_cast<unsigned>(ViewportResources);
                break;
            case ViewportResources:
                break;
            case Extraction:
                required = static_cast<unsigned>(GuiBegin) | static_cast<unsigned>(ViewportResources);
                break;
            case NativeBegin:
                required = static_cast<unsigned>(GuiBegin) | static_cast<unsigned>(ViewportResources) | static_cast<unsigned>(Extraction);
                break;
            case Execution:
                required = static_cast<unsigned>(NativeBegin);
                break;
            case GuiRender:
                required = static_cast<unsigned>(Execution);
                break;
            case Presentation:
                required = static_cast<unsigned>(GuiRender);
                break;
        }
        const auto bit = static_cast<unsigned>(work);
        frameOrderValid_ = frameOrderValid_ && (frameWork_ & required) == required && (frameWork_ & bit) == 0;
        frameWork_ |= bit;
    }

    /** @copydoc EditorSurfaceLifecycleQualification::RecordPresent */
    void EditorSurfaceLifecycleQualification::RecordPresent(const bool succeeded) noexcept {
        if (enabled_ && succeeded)
            ++presents_;
    }

    /** @copydoc EditorSurfaceLifecycleQualification::EndFrame */
    Result<void> EditorSurfaceLifecycleQualification::EndFrame(const Render::RenderSurfaceSnapshot &snapshot) {
        if (!enabled_)
            return Result<void>::Success();
        if (!frameOrderValid_)
            return Result<void>::Failure(QualificationError("Native admission preceded GUI CPU extraction or a frame phase repeated."));
        if (!ContextRetained())
            return Result<void>::Failure(QualificationError("Recovery replaced or detached the retained native context."));
        if (const unsigned expectedFailures = ExpectedResizeCalls() == 1 ? 1U : 2U;
            resizeCalls_ != ExpectedResizeCalls() || failureCalls_ != expectedFailures)
            return Result<void>::Failure(QualificationError("Real host retried without an owner event or failed to coalesce events."));
        if (stage_ == Stage::Recovery) {
            if (!RecoveredFrameMatches(snapshot))
                return Result<void>::Failure(QualificationError("Owner-window recovery did not resume real native and GUI presentation."));
        } else if (!LostFrameMatches(snapshot)) {
            return Result<void>::Failure(QualificationError("Lost surface admitted viewport, native frame or GUI work."));
        }
        stage_ = static_cast<Stage>(static_cast<unsigned>(stage_) + 1);
        return Result<void>::Success();
    }

    /** @copydoc EditorSurfaceLifecycleQualification::Complete */
    bool EditorSurfaceLifecycleQualification::Complete(const Render::RenderSurfaceSnapshot &snapshot) const noexcept {
        return !enabled_ || (stage_ == Stage::Close && closeQueued_ && ClosedSurfaceMatches(snapshot) && resizeCalls_ == 3 &&
                             failureCalls_ == 2 && presents_ == 1);
    }

    /** @copydoc EditorSurfaceLifecycleQualification::ExpectedResizeCalls */
    unsigned EditorSurfaceLifecycleQualification::ExpectedResizeCalls() const noexcept {
        using enum Stage;
        switch (stage_) {
            case Quiet:
            case ForeignEvent:
                return 1;
            case Recovery:
            case Close:
                return 3;
            case FailedRetry:
            case Deferred:
                return 2;
        }
        return 0;
    }

    /** @copydoc EditorSurfaceLifecycleQualification::ContextRetained */
    bool EditorSurfaceLifecycleQualification::ContextRetained() const noexcept {
        return retainedContext_ && SDL_GL_GetCurrentContext() == retainedContext_;
    }

    /** @copydoc EditorSurfaceLifecycleQualification::RecoveredFrameMatches */
    bool EditorSurfaceLifecycleQualification::RecoveredFrameMatches(const Render::RenderSurfaceSnapshot &snapshot) const noexcept {
        using enum FrameWork;
        constexpr unsigned completeFrame = static_cast<unsigned>(ViewportResources) | static_cast<unsigned>(NativeBegin) |
                                           static_cast<unsigned>(GuiBegin) | static_cast<unsigned>(Extraction) |
                                           static_cast<unsigned>(Execution) | static_cast<unsigned>(GuiRender) |
                                           static_cast<unsigned>(Presentation);
        return snapshot.state == Render::RenderSurfaceState::Ready && snapshot.active.has_value() && frameWork_ == completeFrame &&
               presents_ == 1;
    }

    /** @copydoc EditorSurfaceLifecycleQualification::LostFrameMatches */
    bool EditorSurfaceLifecycleQualification::LostFrameMatches(const Render::RenderSurfaceSnapshot &snapshot) const noexcept {
        return snapshot.state == Render::RenderSurfaceState::Lost && !snapshot.active.has_value() && frameWork_ == 0 && presents_ == 0;
    }

    /** @copydoc EditorSurfaceLifecycleQualification::ClosedSurfaceMatches */
    bool EditorSurfaceLifecycleQualification::ClosedSurfaceMatches(const Render::RenderSurfaceSnapshot &snapshot) const noexcept {
        return snapshot.state == Render::RenderSurfaceState::Unattached && !snapshot.active.has_value() &&
               !snapshot.inFlightSequence.has_value() && ContextRetained();
    }
}  // namespace Horo::Editor
