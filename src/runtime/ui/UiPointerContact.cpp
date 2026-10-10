#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiPointerInteraction.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Measures finite logical displacement without signed integer overflow. */
        double Distance(const UiLogicalPoint from, const UiLogicalPoint to) noexcept {
            return std::hypot(static_cast<double>(to.x) - from.x, static_cast<double>(to.y) - from.y);
        }

        /** @brief Saturates derived deltas rather than wrapping a checked endpoint. */
        UiLogicalPoint Delta(const UiLogicalPoint from, const UiLogicalPoint to) noexcept {
            const auto difference = [](const std::int32_t left, const std::int32_t right) {
                return static_cast<std::int32_t>(std::clamp(static_cast<std::int64_t>(right) - left,
                                                            static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()),
                                                            static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())));
            };
            return {difference(from.x, to.x), difference(from.y, to.y)};
        }

    }  // namespace

    /** @copydoc UiPointerInteraction::ObservePosition */
    UiLogicalPoint UiPointerInteraction::ObservePosition(Contact &contact, const UiLogicalPoint position) noexcept {
        const auto previous = contact.position;
        contact.position = position;
        contact.exceededSlop |= Distance(contact.origin, position) >= descriptor_.dragThreshold;
        return previous;
    }

    /** @copydoc UiPointerInteraction::LinkTouch */
    Result<void> UiPointerInteraction::LinkTouch(Contact &contact, const SampleContext &context) {
        const auto &environment = context.environment;
        auto &nextSequence = context.nextSequence;
        auto &result = context.result;
        const auto target = contact.capture.Request().route.target;
        auto slots = std::span{contacts_}.first(descriptor_.pointerCapacity);
        for (auto &partner : slots) {
            if (&partner != &contact && partner.capture.IsActive() && partner.modality == UiPointerModality::Touch &&
                partner.capture.Request().route.target == target) {
                contact.multiTouch = true;
                partner.multiTouch = true;
                if (partner.dragging) {
                    if (const auto ended =
                            Emit(environment, target, {UiGestureKind::Cancel, partner.capture.Request().pointer.Value(), target},
                                 partner.position, nextSequence, result);
                        ended.HasError())
                        return Result<void>::Failure(ended.ErrorValue());
                    if (!partner.capture.IsActive() || !contact.capture.IsActive())
                        return Result<void>::Failure(MakeError(UiErrors::PointerCaptureSourceStale));
                    partner.dragging = false;
                }
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc UiPointerInteraction::PressContact */
    Result<void> UiPointerInteraction::PressContact(const SampleContext &context) {
        const auto &environment = context.environment;
        const auto &sample = context.sample;
        const auto &hit = context.hit;
        auto &nextSequence = context.nextSequence;
        auto &result = context.result;
        const auto position = context.position;
        auto slots = std::span{contacts_}.first(descriptor_.pointerCapacity);
        auto found = std::ranges::find_if(slots, [&sample](const Contact &contact) {
            return contact.capture.State() != UiPointerCaptureState::Inactive && contact.capture.Request().pointer == sample.pointer;
        });
        if (found != slots.end())
            return Result<void>::Failure(MakeError(UiErrors::PointerCaptureBusy));
        if (!hit)
            return Result<void>::Success();
        found = std::ranges::find_if(slots, [](const Contact &contact) {
            return contact.capture.State() == UiPointerCaptureState::Inactive;
        });
        if (found == slots.end())
            return Result<void>::Failure(MakeError(UiErrors::PointerCaptureCapacityExceeded));
        const auto &owner = descriptor_.owner;
        const auto target = hit->element;
        const UiEventRoute route{owner.instance,    owner.canvas, owner.document,       owner.tree,
                                 owner.interaction, target,       descriptor_.modalRoot};
        auto captured = environment.captures.Capture({owner.context, sample.pointer, sample.button, owner.view, route}, environment.tree,
                                                     environment.presented);
        if (captured.HasError())
            return Result<void>::Failure(captured.ErrorValue());
        found->capture = std::move(captured).Value();
        found->modality = sample.modality;
        found->origin = position;
        found->position = position;
        found->pressedAt = time_;
        if (sample.modality == UiPointerModality::Touch)
            if (const auto linked = LinkTouch(*found, context); linked.HasError())
                return linked;
        const auto emitted =
            Emit(environment, target, {UiGestureKind::Press, sample.pointer.Value(), target}, position, nextSequence, result);
        if (emitted.HasError())
            return Result<void>::Failure(emitted.ErrorValue());
        if (!found->capture.IsActive())
            return Result<void>::Failure(MakeError(UiErrors::PointerCaptureSourceStale));
        if (!emitted.Value())
            *found = {};
        return Result<void>::Success();
    }

    /** @copydoc UiPointerInteraction::MoveTouch */
    Result<bool> UiPointerInteraction::MoveTouch(Contact &contact, const UiLogicalPoint previous, UiGestureEvent &gesture,
                                                 const SampleContext &context) {
        const auto &environment = context.environment;
        const auto position = context.position;
        const auto target = contact.capture.Request().route.target;
        auto slots = std::span{contacts_}.first(descriptor_.pointerCapacity);
        const auto partner = std::ranges::find_if(slots, [&contact, target](const Contact &entry) {
            return &entry != &contact && entry.capture.IsActive() && entry.modality == UiPointerModality::Touch &&
                   entry.capture.Request().route.target == target;
        });
        if (partner != slots.end()) {
            contact.multiTouch = true;
            partner->multiTouch = true;
            const double oldX = static_cast<double>(previous.x) - partner->position.x;
            const double oldY = static_cast<double>(previous.y) - partner->position.y;
            const double newX = static_cast<double>(position.x) - partner->position.x;
            const double newY = static_cast<double>(position.y) - partner->position.y;
            const double oldLength = std::hypot(oldX, oldY);
            const double newLength = std::hypot(newX, newY);
            // Coincident contacts are valid but cannot establish a scale or angle baseline.
            if (oldLength >= 1.0 && newLength >= 1.0) {
                auto &nextSequence = context.nextSequence;
                auto &result = context.result;
                gesture.kind = UiGestureKind::PinchRotate;
                gesture.scale = newLength / oldLength;
                gesture.rotation = std::atan2(oldX * newY - oldY * newX, oldX * newX + oldY * newY);
                if (const auto emitted = Emit(environment, target, gesture, position, nextSequence, result); emitted.HasError())
                    return Result<bool>::Failure(emitted.ErrorValue());
                if (!contact.capture.IsActive() || !partner->capture.IsActive())
                    return Result<bool>::Failure(MakeError(UiErrors::PointerCaptureSourceStale));
            }
            return Result<bool>::Success(true);
        }
        return Result<bool>::Success(false);
    }

    /** @copydoc UiPointerInteraction::MoveContact */
    Result<void> UiPointerInteraction::MoveContact(Contact &contact, const UiLogicalPoint previous, UiGestureEvent &gesture,
                                                   const SampleContext &context) {
        const auto &environment = context.environment;
        const auto &sample = context.sample;
        auto &nextSequence = context.nextSequence;
        auto &result = context.result;
        const auto position = context.position;
        const auto target = contact.capture.Request().route.target;
        gesture.delta = Delta(previous, position);
        if (sample.modality == UiPointerModality::Touch) {
            const auto pinched = MoveTouch(contact, previous, gesture, context);
            if (pinched.HasError())
                return Result<void>::Failure(pinched.ErrorValue());
            if (pinched.Value())
                return Result<void>::Success();
        }
        if (!contact.panning && Distance(contact.origin, position) >= descriptor_.dragThreshold) {
            const auto *policy = Policy(target);
            gesture.kind = policy && policy->draggable ? UiGestureKind::DragBegin : UiGestureKind::PanBegin;
            const auto emitted = Emit(environment, target, gesture, position, nextSequence, result);
            if (emitted.HasError())
                return Result<void>::Failure(emitted.ErrorValue());
            if (!contact.capture.IsActive())
                return Result<void>::Failure(MakeError(UiErrors::PointerCaptureSourceStale));
            contact.panning = emitted.Value();
            contact.dragging = gesture.kind == UiGestureKind::DragBegin && emitted.Value();
        } else if (contact.panning) {
            gesture.kind = contact.dragging ? UiGestureKind::DragUpdate : UiGestureKind::PanUpdate;
            if (const auto emitted = Emit(environment, target, gesture, position, nextSequence, result); emitted.HasError())
                return Result<void>::Failure(emitted.ErrorValue());
            if (!contact.capture.IsActive())
                return Result<void>::Failure(MakeError(UiErrors::PointerCaptureSourceStale));
        }
        return Result<void>::Success();
    }

    /** @copydoc UiPointerInteraction::TapContact */
    Result<void> UiPointerInteraction::TapContact(Contact &contact, UiGestureEvent &gesture, const SampleContext &context) {
        const auto &environment = context.environment;
        auto &nextSequence = context.nextSequence;
        auto &result = context.result;
        const auto position = context.position;
        const auto target = contact.capture.Request().route.target;
        const bool doubleTap = lastTap_ == target && time_ - lastTapTime_ <= descriptor_.doubleTapMilliseconds &&
                               Distance(lastTapPosition_, position) < descriptor_.dragThreshold;
        gesture.kind = doubleTap ? UiGestureKind::DoubleTap : UiGestureKind::Tap;
        const auto emitted = Emit(environment, target, gesture, position, nextSequence, result);
        if (emitted.HasError())
            return Result<void>::Failure(emitted.ErrorValue());
        if (!contact.capture.IsActive())
            return Result<void>::Failure(MakeError(UiErrors::PointerCaptureSourceStale));
        if (emitted.Value()) {
            lastTap_ = doubleTap ? UiElementHandle{} : target;
            lastTapPosition_ = position;
            lastTapTime_ = time_;
        }
        return Result<void>::Success();
    }

    /** @copydoc UiPointerInteraction::ReleaseContact */
    Result<void> UiPointerInteraction::ReleaseContact(Contact &contact, UiGestureEvent &gesture, const SampleContext &context) {
        const auto &environment = context.environment;
        const auto &hit = context.hit;
        auto &nextSequence = context.nextSequence;
        auto &result = context.result;
        const auto position = context.position;
        const auto target = contact.capture.Request().route.target;
        if (contact.dragging && hit) {
            const auto *policy = Policy(hit->element);
            if (policy && policy->dropTarget) {
                gesture.kind = UiGestureKind::Drop;
                if (const auto emitted = Emit(environment, hit->element, gesture, position, nextSequence, result); emitted.HasError())
                    return Result<void>::Failure(emitted.ErrorValue());
            }
        } else if (!contact.panning && !contact.longPressed && !contact.multiTouch && !contact.exceededSlop && hit &&
                   hit->element == target) {
            if (const auto tapped = TapContact(contact, gesture, context); tapped.HasError())
                return tapped;
        }
        if (!contact.capture.IsActive())
            return Result<void>::Failure(MakeError(UiErrors::PointerCaptureSourceStale));
        gesture.kind = contact.panning ? UiGestureKind::PanEnd : UiGestureKind::Release;
        contact = {};
        const auto emitted = Emit(environment, target, gesture, position, nextSequence, result);
        return emitted.HasError() ? Result<void>::Failure(emitted.ErrorValue()) : Result<void>::Success();
    }

}  // namespace Horo::Runtime::Ui
