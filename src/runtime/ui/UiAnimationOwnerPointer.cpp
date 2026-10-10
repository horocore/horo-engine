#include "UiAnimationOwnerInternal.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Holds the aggregate callback exclusion fence across every error/exception path. */
        class PointerDispatchGuard final {
        public:
            explicit PointerDispatchGuard(bool &flag) noexcept : flag_(flag) {
                flag = true;
            }

            ~PointerDispatchGuard() {
                flag_ = false;
            }

            PointerDispatchGuard(const PointerDispatchGuard &) = delete;
            PointerDispatchGuard &operator=(const PointerDispatchGuard &) = delete;
            PointerDispatchGuard(PointerDispatchGuard &&) = delete;
            PointerDispatchGuard &operator=(PointerDispatchGuard &&) = delete;

        private:
            bool &flag_;
        };
    }  // namespace

    /** @brief Synchronous bridge to the actual aggregate controls; no mutable owner or callback escapes dispatch. */
    class UiPointerControlRoute final : public UiEventHandler {
    public:
        UiPointerControlRoute(UiAnimationOwner &owner, const UiAnimationPointerInput &input, UiEventHandler &observer)
            : owner_(owner), input_(input), observer_(observer) {}

        Result<UiEventResponse> Handle(const UiElementHandle element, const UiEventPhase phase, const UiRoutedEvent &event) override {
            if (!owner_.storage_ || owner_.storage_->stopped || !event.gesture)
                return Result<UiEventResponse>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
            if (!UiAnimationOwner::RouteTargetEligible(*owner_.storage_, event.gesture->source))
                return Result<UiEventResponse>::Failure(MakeError(UiErrors::ControlSourceStale));
            if (phase == UiEventPhase::Target && !UiAnimationOwner::RouteTargetEligible(*owner_.storage_, element))
                return Result<UiEventResponse>::Failure(MakeError(UiErrors::ControlSourceStale));
            // Release/cancel cleanup is lifecycle state, not a preventable application action. Do it even if capture stops routing.
            if (const auto kind = event.gesture->kind;
                (kind == UiGestureKind::Cancel || kind == UiGestureKind::Release || kind == UiGestureKind::PanEnd) &&
                cleanedSequence_ != event.sequence) {
                cleanedSequence_ = event.sequence;
                const bool completedTap = kind == UiGestureKind::Release && completedTapPointer_ == event.gesture->pointer;
                completedTapPointer_ = 0;
                if (!completedTap)
                    if (const auto cleaned = Input(event.gesture->source, UiControlInputKind::Cancel, event); cleaned.HasError())
                        return Result<UiEventResponse>::Failure(cleaned.ErrorValue());
            }
            return observer_.Handle(element, phase, event);
        }

        Result<void> ApplyDefault(const UiElementHandle target, const UiRoutedEvent &event) override {
            if (const auto observed = observer_.ApplyDefault(target, event); observed.HasError())
                return observed;
            if (!event.gesture || !owner_.storage_ || owner_.storage_->stopped)
                return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
            switch (const auto &gesture = *event.gesture; gesture.kind) {
                case UiGestureKind::Press:
                    if (const auto focused = Focus(target, event); focused.HasError())
                        return focused;
                    return Input(target, UiControlInputKind::PointerPress, event);
                case UiGestureKind::Tap:
                case UiGestureKind::DoubleTap:
                    if (gesture.accessible) {
                        if (const auto focused = Input(target, UiControlInputKind::FocusGained, event); focused.HasError())
                            return focused;
                        if (const auto pressed = Input(target, UiControlInputKind::SubmitPress, event); pressed.HasError())
                            return pressed;
                    }
                    if (const auto released =
                            Input(target, gesture.accessible ? UiControlInputKind::SubmitRelease : UiControlInputKind::PointerRelease,
                                  event);
                        released.HasError())
                        return released;
                    completedTapPointer_ = gesture.pointer;
                    return Result<void>::Success();
                case UiGestureKind::LongPress:
                case UiGestureKind::PanBegin:
                case UiGestureKind::DragBegin:
                case UiGestureKind::PinchRotate:
                case UiGestureKind::Drop:
                    return Input(gesture.source, UiControlInputKind::Cancel, event);
                case UiGestureKind::Release:
                case UiGestureKind::PanUpdate:
                case UiGestureKind::PanEnd:
                case UiGestureKind::DragUpdate:
                case UiGestureKind::Cancel:
                case UiGestureKind::HoverEnter:
                case UiGestureKind::HoverMove:
                case UiGestureKind::HoverLeave:
                case UiGestureKind::Count:
                    return Result<void>::Success();
            }
            return Result<void>::Failure(MakeError(UiErrors::EventDispatchInvalid));
        }

        /** @brief Completes accessibility through the same admitted route and cleans the applied prefix on failure. */
        Result<UiPointerInteractionResult> Accessible(const UiPointerInteractionEnvironment &environment, UiFocusGraph *focus,
                                                      const std::span<const UiElementHandle> previous, UiPointerInteractionResult result) {
            const auto focused = focus ? focus->CurrentFocus() : Result<std::optional<UiFocusTarget>>::Success(std::nullopt);
            if (focused.HasValue() && !focused.Value() && *input_.accessible == UiAccessibleGesture::Cancel) {
                input_.interaction.CancelTransient();
                Cleanup(previous);
                result.defaultActions = input_.writtenDefaults;
                return Result<UiPointerInteractionResult>::Success(result);  // Cancellation does not require a focused target.
            }
            if (focused.HasError() || !focused.Value()) {
                Cleanup(previous);
                return Result<UiPointerInteractionResult>::Failure(MakeError(UiErrors::FocusTargetUnavailable));
            }
            auto accessible = input_.interaction.Accessible(environment, focused.Value()->element, *input_.accessible, input_.nextSequence);
            if (accessible.HasError()) {
                Cleanup(previous);
                return accessible;
            }
            result.dispatched += accessible.Value().dispatched;
            result.handled |= accessible.Value().handled;
            result.defaultPrevented |= accessible.Value().defaultPrevented;
            result.defaultActions = input_.writtenDefaults;
            return Result<UiPointerInteractionResult>::Success(result);
        }

        /** @brief Pumps physical transitions, then accessibility while the aggregate pin and exclusion fence remain held. */
        Result<UiPointerInteractionResult> Pump(const UiPointerInteractionEnvironment &environment, UiReloadCanvas &canvas,
                                                const std::span<const UiElementHandle> previous) {
            auto pumped =
                input_.interaction.Pump(environment, input_.canvasSpace, input_.samples, input_.milliseconds, input_.nextSequence);
            if (pumped.HasError()) {
                Cleanup(previous);
                return pumped;
            }
            auto result = std::move(pumped).Value();
            if (input_.accessible)
                return Accessible(environment, canvas.focus ? &*canvas.focus : nullptr, previous, result);
            result.defaultActions = input_.writtenDefaults;
            return Result<UiPointerInteractionResult>::Success(result);
        }

        /** @brief Cancels only controls touched by this ordered call after route failure; preserves applied values/default prefix. */
        void Cleanup(const std::span<const UiElementHandle> previous) {
            CleanupTargets(previous);
            CleanupTargets(touched_);
        }

    private:
        Result<void> Focus(const UiElementHandle target, const UiRoutedEvent &event) {
            auto *canvas = owner_.storage_->publisher.Current()->Canvas(owner_.storage_->definition.canvas);
            if (!canvas->focus)
                return Result<void>::Success();
            std::array<UiFocusTarget, MaximumUiInteractionTargets> eligible{};
            const auto count = canvas->focus->Order(eligible);
            if (count.HasError())
                return Result<void>::Failure(count.ErrorValue());
            if (const auto candidates = std::span{eligible}.first(count.Value());
                std::ranges::find(candidates, target, &UiFocusTarget::element) == candidates.end())
                return Result<void>::Success();  // A semantic hit need not be a focusable control.
            const auto previous = canvas->focus->CurrentFocus();
            if (previous.HasError())
                return Result<void>::Failure(previous.ErrorValue());
            if (const auto changed = canvas->focus->SetFocus(target); changed.HasError())
                return Result<void>::Failure(changed.ErrorValue());
            if (previous.Value() && previous.Value()->element != target)
                if (const auto lost = Input(previous.Value()->element, UiControlInputKind::FocusLost, event); lost.HasError())
                    return lost;
            return Input(target, UiControlInputKind::FocusGained, event);
        }

        void CleanupTargets(const std::span<const UiElementHandle> targets) {
            for (const auto target : targets) {
                if (!target.IsValid())
                    continue;
                const UiRoutedEvent event{UiEventKind::Gesture,
                                          1,
                                          {},
                                          false,
                                          UiGestureEvent{UiGestureKind::Cancel, 0, target, {}, 1.0, 0.0, true}};
                (void)Input(target, UiControlInputKind::Cancel, event);
            }
        }

        Result<void> Input(const UiElementHandle target, const UiControlInputKind kind, const UiRoutedEvent &event) {
            if (!owner_.storage_ || owner_.storage_->stopped || !owner_.storage_->currentFrame.has_value())
                return Result<void>::Failure(MakeError(UiErrors::AnimationLifecycleUnavailable));
            const auto &controls = owner_.storage_->frames[*owner_.storage_->currentFrame]->controls;
            const auto record = std::ranges::find(controls, target, [](const UiAnimationControlRecord &entry) {
                return entry.source.element;
            });
            if (record == controls.end())
                return Result<void>::Success();  // Non-control semantic targets still route through the application's typed default.
            if (input_.nextSequence == 0 || input_.nextSequence == std::numeric_limits<std::uint64_t>::max())
                return Result<void>::Failure(MakeError(UiErrors::EventDispatchCapacityExceeded));
            if (std::ranges::find(touched_, target) == touched_.end()) {
                const auto free = std::ranges::find(touched_, UiElementHandle{});
                if (free == touched_.end())
                    return Result<void>::Failure(MakeError(UiErrors::EventDispatchCapacityExceeded));
                *free = target;
            }
            const UiControlInput controlInput{record->source, kind,
                                              event.gesture && event.gesture->accessible ? UiControlActivationSource::Accessibility
                                                                                         : UiControlActivationSource::Pointer,
                                              input_.nextSequence++};
            const auto control = UiAnimationOwner::PresentedControl(*owner_.storage_, input_.view, record->source);
            if (control.HasError())
                return Result<void>::Failure(control.ErrorValue());
            const auto handled = control.Value()->Handle(controlInput);
            if (handled.HasError())
                return Result<void>::Failure(handled.ErrorValue());
            ++owner_.storage_->commandRevision;
            ++owner_.storage_->pendingCommands;
            if (!handled.Value().defaultActionPending)
                return Result<void>::Success();
            if (input_.writtenDefaults == input_.defaults.size()) {
                (void)owner_.SuppressControlDefault(input_.view, record->source);
                return Result<void>::Failure(MakeError(UiErrors::EventDispatchCapacityExceeded));
            }
            auto applied = owner_.ApplyControlDefault(input_.view, record->source);
            if (applied.HasError())
                return Result<void>::Failure(applied.ErrorValue());
            if (applied.Value())
                input_.defaults[input_.writtenDefaults++] = *applied.Value();
            return Result<void>::Success();
        }

        UiAnimationOwner &owner_;
        const UiAnimationPointerInput &input_;
        UiEventHandler &observer_;
        std::array<UiElementHandle, MaximumUiInteractionSamples + 1> touched_{};
        std::uint64_t cleanedSequence_{};
        std::uint32_t completedTapPointer_{};
    };

    /** @copydoc UiAnimationOwner::PointerInputEligible */
    bool UiAnimationOwner::PointerInputEligible(const UiPointerCaptureContext &source) const noexcept {
        if (!source.IsValid() || !InputEligible(source.view))
            return false;
        const auto &frame = *storage_->frames[*storage_->currentFrame];
        if (!frame.layout)
            return false;
        const auto &layout = frame.layout->Descriptor();
        return source.instance == layout.instance && source.canvas == layout.canvas && source.document == layout.document &&
               source.tree == layout.sources.tree && source.interaction == layout.interaction;
    }

    /** @copydoc UiAnimationOwner::CancelPointers */
    Result<void> UiAnimationOwner::CancelPointers(UiPointerInteraction &interaction, std::uint64_t &nextSequence) {
        const auto targets = interaction.TargetsInFlight();
        const auto source = interaction.Owner();
        interaction.CancelTransient();
        if (!storage_ || storage_->stopped || !storage_->currentFrame.has_value())
            return Result<void>::Success();  // Aggregate shutdown/reload already closes/cancels actual controls.
        const auto &frame = *storage_->frames[*storage_->currentFrame];
        if (frame.controls.size() > MaximumUiInteractionTargets)
            return Result<void>::Failure(MakeError(UiErrors::EventDispatchCapacityExceeded));
        for (const auto target : targets) {
            if (!target.IsValid())
                continue;
            const auto record = std::ranges::find(frame.controls, target, [](const auto &entry) {
                return entry.source.element;
            });
            if (record == frame.controls.end() || record->source.owner.instance != source.instance ||
                record->source.owner.treeRevision != source.tree || record->source.owner.interaction != source.interaction)
                continue;
            if (nextSequence == 0 || nextSequence == std::numeric_limits<std::uint64_t>::max())
                return Result<void>::Failure(MakeError(UiErrors::EventDispatchCapacityExceeded));
            if (const auto cancelled = HandleControl(source.view, {record->source, UiControlInputKind::Cancel,
                                                                   UiControlActivationSource::Pointer, nextSequence++});
                cancelled.HasError())
                return Result<void>::Failure(cancelled.ErrorValue());
        }
        return Result<void>::Success();
    }

    /** @copydoc UiAnimationOwner::PumpPointers */
    Result<UiPointerInteractionResult> UiAnimationOwner::PumpPointers(const UiAnimationPointerInput &input, UiEventHandler &routeHandler) {
        input.writtenDefaults = 0;
        if (!storage_ || storage_->pointerDispatching || !storage_->currentFrame.has_value() ||
            input.defaults.size() < MaximumUiInteractionSamples + 1)
            return Result<UiPointerInteractionResult>::Failure(MakeError(UiErrors::EventDispatchLifecycleUnavailable));
        if (const auto admitted = AdmitCommand(*storage_); admitted.HasError())
            return Result<UiPointerInteractionResult>::Failure(admitted.ErrorValue());
        // Includes long-press defaults and failure cleanup of both previously held and newly touched targets.
        if (const auto commands = input.samples.size() * 4 + MaximumUiInteractionSamples + MaximumUiInteractionPointers * 2 + 5;
            input.samples.size() > MaximumUiInteractionSamples || commands > storage_->limits.commands - storage_->pendingCommands ||
            input.nextSequence == 0 || input.nextSequence > std::numeric_limits<std::uint64_t>::max() - 1024 ||
            storage_->commandRevision > std::numeric_limits<std::uint64_t>::max() - commands)
            return Result<UiPointerInteractionResult>::Failure(MakeError(UiErrors::EventDispatchCapacityExceeded));
        if (!InputEligible(input.view) || storage_->route.gate)
            return Result<UiPointerInteractionResult>::Failure(MakeError(UiErrors::PointerCaptureInteractionStale));
        auto pin = storage_.PublisherPin();  // Shutdown may occur in a callback; guard storage outlives that synchronous borrow.
        const auto &frame = *pin->frames[*pin->currentFrame];
        const auto &owner = input.interaction.Owner();
        if (const auto &layout = frame.layout->Descriptor();
            frame.controls.size() > MaximumUiInteractionTargets || frame.layout->Records().size() > MaximumUiInteractionTargets ||
            owner.instance != layout.instance || owner.canvas != layout.canvas || owner.tree != layout.sources.tree ||
            owner.interaction != layout.interaction || owner.view != input.view)
            return Result<UiPointerInteractionResult>::Failure(MakeError(UiErrors::ControlSourceStale));
        auto *canvas = pin->publisher.Current()->Canvas(pin->definition.canvas);
        const auto presentation = std::ranges::find(canvas->presentations, input.view, &UiPresentedInteractionState::View);
        if (!canvas->captures || presentation == canvas->presentations.end())
            return Result<UiPointerInteractionResult>::Failure(MakeError(UiErrors::PointerCaptureLifecycleUnavailable));
        if (canvas->focus) {
            if (const auto focus = canvas->focus->Snapshot();
                focus.HasError() ||
                input.interaction.ModalRoot() != (focus.Value().modalRoot ? std::optional{focus.Value().modalRoot->element} : std::nullopt))
                return Result<UiPointerInteractionResult>::Failure(MakeError(UiErrors::FocusSourceStale));
        }
        const auto previous = input.interaction.TargetsInFlight();
        PointerDispatchGuard guard{pin->pointerDispatching};
        UiPointerControlRoute bridge{*this, input, routeHandler};
        const UiPointerInteractionEnvironment environment{canvas->tree,      input.hitTesting, *presentation,
                                                          *canvas->captures, input.dispatcher, bridge};
        return bridge.Pump(environment, *canvas, previous);
    }
}  // namespace Horo::Runtime::Ui
