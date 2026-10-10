#include "Horo/Runtime/Ui/UiPointerInteraction.h"

#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Converts captured coordinates even outside hit regions, rejecting unrepresentable endpoints. */
        Result<UiLogicalPoint> Position(const UiScreenPointerQuery &query) {
            const auto content = query.canvasSpace.ContentPixelRect();
            const double scale =
                static_cast<double>(query.canvasSpace.pixelsPerDip.logicalDips) * 64.0 / query.canvasSpace.pixelsPerDip.pixelUnits;
            const double x = (static_cast<double>(query.pixelX) - content.x) * scale;
            const double y = (static_cast<double>(query.pixelY) - content.y) * scale;
            if (!std::isfinite(x) || !std::isfinite(y) || x < std::numeric_limits<std::int32_t>::min() ||
                x > std::numeric_limits<std::int32_t>::max() || y < std::numeric_limits<std::int32_t>::min() ||
                y > std::numeric_limits<std::int32_t>::max())
                return Result<UiLogicalPoint>::Failure(MakeError(UiErrors::HitTestInvalid));
            return Result<UiLogicalPoint>::Success({static_cast<std::int32_t>(std::round(x)), static_cast<std::int32_t>(std::round(y))});
        }

        /** @brief Protects synchronous owner admission across handler failures and reentrant calls. */
        class PumpGuard final {
        public:
            explicit PumpGuard(bool &active) noexcept : active_(active) {
                active = true;
            }

            ~PumpGuard() {
                active_ = false;
            }

            PumpGuard(const PumpGuard &) = delete;
            PumpGuard &operator=(const PumpGuard &) = delete;
            PumpGuard(PumpGuard &&) = delete;
            PumpGuard &operator=(PumpGuard &&) = delete;

        private:
            bool &active_;
        };

        /** @brief Rechecks synchronous cancellation before routed defaults, not merely after dispatch has applied them. */
        class LiveGestureRoute final : public UiEventHandler {
        public:
            LiveGestureRoute(UiEventHandler &handler, const bool &stopped, const std::uint64_t &revision,
                             const UiPointerCaptureToken *capture) noexcept
                : handler_(handler), stopped_(stopped), revision_(revision), admittedRevision_(revision), capture_(capture) {}

            Result<UiEventResponse> Handle(const UiElementHandle element, const UiEventPhase phase, const UiRoutedEvent &event) override {
                auto response = handler_.Handle(element, phase, event);
                if (response.HasError())
                    return response;
                if (const auto live = Check(); live.HasError())
                    return Result<UiEventResponse>::Failure(live.ErrorValue());
                return response;
            }

            Result<void> ApplyDefault(const UiElementHandle target, const UiRoutedEvent &event) override {
                if (const auto live = Check(); live.HasError())
                    return live;
                const auto applied = handler_.ApplyDefault(target, event);
                return applied.HasError() ? applied : Check();
            }

        private:
            Result<void> Check() const {
                if (stopped_)
                    return Result<void>::Failure(MakeError(UiErrors::EventDispatchLifecycleUnavailable));
                if (revision_ != admittedRevision_)
                    return Result<void>::Failure(MakeError(UiErrors::EventDispatchRouteInvalidated));
                if (capture_ && !capture_->IsActive())
                    return Result<void>::Failure(MakeError(UiErrors::PointerCaptureSourceStale));
                return Result<void>::Success();
            }

            UiEventHandler &handler_;
            const bool &stopped_;
            const std::uint64_t &revision_;
            const std::uint64_t admittedRevision_;
            const UiPointerCaptureToken *capture_;
        };
    }  // namespace

    /** @copydoc UiPointerInteraction::Create */
    Result<UiPointerInteraction> UiPointerInteraction::Create(const UiPointerInteractionDescriptor &descriptor,
                                                              const std::span<const UiPointerTargetPolicy> targets) {
        if (!descriptor.owner.IsValid() || descriptor.pointerCapacity == 0 || descriptor.pointerCapacity > MaximumUiInteractionPointers ||
            descriptor.dragThreshold <= 0 || descriptor.longPressMilliseconds == 0 || descriptor.longPressMilliseconds > 60'000 ||
            descriptor.doubleTapMilliseconds == 0 || descriptor.doubleTapMilliseconds > 5'000 ||
            (descriptor.modalRoot &&
             (!descriptor.modalRoot->IsValid() || descriptor.modalRoot->ownership != descriptor.owner.instance.ownership)))
            return Result<UiPointerInteraction>::Failure(MakeError(UiErrors::EventDispatchInvalid));
        if (targets.size() > MaximumUiInteractionTargets)
            return Result<UiPointerInteraction>::Failure(MakeError(UiErrors::EventDispatchCapacityExceeded));
        UiPointerInteraction result;
        result.descriptor_ = descriptor;
        for (const auto &target : targets) {
            if (!target.element.IsValid() || target.element.ownership != descriptor.owner.instance.ownership ||
                result.Policy(target.element))
                return Result<UiPointerInteraction>::Failure(MakeError(UiErrors::EventDispatchInvalid));
            result.targets_[result.targetCount_++] = target;
        }
        return Result<UiPointerInteraction>::Success(std::move(result));
    }

    /** @copydoc UiPointerInteraction::~UiPointerInteraction */
    UiPointerInteraction::~UiPointerInteraction() {
        if (pumping_)
            std::terminate();
        Shutdown();
    }

    /** @copydoc UiPointerInteraction::UiPointerInteraction */
    UiPointerInteraction::UiPointerInteraction(UiPointerInteraction &&source) noexcept {
        *this = std::move(source);
    }

    /** @copydoc UiPointerInteraction::operator= */
    UiPointerInteraction &UiPointerInteraction::operator=(UiPointerInteraction &&source) noexcept {
        if (pumping_ || source.pumping_)
            std::terminate();
        if (this != &source) {
            Shutdown();
            descriptor_ = source.descriptor_;
            targets_ = source.targets_;
            targetCount_ = source.targetCount_;
            contacts_ = std::move(source.contacts_);
            accessibleDrag_ = source.accessibleDrag_;
            hovered_ = source.hovered_;
            lastTap_ = source.lastTap_;
            lastTapPosition_ = source.lastTapPosition_;
            lastTapTime_ = source.lastTapTime_;
            time_ = source.time_;
            stopped_ = source.stopped_;
            source.Shutdown();
        }
        return *this;
    }

    /** @copydoc UiPointerInteraction::Shutdown */
    void UiPointerInteraction::Shutdown() noexcept {
        stopped_ = true;
        CancelTransient();
    }

    /** @copydoc UiPointerInteraction::CancelTransient */
    void UiPointerInteraction::CancelTransient() noexcept {
        ++transientRevision_;
        for (auto &contact : contacts_)
            contact = {};
        accessibleDrag_.reset();
        hovered_ = {};
        lastTap_ = {};
    }

    /** @copydoc UiPointerInteraction::TargetsInFlight */
    std::array<UiElementHandle, MaximumUiInteractionPointers + 2> UiPointerInteraction::TargetsInFlight() const noexcept {
        std::array<UiElementHandle, MaximumUiInteractionPointers + 2> result{};
        for (std::size_t index = 0; index < contacts_.size(); ++index)
            if (contacts_[index].capture.State() != UiPointerCaptureState::Inactive)
                result[index] = contacts_[index].capture.Request().route.target;
        result[MaximumUiInteractionPointers] = accessibleDrag_.value_or(UiElementHandle{});
        result.back() = hovered_;
        return result;
    }

    /** @copydoc UiPointerInteraction::Policy */
    const UiPointerTargetPolicy *UiPointerInteraction::Policy(const UiElementHandle target) const noexcept {
        const auto policies = std::span{targets_}.first(targetCount_);
        const auto found = std::ranges::find(policies, target, &UiPointerTargetPolicy::element);
        return found == policies.end() ? nullptr : std::to_address(found);
    }

    /** @copydoc UiPointerInteraction::Validate */
    Result<void> UiPointerInteraction::Validate(const UiPointerInteractionEnvironment &environment) const {
        if (stopped_ || !descriptor_.owner.IsValid())
            return Result<void>::Failure(MakeError(UiErrors::EventDispatchLifecycleUnavailable));
        if (pumping_)
            return Result<void>::Failure(MakeError(UiErrors::EventDispatchReentrant));
        const auto &owner = descriptor_.owner;
        const auto &hit = environment.hitTesting.Descriptor();
        if (environment.tree.State() != UiElementTreeState::Active || environment.tree.Instance() != owner.instance ||
            environment.tree.Canvas() != owner.canvas || environment.tree.SourceDocument() != owner.document ||
            environment.tree.Revision() != owner.tree || hit.instance != owner.instance || hit.canvas != owner.canvas ||
            hit.document != owner.document || hit.tree != owner.tree || hit.interaction != owner.interaction ||
            environment.presented.View() != owner.view || environment.presented.Canvas() != owner.canvas ||
            environment.presented.LastPresentedInteraction() != owner.interaction ||
            environment.captures.Ownership() != owner.instance.ownership)
            return Result<void>::Failure(MakeError(UiErrors::EventDispatchSourceStale));
        return Result<void>::Success();
    }

    /** @copydoc UiPointerInteraction::Emit */
    Result<bool> UiPointerInteraction::Emit(const UiPointerInteractionEnvironment &environment, const UiElementHandle target,
                                            const UiGestureEvent &gesture, const UiLogicalPoint position, std::uint64_t &nextSequence,
                                            UiPointerInteractionResult &result) {
        if (nextSequence == 0 || nextSequence == std::numeric_limits<std::uint64_t>::max())
            return Result<bool>::Failure(MakeError(UiErrors::EventDispatchCapacityExceeded));
        const auto &owner = descriptor_.owner;
        const UiEventRoute route{owner.instance,    owner.canvas, owner.document,       owner.tree,
                                 owner.interaction, target,       descriptor_.modalRoot};
        const UiRoutedEvent event{UiEventKind::Gesture, nextSequence++, position, !gesture.accessible, gesture};
        const auto cancellationRevision = transientRevision_;
        const auto contact = std::ranges::find_if(contacts_, [&gesture](const Contact &entry) {
            return entry.capture.IsActive() && entry.capture.Request().pointer.Value() == gesture.pointer;
        });
        LiveGestureRoute handler{environment.handler, stopped_, transientRevision_,
                                 contact == contacts_.end() ? nullptr : &contact->capture};
        const auto dispatched = environment.dispatcher.Dispatch(environment.tree, route, event, handler);
        if (dispatched.HasError())
            return Result<bool>::Failure(dispatched.ErrorValue());
        if (stopped_)
            return Result<bool>::Failure(MakeError(UiErrors::EventDispatchLifecycleUnavailable));
        if (cancellationRevision != transientRevision_)
            return Result<bool>::Failure(MakeError(UiErrors::EventDispatchRouteInvalidated));
        if (environment.tree.State() != UiElementTreeState::Active || environment.tree.Revision() != owner.tree ||
            environment.presented.LastPresentedInteraction() != owner.interaction ||
            environment.captures.State() != UiPointerCaptureStoreState::Active)
            return Result<bool>::Failure(MakeError(UiErrors::EventDispatchRouteInvalidated));
        ++result.dispatched;
        result.handled |= dispatched.Value().handled;
        result.defaultPrevented |= dispatched.Value().defaultPrevented;
        return Result<bool>::Success(!dispatched.Value().defaultPrevented);
    }

    /** @copydoc UiPointerInteraction::Hover */
    Result<void> UiPointerInteraction::Hover(const UiPointerInteractionEnvironment &environment, const UiElementHandle target,
                                             const UiPointerSample &sample, const UiLogicalPoint position, std::uint64_t &nextSequence,
                                             UiPointerInteractionResult &result) {
        if (target != hovered_) {
            if (hovered_.IsValid()) {
                const auto previous = std::exchange(hovered_, {});
                if (const auto left = Emit(environment, previous, {UiGestureKind::HoverLeave, sample.pointer.Value(), previous}, position,
                                           nextSequence, result);
                    left.HasError())
                    return Result<void>::Failure(left.ErrorValue());
            }
            if (target.IsValid()) {
                const auto entered =
                    Emit(environment, target, {UiGestureKind::HoverEnter, sample.pointer.Value(), target}, position, nextSequence, result);
                if (entered.HasError())
                    return Result<void>::Failure(entered.ErrorValue());
                if (entered.Value())
                    hovered_ = target;
            }
        }
        if (hovered_.IsValid()) {
            const auto moved =
                Emit(environment, hovered_, {UiGestureKind::HoverMove, sample.pointer.Value(), hovered_}, position, nextSequence, result);
            if (moved.HasError())
                return Result<void>::Failure(moved.ErrorValue());
        }
        return Result<void>::Success();
    }

    /** @copydoc UiPointerInteraction::ProcessSample */
    Result<void> UiPointerInteraction::ProcessSample(const UiPointerInteractionEnvironment &environment, const UiScreenPointerQuery &query,
                                                     const UiPointerSample &sample, std::uint64_t &nextSequence,
                                                     UiPointerInteractionResult &result) {
        const auto hit = environment.hitTesting.HitTestScreen(query, environment.presented);
        if (hit.HasError())
            return Result<void>::Failure(hit.ErrorValue());
        const auto position = Position(query);
        if (position.HasError())
            return Result<void>::Failure(position.ErrorValue());
        const SampleContext context{environment, sample, hit.Value(), position.Value(), nextSequence, result};
        if (sample.edge == UiPointerEdge::Press)
            return PressContact(context);
        auto slots = std::span{contacts_}.first(descriptor_.pointerCapacity);
        auto found = std::ranges::find_if(slots, [&sample](const Contact &contact) {
            return contact.capture.State() != UiPointerCaptureState::Inactive && contact.capture.Request().pointer == sample.pointer;
        });
        if (found == slots.end()) {
            if (sample.modality == UiPointerModality::Mouse && (sample.edge == UiPointerEdge::Move || sample.edge == UiPointerEdge::Cancel))
                return Hover(environment, sample.edge == UiPointerEdge::Move && hit.Value() ? hit.Value()->element : UiElementHandle{},
                             sample, position.Value(), nextSequence, result);
            // Hoverless sources never manufacture a target before an admitted press.
            return Result<void>::Success();
        }
        const auto target = found->capture.Request().route.target;
        if (found->modality != sample.modality || found->capture.Request().button != sample.button)
            return Result<void>::Failure(MakeError(UiErrors::PointerCaptureInvalid));
        const auto previous = ObservePosition(*found, position.Value());
        UiGestureEvent gesture{UiGestureKind::Cancel, sample.pointer.Value(), target};
        if (sample.edge == UiPointerEdge::Cancel || !found->capture.IsActive()) {
            *found = {};
            const auto emitted = Emit(environment, target, gesture, position.Value(), nextSequence, result);
            return emitted.HasError() ? Result<void>::Failure(emitted.ErrorValue()) : Result<void>::Success();
        }
        if (sample.edge == UiPointerEdge::Move)
            return MoveContact(*found, previous, gesture, context);
        return ReleaseContact(*found, gesture, context);
    }

    /** @copydoc UiPointerInteraction::Tick */
    Result<void> UiPointerInteraction::Tick(const UiPointerInteractionEnvironment &environment, std::uint64_t &nextSequence,
                                            UiPointerInteractionResult &result) {
        for (auto &contact : std::span{contacts_}.first(descriptor_.pointerCapacity)) {
            if (contact.capture.State() == UiPointerCaptureState::Inactive)
                continue;
            const auto request = contact.capture.Request();
            if (!contact.capture.IsActive()) {
                const auto position = contact.position;
                contact = {};
                const auto emitted =
                    Emit(environment, request.route.target, {UiGestureKind::Cancel, request.pointer.Value(), request.route.target},
                         position, nextSequence, result);
                if (emitted.HasError())
                    return Result<void>::Failure(emitted.ErrorValue());
            } else if (!contact.panning && !contact.longPressed && !contact.multiTouch && !contact.exceededSlop &&
                       time_ - contact.pressedAt >= descriptor_.longPressMilliseconds) {
                const auto emitted =
                    Emit(environment, request.route.target, {UiGestureKind::LongPress, request.pointer.Value(), request.route.target},
                         contact.position, nextSequence, result);
                if (emitted.HasError())
                    return Result<void>::Failure(emitted.ErrorValue());
                if (!contact.capture.IsActive())
                    return Result<void>::Failure(MakeError(UiErrors::PointerCaptureSourceStale));
                contact.longPressed = emitted.Value();
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc UiPointerInteraction::Pump */
    Result<UiPointerInteractionResult> UiPointerInteraction::Pump(const UiPointerInteractionEnvironment &environment,
                                                                  const UiResolvedScreenCanvas &canvasSpace,
                                                                  const std::span<const UiPointerSample> samples,
                                                                  const std::uint64_t milliseconds, std::uint64_t &nextSequence) {
        if (const auto admitted = Validate(environment); admitted.HasError())
            return Result<UiPointerInteractionResult>::Failure(admitted.ErrorValue());
        if (!canvasSpace.IsValid() || milliseconds < time_ || samples.size() > MaximumUiInteractionSamples || nextSequence == 0 ||
            nextSequence > std::numeric_limits<std::uint64_t>::max() - 256)
            return Result<UiPointerInteractionResult>::Failure(MakeError(UiErrors::EventDispatchInvalid));
        // Validate the entire batch before any default runs; coordinates outside the viewport are legal during capture.
        for (const auto &sample : samples) {
            if (!sample.pointer.IsValid() || sample.edge >= UiPointerEdge::Count || sample.modality >= UiPointerModality::Count ||
                sample.button >= UiPointerButton::Count || !std::isfinite(sample.pixelX) || !std::isfinite(sample.pixelY))
                return Result<UiPointerInteractionResult>::Failure(MakeError(UiErrors::EventDispatchInvalid));
            if (const auto position =
                    Position({descriptor_.owner.view, descriptor_.owner.canvas, canvasSpace, sample.pixelX, sample.pixelY});
                position.HasError())
                return Result<UiPointerInteractionResult>::Failure(position.ErrorValue());
        }
        PumpGuard guard{pumping_};
        time_ = milliseconds;
        UiPointerInteractionResult result;
        for (const auto &sample : samples) {
            const auto processed =
                ProcessSample(environment, {descriptor_.owner.view, descriptor_.owner.canvas, canvasSpace, sample.pixelX, sample.pixelY},
                              sample, nextSequence, result);
            if (processed.HasError()) {
                for (auto &contact : contacts_)
                    contact = {};
                accessibleDrag_.reset();
                hovered_ = {};
                return Result<UiPointerInteractionResult>::Failure(processed.ErrorValue());
            }
        }
        if (const auto ticked = Tick(environment, nextSequence, result); ticked.HasError()) {
            for (auto &contact : contacts_)
                contact = {};
            hovered_ = {};
            return Result<UiPointerInteractionResult>::Failure(ticked.ErrorValue());
        }
        for (const auto &contact : contacts_)
            result.activePointers += contact.capture.IsActive() ? 1U : 0U;
        return Result<UiPointerInteractionResult>::Success(result);
    }

    /** @copydoc UiPointerInteraction::Accessible */
    Result<UiPointerInteractionResult> UiPointerInteraction::Accessible(const UiPointerInteractionEnvironment &environment,
                                                                        const UiElementHandle focused,
                                                                        const UiAccessibleGesture alternative,
                                                                        std::uint64_t &nextSequence) {
        if (const auto admitted = Validate(environment); admitted.HasError())
            return Result<UiPointerInteractionResult>::Failure(admitted.ErrorValue());
        if (alternative >= UiAccessibleGesture::Count || environment.tree.Get(focused).HasError())
            return Result<UiPointerInteractionResult>::Failure(MakeError(UiErrors::EventDispatchInvalid));
        PumpGuard guard{pumping_};
        const auto *policy = Policy(focused);
        UiGestureKind kind = UiGestureKind::Cancel;
        switch (alternative) {
            case UiAccessibleGesture::Activate:
                kind = UiGestureKind::Tap;
                break;
            case UiAccessibleGesture::ContextAction:
                kind = UiGestureKind::LongPress;
                break;
            case UiAccessibleGesture::PickUp:
                if (!policy || !policy->draggable || accessibleDrag_)
                    return Result<UiPointerInteractionResult>::Failure(MakeError(UiErrors::PointerCaptureBusy));
                kind = UiGestureKind::DragBegin;
                break;
            case UiAccessibleGesture::Drop:
                if (!accessibleDrag_ || !policy || !policy->dropTarget)
                    return Result<UiPointerInteractionResult>::Failure(MakeError(UiErrors::EventDispatchInvalid));
                kind = UiGestureKind::Drop;
                break;
            case UiAccessibleGesture::Cancel:
                break;
            case UiAccessibleGesture::Count:
                break;
        }
        const auto source = accessibleDrag_.value_or(focused);
        UiPointerInteractionResult result;
        const auto emitted = Emit(environment, focused, {kind, 0, source, {}, 1.0, 0.0, true}, {}, nextSequence, result);
        if (emitted.HasError()) {
            accessibleDrag_.reset();
            return Result<UiPointerInteractionResult>::Failure(emitted.ErrorValue());
        }
        if (alternative == UiAccessibleGesture::Cancel || (emitted.Value() && alternative == UiAccessibleGesture::Drop))
            accessibleDrag_.reset();
        else if (emitted.Value() && alternative == UiAccessibleGesture::PickUp)
            accessibleDrag_ = focused;
        return Result<UiPointerInteractionResult>::Success(result);
    }
}  // namespace Horo::Runtime::Ui
