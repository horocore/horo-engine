#include "Horo/Runtime/Ui/UiErrors.h"
#include "UiControlsInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] Result<void> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<void>::Failure(MakeError(descriptor));
        }

        template <typename Enum> [[nodiscard]] bool IsKnown(const Enum value, const Enum count) noexcept {
            using Underlying = std::underlying_type_t<Enum>;
            return static_cast<Underlying>(value) < static_cast<Underlying>(count);
        }
    }  // namespace

    namespace UiControlDetail {
        [[nodiscard]] bool IsPressSource(const UiControlInputKind kind, const UiControlActivationSource source) noexcept {
            switch (kind) {
                case UiControlInputKind::PointerPress:
                case UiControlInputKind::PointerRelease:
                    return source == UiControlActivationSource::Pointer;
                case UiControlInputKind::SubmitPress:
                case UiControlInputKind::SubmitRelease:
                    return source != UiControlActivationSource::Pointer && IsKnown(source, UiControlActivationSource::Count);
                case UiControlInputKind::AdjustPress:
                case UiControlInputKind::AdjustRelease:
                    return IsKnown(source, UiControlActivationSource::Count);
                case UiControlInputKind::Cancel:
                case UiControlInputKind::FocusGained:
                case UiControlInputKind::FocusLost:
                case UiControlInputKind::TextInput:
                case UiControlInputKind::RepeatTick:
                case UiControlInputKind::Count:
                    return IsKnown(source, UiControlActivationSource::Count);
            }
            return false;
        }

        [[nodiscard]] std::uint64_t EventTick(const UiControlInput &input) noexcept {
            return input.tick != 0 ? input.tick : input.sequence;
        }

        [[nodiscard]] double AdjustedSliderValue(const UiSliderControlDescriptor &descriptor, const double current,
                                                 const UiControlAdjustment adjustment) noexcept {
            const double delta = adjustment == UiControlAdjustment::Increase ? descriptor.step : -descriptor.step;
            double next = current + delta;
            if (!std::isfinite(next))
                next = adjustment == UiControlAdjustment::Increase ? descriptor.maximum : descriptor.minimum;
            return std::clamp(next, descriptor.minimum, descriptor.maximum);
        }
    }  // namespace UiControlDetail

    /** @copydoc UiControlStateMachine::Storage::Storage */
    UiControlStateMachine::Storage::Storage(UiControlDescriptor source)
        : descriptor(std::move(source)), state(UiControlDetail::InitialState(descriptor)) {
        if (const auto *text = std::get_if<UiTextInputControlDescriptor>(&descriptor); text != nullptr)
            editStartText = text->initialText;
    }

    /** @brief Clears press, repeat, editing, pending and optionally focus state. */
    void UiControlStateMachine::Storage::ClearTransient(const bool clearFocus) noexcept {
        UiControlDetail::SetPressed(state, false);
        UiControlDetail::SetRepeating(state, false);
        UiControlDetail::SetEditing(state, false);
        if (clearFocus)
            UiControlDetail::SetFocused(state, false);
        pending = false;
        repeatArmed = false;
        repeatNextTick = 0;
        adjustment = UiControlAdjustment::Count;
    }

    /** @brief Projects an availability transition into the typed state variant. */
    void UiControlStateMachine::Storage::SetAvailabilityProjection(const UiControlAvailability availability) noexcept {
        std::visit([availability](auto &typed) noexcept {
            typed.availability = availability;
        }, state);
    }

    /** @brief Arms bounded repeat without mutating state when tick arithmetic would overflow. */
    Result<void> UiControlStateMachine::Storage::ArmRepeat(const UiControlRepeatPolicy &policy, const std::uint64_t tick) {
        if (!policy.enabled)
            return Result<void>::Success();
        if (tick > std::numeric_limits<std::uint64_t>::max() - policy.initialDelayTicks)
            return Failure(UiErrors::ControlSequenceInvalid);
        repeatNextTick = tick + policy.initialDelayTicks;
        repeatArmed = true;
        UiControlDetail::SetRepeating(state, false);
        return Result<void>::Success();
    }

    /** @brief Stages one default action while preserving the one-pending-action invariant. */
    Result<void> UiControlStateMachine::Storage::Queue(const UiControlDetail::PendingDefault &queued) {
        if (pending)
            return Failure(UiErrors::ControlDefaultPending);
        pendingDefault = queued;
        pending = true;
        return Result<void>::Success();
    }

    /** @brief Handles focus admission for an enabled focusable control. */
    Result<UiControlTransitionKind> UiControlStateMachine::Storage::HandleFocusGained() {
        const UiControlDescriptorBase &base = UiControlDetail::BaseOf(descriptor);
        if (!UiControlDetail::IsEnabled(state) || !base.focusable)
            return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::IgnoredDisabled);
        if (UiControlDetail::IsFocused(state))
            return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::NoOp);
        UiControlDetail::SetFocused(state, true);
        if (UiControlDetail::KindOf(descriptor) == UiControlKind::TextInput) {
            editStartText = std::get<UiTextInputControlState>(state).text;
            UiControlDetail::SetEditing(state, true);
        }
        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::Focused);
    }

    /** @brief Cancels transient state and focus while preserving the last edit value. */
    Result<UiControlTransitionKind> UiControlStateMachine::Storage::HandleFocusLost() {
        const bool changed = UiControlDetail::IsFocused(state) || UiControlDetail::IsPressed(state) || pending;
        ClearTransient(true);
        return Result<UiControlTransitionKind>::Success(changed ? UiControlTransitionKind::Unfocused : UiControlTransitionKind::NoOp);
    }

    /** @brief Cancels transient state and restores an active text edit session. */
    Result<UiControlTransitionKind> UiControlStateMachine::Storage::HandleCancel() {
        const UiControlKind kind = UiControlDetail::KindOf(descriptor);
        const bool editing = kind == UiControlKind::TextInput && std::get<UiTextInputControlState>(state).editing;
        const bool changed = UiControlDetail::IsFocused(state) || UiControlDetail::IsPressed(state) || pending || editing;
        if (editing)
            std::get<UiTextInputControlState>(state).text = editStartText;
        ClearTransient(false);
        return Result<UiControlTransitionKind>::Success(changed ? UiControlTransitionKind::Cancelled : UiControlTransitionKind::NoOp);
    }

    /** @brief Handles pointer activation for buttons, toggles and text fields. */
    Result<UiControlTransitionKind> UiControlStateMachine::Storage::HandlePointerPress(const UiControlInput &input) {
        if (!UiControlDetail::IsEnabled(state))
            return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::IgnoredDisabled);
        const UiControlKind kind = UiControlDetail::KindOf(descriptor);
        const UiControlDescriptorBase &base = UiControlDetail::BaseOf(descriptor);
        if (kind == UiControlKind::TextInput) {
            if (base.focusable)
                UiControlDetail::SetFocused(state, true);
            if (!std::get<UiTextInputControlState>(state).editing) {
                editStartText = std::get<UiTextInputControlState>(state).text;
                UiControlDetail::SetEditing(state, true);
            }
            return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::Focused);
        }
        if (kind == UiControlKind::Slider)
            return Failure<UiControlTransitionKind>(UiErrors::ControlInputInvalid);
        if (const auto armed = ArmRepeat(base.repeat, UiControlDetail::EventTick(input)); armed.HasError())
            return Result<UiControlTransitionKind>::Failure(armed.ErrorValue());
        if (base.focusable)
            UiControlDetail::SetFocused(state, true);
        UiControlDetail::SetPressed(state, true);
        pressSource = input.activationSource;
        adjustment = UiControlAdjustment::Count;
        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::Pressed);
    }

    /** @brief Handles normalized submit activation for focused controls. */
    Result<UiControlTransitionKind> UiControlStateMachine::Storage::HandleSubmitPress(const UiControlInput &input) {
        if (!UiControlDetail::IsEnabled(state))
            return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::IgnoredDisabled);
        if (!UiControlDetail::IsFocused(state))
            return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::IgnoredUnfocused);
        const UiControlKind kind = UiControlDetail::KindOf(descriptor);
        if (kind == UiControlKind::Slider)
            return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::NoOp);
        const UiControlDescriptorBase &base = UiControlDetail::BaseOf(descriptor);
        if (kind == UiControlKind::TextInput && !std::get<UiTextInputControlState>(state).editing) {
            editStartText = std::get<UiTextInputControlState>(state).text;
            UiControlDetail::SetEditing(state, true);
        }
        if (const auto armed = ArmRepeat(base.repeat, UiControlDetail::EventTick(input)); armed.HasError())
            return Result<UiControlTransitionKind>::Failure(armed.ErrorValue());
        UiControlDetail::SetPressed(state, true);
        pressSource = input.activationSource;
        adjustment = UiControlAdjustment::Count;
        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::Pressed);
    }

    /** @brief Handles one typed slider adjustment and stages its bounded value action. */
    Result<UiControlTransitionKind> UiControlStateMachine::Storage::HandleAdjustPress(const UiControlInput &input) {
        if (UiControlDetail::KindOf(descriptor) != UiControlKind::Slider)
            return Failure<UiControlTransitionKind>(UiErrors::ControlInputInvalid);
        if (!UiControlDetail::IsEnabled(state))
            return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::IgnoredDisabled);
        if (!UiControlDetail::IsFocused(state))
            return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::IgnoredUnfocused);
        const UiControlDescriptorBase &base = UiControlDetail::BaseOf(descriptor);
        if (const auto armed = ArmRepeat(base.repeat, UiControlDetail::EventTick(input)); armed.HasError())
            return Result<UiControlTransitionKind>::Failure(armed.ErrorValue());
        UiControlDetail::SetPressed(state, true);
        UiControlDetail::SetEditing(state, true);
        pressSource = input.activationSource;
        adjustment = input.adjustment;
        const auto &slider = std::get<UiSliderControlDescriptor>(descriptor);
        const double next = UiControlDetail::AdjustedSliderValue(slider, UiControlDetail::SliderValue(state), input.adjustment);
        if (next == UiControlDetail::SliderValue(state))
            return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::Pressed);
        if (const auto queued = Queue({UiControlDetail::PendingKind::ValueChanged, input.activationSource, input.sequence, false, next});
            queued.HasError())
            return Result<UiControlTransitionKind>::Failure(queued.ErrorValue());
        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::DefaultPending);
    }

    /** @brief Handles release edges and stages button, toggle, or text submit defaults. */
    Result<UiControlTransitionKind> UiControlStateMachine::Storage::HandleRelease(const UiControlInput &input) {
        const bool matchingPress = UiControlDetail::IsPressed(state) && pressSource == input.activationSource;
        if (!matchingPress) {
            if (UiControlDetail::IsPressed(state))
                ClearTransient(false);
            return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::Cancelled);
        }
        UiControlDetail::SetPressed(state, false);
        UiControlDetail::SetRepeating(state, false);
        repeatArmed = false;
        repeatNextTick = 0;
        const UiControlKind kind = UiControlDetail::KindOf(descriptor);
        if (input.kind == UiControlInputKind::AdjustRelease || kind == UiControlKind::Slider) {
            UiControlDetail::SetEditing(state, false);
            adjustment = UiControlAdjustment::Count;
            return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::Released);
        }
        const auto pendingKind = kind == UiControlKind::Button   ? UiControlDetail::PendingKind::Activate
                                 : kind == UiControlKind::Toggle ? UiControlDetail::PendingKind::Toggle
                                                                 : UiControlDetail::PendingKind::Submit;
        if (const auto queued = Queue({pendingKind, input.activationSource, input.sequence, false, 0.0}); queued.HasError())
            return Result<UiControlTransitionKind>::Failure(queued.ErrorValue());
        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::DefaultPending);
    }

    /** @brief Appends bounded UTF-8 text to a focused editing control without allocation. */
    Result<UiControlTransitionKind> UiControlStateMachine::Storage::HandleTextInput(const UiControlInput &input) {
        if (UiControlDetail::KindOf(descriptor) != UiControlKind::TextInput)
            return Failure<UiControlTransitionKind>(UiErrors::ControlInputInvalid);
        if (!UiControlDetail::IsEnabled(state))
            return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::IgnoredDisabled);
        if (!UiControlDetail::IsFocused(state) || !std::get<UiTextInputControlState>(state).editing)
            return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::IgnoredUnfocused);
        const auto &text = std::get<UiTextInputControlState>(state).text;
        const auto &descriptor = std::get<UiTextInputControlDescriptor>(this->descriptor);
        if (input.text.size > descriptor.maximumTextBytes - text.size)
            return Failure<UiControlTransitionKind>(UiErrors::ControlCapacityExceeded);
        auto &mutableText = std::get<UiTextInputControlState>(state).text;
        std::copy(input.text.View().begin(), input.text.View().end(), mutableText.bytes.begin() + mutableText.size);
        mutableText.size = static_cast<std::uint16_t>(mutableText.size + input.text.size);
        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::TextEdited);
    }

    /** @brief Emits at most one bounded repeat default for an admitted owner tick. */
    Result<UiControlTransitionKind> UiControlStateMachine::Storage::HandleRepeatTick(const UiControlInput &input) {
        if (!UiControlDetail::IsEnabled(state) || !UiControlDetail::IsPressed(state) || !repeatArmed ||
            !UiControlDetail::BaseOf(descriptor).repeat.enabled)
            return Result<UiControlTransitionKind>::Success(UiControlDetail::IsEnabled(state) ? UiControlTransitionKind::NoOp
                                                                                              : UiControlTransitionKind::IgnoredDisabled);
        if (input.activationSource != pressSource)
            return Failure<UiControlTransitionKind>(UiErrors::ControlInputInvalid);
        if (input.tick < repeatNextTick)
            return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::NoOp);
        const auto &base = UiControlDetail::BaseOf(descriptor);
        if (input.tick > std::numeric_limits<std::uint64_t>::max() - base.repeat.intervalTicks)
            return Failure<UiControlTransitionKind>(UiErrors::ControlSequenceInvalid);
        repeatNextTick = input.tick + base.repeat.intervalTicks;
        UiControlDetail::SetRepeating(state, true);
        const UiControlKind kind = UiControlDetail::KindOf(descriptor);
        if (kind == UiControlKind::Slider) {
            const auto &slider = std::get<UiSliderControlDescriptor>(descriptor);
            const double next = UiControlDetail::AdjustedSliderValue(slider, UiControlDetail::SliderValue(state), adjustment);
            if (next == UiControlDetail::SliderValue(state))
                return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::NoOp);
            if (const auto queued = Queue({UiControlDetail::PendingKind::ValueChanged, input.activationSource, input.sequence, true, next});
                queued.HasError())
                return Result<UiControlTransitionKind>::Failure(queued.ErrorValue());
            return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::DefaultPending);
        }
        if (kind == UiControlKind::Button || kind == UiControlKind::Toggle) {
            const auto pendingKind =
                kind == UiControlKind::Button ? UiControlDetail::PendingKind::Activate : UiControlDetail::PendingKind::Toggle;
            if (const auto queued = Queue({pendingKind, input.activationSource, input.sequence, true, 0.0}); queued.HasError())
                return Result<UiControlTransitionKind>::Failure(queued.ErrorValue());
            return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::DefaultPending);
        }
        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::NoOp);
    }

    /** @brief Dispatches one normalized input to a focused transition helper. */
    Result<UiControlTransitionKind> UiControlStateMachine::Storage::ApplyInput(const UiControlInput &input) {
        switch (input.kind) {
            case UiControlInputKind::FocusGained:
                return HandleFocusGained();
            case UiControlInputKind::FocusLost:
                return HandleFocusLost();
            case UiControlInputKind::Cancel:
                return HandleCancel();
            case UiControlInputKind::PointerPress:
                return HandlePointerPress(input);
            case UiControlInputKind::SubmitPress:
                return HandleSubmitPress(input);
            case UiControlInputKind::AdjustPress:
                return HandleAdjustPress(input);
            case UiControlInputKind::PointerRelease:
            case UiControlInputKind::SubmitRelease:
            case UiControlInputKind::AdjustRelease:
                return HandleRelease(input);
            case UiControlInputKind::TextInput:
                return HandleTextInput(input);
            case UiControlInputKind::RepeatTick:
                return HandleRepeatTick(input);
            case UiControlInputKind::Count:
                break;
        }
        return Failure<UiControlTransitionKind>(UiErrors::ControlInputInvalid);
    }
}  // namespace Horo::Runtime::Ui
