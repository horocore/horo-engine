#include "UiAnimationOwnerInternal.h"

#include <algorithm>

namespace Horo::Runtime::Ui {
    /** @copydoc UiAnimationOwner::PresentedControl */
    Result<UiControlStateMachine *> UiAnimationOwner::PresentedControl(Storage &storage, const UiRenderViewId view,
                                                                       const UiActionSource &source) {
        if (storage.ownerThread != std::this_thread::get_id() || storage.stopped || storage.draining || storage.candidate.admitted ||
            storage.route.gate || !storage.currentFrame || !storage.publisher.InputEligible(storage.definition.canvas, view))
            return Result<UiControlStateMachine *>::Failure(MakeError(UiErrors::ControlSourceStale));
        const auto &frame = *storage.frames[*storage.currentFrame];
        const auto record = std::ranges::find(frame.controls, source, &UiAnimationControlRecord::source);
        if (record == frame.controls.end() || !RouteTargetEligible(storage, source.element))
            return Result<UiControlStateMachine *>::Failure(MakeError(UiErrors::ControlSourceStale));
        auto *canvas = storage.publisher.Current()->Canvas(storage.definition.canvas);
        const auto control = std::ranges::find(canvas->controls, record->element, &UiReloadControl::id);
        if (control == canvas->controls.end() || control->control.Owner() != source.owner || control->control.Element() != source.element)
            return Result<UiControlStateMachine *>::Failure(MakeError(UiErrors::ControlSourceStale));
        return Result<UiControlStateMachine *>::Success(&control->control);
    }

    /** @copydoc UiAnimationOwner::CapturePointer */
    Result<UiPointerCaptureToken> UiAnimationOwner::CapturePointer(const UiPointerCaptureRequest &request) {
        if (!storage_ || storage_->ownerThread != std::this_thread::get_id() || storage_->stopped || storage_->draining ||
            storage_->candidate.admitted || !storage_->currentFrame)
            return Result<UiPointerCaptureToken>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
        if (storage_->route.gate || !RouteTargetEligible(*storage_, request.route.target) ||
            !storage_->publisher.InputEligible(storage_->definition.canvas, request.view))
            return Result<UiPointerCaptureToken>::Failure(MakeError(UiErrors::PointerCaptureInteractionStale));
        auto *canvas = storage_->publisher.Current()->Canvas(storage_->definition.canvas);
        if (!canvas->captures)
            return Result<UiPointerCaptureToken>::Failure(MakeError(UiErrors::PointerCaptureLifecycleUnavailable));
        const auto presented = std::ranges::find(canvas->presentations, request.view, &UiPresentedInteractionState::View);
        if (presented == canvas->presentations.end())
            return Result<UiPointerCaptureToken>::Failure(MakeError(UiErrors::PointerCaptureSourceStale));
        return canvas->captures->Capture(request, canvas->tree, *presented);
    }

    /** @copydoc UiAnimationOwner::HandleControl */
    Result<UiControlEventResult> UiAnimationOwner::HandleControl(const UiRenderViewId view, const UiControlInput &input) {
        if (!storage_)
            return Result<UiControlEventResult>::Failure(MakeError(UiErrors::ControlLifecycleUnavailable));
        if (auto admitted = AdmitCommand(*storage_); admitted.HasError())
            return Result<UiControlEventResult>::Failure(admitted.ErrorValue());
        const auto control = PresentedControl(*storage_, view, input.source);
        if (control.HasError())
            return Result<UiControlEventResult>::Failure(control.ErrorValue());
        auto handled = control.Value()->Handle(input);
        if (handled.HasError())
            return handled;
        ++storage_->commandRevision;
        ++storage_->pendingCommands;
        return handled;
    }

    /** @copydoc UiAnimationOwner::ApplyControlDefault */
    Result<std::optional<UiControlDefaultAction>> UiAnimationOwner::ApplyControlDefault(const UiRenderViewId view,
                                                                                        const UiActionSource &source) {
        if (!storage_)
            return Result<std::optional<UiControlDefaultAction>>::Failure(MakeError(UiErrors::ControlLifecycleUnavailable));
        const auto control = PresentedControl(*storage_, view, source);
        if (control.HasError())
            return Result<std::optional<UiControlDefaultAction>>::Failure(control.ErrorValue());
        return control.Value()->ApplyDefault();
    }

    /** @copydoc UiAnimationOwner::SuppressControlDefault */
    Result<void> UiAnimationOwner::SuppressControlDefault(const UiRenderViewId view, const UiActionSource &source) {
        if (!storage_)
            return Result<void>::Failure(MakeError(UiErrors::ControlLifecycleUnavailable));
        const auto control = PresentedControl(*storage_, view, source);
        if (control.HasError())
            return Result<void>::Failure(control.ErrorValue());
        return control.Value()->SuppressDefault();
    }
}  // namespace Horo::Runtime::Ui
