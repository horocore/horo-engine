#include "Horo/Runtime/Ui/UiControls.h"

#include "Horo/Foundation/Utf8.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
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

        [[nodiscard]] bool IsValidControlText(const UiActionText &text) noexcept {
            return text.IsValid() && IsValidUtf8ScalarSequence(text.View());
        }

        [[nodiscard]] bool IsValidControlValue(const UiActionValue &value) noexcept {
            return std::visit([]<typename Typed>(const Typed &typed) noexcept {
                using Value = std::decay_t<Typed>;
                if constexpr (std::is_same_v<Value, double>)
                    return std::isfinite(typed);
                else if constexpr (std::is_same_v<Value, UiActionText>)
                    return IsValidControlText(typed);
                else if constexpr (std::is_same_v<Value, UiActionId> || std::is_same_v<Value, UiDocumentId> ||
                                   std::is_same_v<Value, UiCanvasId> || std::is_same_v<Value, UiElementId>)
                    return typed.IsValid();
                else
                    return true;
            }, value);
        }

        [[nodiscard]] bool IsValidControlPayload(const UiActionPayload &payload) noexcept {
            if (payload.Size() > MaximumUiActionArguments)
                return false;
            for (const UiActionValue &value : payload.Values())
                if (!IsValidControlValue(value))
                    return false;
            return true;
        }

        [[nodiscard]] const UiControlDescriptorBase &BaseOf(const UiControlDescriptor &descriptor) noexcept {
            return std::visit([](const auto &typed) -> const UiControlDescriptorBase & {
                return typed.base;
            }, descriptor);
        }

        [[nodiscard]] UiControlKind KindOf(const UiControlDescriptor &descriptor) noexcept {
            return std::visit([]<typename Typed>(const Typed &) noexcept {
                if constexpr (std::is_same_v<Typed, UiButtonControlDescriptor>)
                    return UiControlKind::Button;
                else if constexpr (std::is_same_v<Typed, UiToggleControlDescriptor>)
                    return UiControlKind::Toggle;
                else if constexpr (std::is_same_v<Typed, UiSliderControlDescriptor>)
                    return UiControlKind::Slider;
                else
                    return UiControlKind::TextInput;
            }, descriptor);
        }

        [[nodiscard]] UiControlAvailability InitialAvailability(const UiControlDescriptorBase &base) noexcept {
            return base.initiallyEnabled ? UiControlAvailability::Enabled : UiControlAvailability::Disabled;
        }

        [[nodiscard]] UiControlState InitialState(const UiControlDescriptor &descriptor) {
            return std::visit([](const auto &typed) -> UiControlState {
                using Typed = std::decay_t<decltype(typed)>;
                const UiControlAvailability availability = InitialAvailability(typed.base);
                if constexpr (std::is_same_v<Typed, UiButtonControlDescriptor>)
                    return UiButtonControlState{availability, false, false, false};
                else if constexpr (std::is_same_v<Typed, UiToggleControlDescriptor>)
                    return UiToggleControlState{availability, false, false, typed.initiallyChecked, false};
                else if constexpr (std::is_same_v<Typed, UiSliderControlDescriptor>)
                    return UiSliderControlState{availability, false, false, false, false, typed.initialValue};
                else
                    return UiTextInputControlState{availability, false, false, false, typed.initialText};
            }, descriptor);
        }

        [[nodiscard]] bool IsEnabled(const UiControlState &state) noexcept {
            return std::visit([](const auto &typed) noexcept {
                return typed.availability == UiControlAvailability::Enabled;
            }, state);
        }

        [[nodiscard]] bool IsFocused(const UiControlState &state) noexcept {
            return std::visit([](const auto &typed) noexcept {
                return typed.focused;
            }, state);
        }

        [[nodiscard]] bool IsPressed(const UiControlState &state) noexcept {
            return std::visit([](const auto &typed) noexcept {
                return typed.pressed;
            }, state);
        }

        void SetFocused(UiControlState &state, const bool focused) noexcept {
            std::visit([focused](auto &typed) noexcept {
                typed.focused = focused;
            }, state);
        }

        void SetPressed(UiControlState &state, const bool pressed) noexcept {
            std::visit([pressed](auto &typed) noexcept {
                typed.pressed = pressed;
            }, state);
        }

        void SetRepeating(UiControlState &state, const bool repeating) noexcept {
            std::visit([repeating](auto &typed) noexcept {
                using Typed = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<Typed, UiButtonControlState> || std::is_same_v<Typed, UiToggleControlState> ||
                              std::is_same_v<Typed, UiSliderControlState>)
                    typed.repeating = repeating;
            }, state);
        }

        void SetEditing(UiControlState &state, const bool editing) noexcept {
            std::visit([editing](auto &typed) noexcept {
                using Typed = std::decay_t<decltype(typed)>;
                if constexpr (std::is_same_v<Typed, UiSliderControlState> || std::is_same_v<Typed, UiTextInputControlState>)
                    typed.editing = editing;
            }, state);
        }

        [[nodiscard]] double SliderValue(const UiControlState &state) noexcept {
            return std::get<UiSliderControlState>(state).value;
        }

        [[nodiscard]] UiControlKind KindOf(const UiControlState &state) noexcept {
            return std::visit([]<typename Typed>(const Typed &) noexcept {
                if constexpr (std::is_same_v<Typed, UiButtonControlState>)
                    return UiControlKind::Button;
                else if constexpr (std::is_same_v<Typed, UiToggleControlState>)
                    return UiControlKind::Toggle;
                else if constexpr (std::is_same_v<Typed, UiSliderControlState>)
                    return UiControlKind::Slider;
                else
                    return UiControlKind::TextInput;
            }, state);
        }

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

        enum class PendingKind : std::uint8_t {
            Activate,
            Toggle,
            ValueChanged,
            Submit,
        };

        struct PendingDefault final {
            PendingKind kind{PendingKind::Activate};
            UiControlActivationSource source{UiControlActivationSource::Programmatic};
            std::uint64_t sequence{};
            bool repeated{};
            double value{};
        };

        [[nodiscard]] double AdjustedSliderValue(const UiSliderControlDescriptor &descriptor, const double current,
                                                 const UiControlAdjustment adjustment) noexcept {
            const double delta = adjustment == UiControlAdjustment::Increase ? descriptor.step : -descriptor.step;
            double next = current + delta;
            if (!std::isfinite(next))
                next = adjustment == UiControlAdjustment::Increase ? descriptor.maximum : descriptor.minimum;
            return std::clamp(next, descriptor.minimum, descriptor.maximum);
        }

        [[nodiscard]] const UiActionOwnerContext &InvalidOwner() noexcept {
            static const UiActionOwnerContext invalid{};
            return invalid;
        }

        [[nodiscard]] UiElementHandle InvalidElement() noexcept {
            return {};
        }
    }  // namespace

    /** @copydoc UiControlRepeatPolicy::IsValid */
    bool UiControlRepeatPolicy::IsValid() const noexcept {
        if (!enabled)
            return initialDelayTicks == 0 && intervalTicks == 0;
        return initialDelayTicks > 0 && initialDelayTicks <= MaximumUiControlRepeatTicks && intervalTicks > 0 &&
               intervalTicks <= MaximumUiControlRepeatTicks;
    }

    /** @copydoc UiControlDescriptorBase::IsValid */
    bool UiControlDescriptorBase::IsValid() const noexcept {
        return UiActionSource{owner, element}.IsValid() && action.IsValid() && IsValidControlPayload(payload) && repeat.IsValid();
    }

    /** @copydoc UiButtonControlDescriptor::IsValid */
    bool UiButtonControlDescriptor::IsValid() const noexcept {
        return base.IsValid();
    }

    /** @copydoc UiToggleControlDescriptor::IsValid */
    bool UiToggleControlDescriptor::IsValid() const noexcept {
        return base.IsValid() && base.payload.Size() < MaximumUiActionArguments;
    }

    /** @copydoc UiSliderControlDescriptor::IsValid */
    bool UiSliderControlDescriptor::IsValid() const noexcept {
        return base.IsValid() && base.payload.Size() < MaximumUiActionArguments && std::isfinite(minimum) && std::isfinite(maximum) &&
               std::isfinite(step) && std::isfinite(initialValue) && minimum <= maximum && step > 0.0 && initialValue >= minimum &&
               initialValue <= maximum;
    }

    /** @copydoc UiTextInputControlDescriptor::IsValid */
    bool UiTextInputControlDescriptor::IsValid() const noexcept {
        return base.IsValid() && base.payload.Size() < MaximumUiActionArguments && !base.repeat.enabled && maximumTextBytes > 0 &&
               maximumTextBytes <= MaximumUiActionTextBytes && IsValidControlText(initialText) && initialText.size <= maximumTextBytes;
    }

    /** @copydoc ValidateUiControlDescriptor */
    Result<void> ValidateUiControlDescriptor(const UiControlDescriptor &descriptor) {
        const bool valid = std::visit([](const auto &typed) {
            return typed.IsValid();
        }, descriptor);
        return valid ? Result<void>::Success() : Failure(UiErrors::ControlDescriptorInvalid);
    }

    /** @copydoc UiControlKindOf */
    UiControlKind UiControlKindOf(const UiControlDescriptor &descriptor) noexcept {
        return KindOf(descriptor);
    }

    /** @copydoc UiControlKindOf */
    UiControlKind UiControlKindOf(const UiControlState &state) noexcept {
        return KindOf(state);
    }

    /** @copydoc UiControlInput::IsValid */
    bool UiControlInput::IsValid() const noexcept {
        return source.IsValid() && IsKnown(kind, UiControlInputKind::Count) &&
               IsKnown(activationSource, UiControlActivationSource::Count) && sequence != 0 &&
               (kind == UiControlInputKind::RepeatTick ? tick != 0 : true) &&
               (kind == UiControlInputKind::AdjustPress || kind == UiControlInputKind::AdjustRelease
                    ? IsKnown(adjustment, UiControlAdjustment::Count)
                    : adjustment == UiControlAdjustment::Count) &&
               text.IsValid();
    }

    /** @copydoc UiControlDefaultAction::IsValid */
    bool UiControlDefaultAction::IsValid() const noexcept {
        return source.IsValid() && action.IsValid() && IsKnown(kind, UiControlActionKind::Count) &&
               IsKnown(activationSource, UiControlActivationSource::Count) && eventSequence != 0 && IsValidControlPayload(payload);
    }

    struct UiControlStateMachine::Storage final {
        explicit Storage(UiControlDescriptor source) : descriptor(std::move(source)), state(InitialState(descriptor)) {
            if (const auto *text = std::get_if<UiTextInputControlDescriptor>(&descriptor); text != nullptr)
                editStartText = text->initialText;
        }

        void ClearTransient(const bool clearFocus) noexcept {
            SetPressed(state, false);
            SetRepeating(state, false);
            SetEditing(state, false);
            if (clearFocus)
                SetFocused(state, false);
            pending = false;
            repeatArmed = false;
            repeatNextTick = 0;
            adjustment = UiControlAdjustment::Count;
        }

        void SetAvailabilityProjection(const UiControlAvailability availability) noexcept {
            std::visit([availability](auto &typed) noexcept {
                typed.availability = availability;
            }, state);
        }

        [[nodiscard]] Result<void> ArmRepeat(const UiControlRepeatPolicy &policy, const std::uint64_t tick) {
            if (!policy.enabled)
                return Result<void>::Success();
            if (tick > std::numeric_limits<std::uint64_t>::max() - policy.initialDelayTicks)
                return Failure(UiErrors::ControlSequenceInvalid);
            repeatNextTick = tick + policy.initialDelayTicks;
            repeatArmed = true;
            SetRepeating(state, false);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> Queue(const PendingDefault queued) {
            if (pending)
                return Failure(UiErrors::ControlDefaultPending);
            pendingDefault = queued;
            pending = true;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<UiControlTransitionKind> ApplyInput(const UiControlInput &input) {
            const UiControlKind kind = KindOf(descriptor);
            const UiControlDescriptorBase &base = BaseOf(descriptor);
            const std::uint64_t tick = EventTick(input);

            switch (input.kind) {
                case UiControlInputKind::FocusGained:
                    if (!IsEnabled(state) || !base.focusable)
                        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::IgnoredDisabled);
                    if (IsFocused(state))
                        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::NoOp);
                    SetFocused(state, true);
                    if (kind == UiControlKind::TextInput) {
                        editStartText = std::get<UiTextInputControlState>(state).text;
                        SetEditing(state, true);
                    }
                    return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::Focused);

                case UiControlInputKind::FocusLost: {
                    const bool changed = IsFocused(state) || IsPressed(state) || pending;
                    ClearTransient(true);
                    return Result<UiControlTransitionKind>::Success(changed ? UiControlTransitionKind::Unfocused
                                                                            : UiControlTransitionKind::NoOp);
                }

                case UiControlInputKind::Cancel: {
                    const bool changed = IsFocused(state) || IsPressed(state) || pending ||
                                         (kind == UiControlKind::TextInput && std::get<UiTextInputControlState>(state).editing);
                    if (kind == UiControlKind::TextInput && std::get<UiTextInputControlState>(state).editing)
                        std::get<UiTextInputControlState>(state).text = editStartText;
                    ClearTransient(false);
                    return Result<UiControlTransitionKind>::Success(changed ? UiControlTransitionKind::Cancelled
                                                                            : UiControlTransitionKind::NoOp);
                }

                case UiControlInputKind::PointerPress:
                    if (!IsEnabled(state))
                        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::IgnoredDisabled);
                    if (kind == UiControlKind::TextInput) {
                        if (base.focusable)
                            SetFocused(state, true);
                        if (!std::get<UiTextInputControlState>(state).editing) {
                            editStartText = std::get<UiTextInputControlState>(state).text;
                            SetEditing(state, true);
                        }
                        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::Focused);
                    }
                    if (kind == UiControlKind::Slider)
                        return Failure<UiControlTransitionKind>(UiErrors::ControlInputInvalid);
                    if (const auto armed = ArmRepeat(base.repeat, tick); armed.HasError())
                        return Result<UiControlTransitionKind>::Failure(armed.ErrorValue());
                    if (base.focusable)
                        SetFocused(state, true);
                    SetPressed(state, true);
                    pressSource = input.activationSource;
                    adjustment = UiControlAdjustment::Count;
                    return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::Pressed);

                case UiControlInputKind::SubmitPress:
                    if (!IsEnabled(state))
                        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::IgnoredDisabled);
                    if (!IsFocused(state))
                        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::IgnoredUnfocused);
                    if (kind == UiControlKind::Slider)
                        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::NoOp);
                    if (kind == UiControlKind::TextInput && !std::get<UiTextInputControlState>(state).editing) {
                        editStartText = std::get<UiTextInputControlState>(state).text;
                        SetEditing(state, true);
                    }
                    if (const auto armed = ArmRepeat(base.repeat, tick); armed.HasError())
                        return Result<UiControlTransitionKind>::Failure(armed.ErrorValue());
                    SetPressed(state, true);
                    pressSource = input.activationSource;
                    adjustment = UiControlAdjustment::Count;
                    return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::Pressed);

                case UiControlInputKind::AdjustPress: {
                    if (kind != UiControlKind::Slider)
                        return Failure<UiControlTransitionKind>(UiErrors::ControlInputInvalid);
                    if (!IsEnabled(state))
                        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::IgnoredDisabled);
                    if (!IsFocused(state))
                        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::IgnoredUnfocused);
                    if (const auto armed = ArmRepeat(base.repeat, tick); armed.HasError())
                        return Result<UiControlTransitionKind>::Failure(armed.ErrorValue());
                    SetPressed(state, true);
                    SetEditing(state, true);
                    pressSource = input.activationSource;
                    adjustment = input.adjustment;
                    const auto &slider = std::get<UiSliderControlDescriptor>(descriptor);
                    const double next = AdjustedSliderValue(slider, SliderValue(state), input.adjustment);
                    if (next == SliderValue(state))
                        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::Pressed);
                    if (const auto queued = Queue({PendingKind::ValueChanged, input.activationSource, input.sequence, false, next});
                        queued.HasError())
                        return Result<UiControlTransitionKind>::Failure(queued.ErrorValue());
                    return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::DefaultPending);
                }

                case UiControlInputKind::PointerRelease:
                case UiControlInputKind::SubmitRelease:
                case UiControlInputKind::AdjustRelease: {
                    const bool matchingPress = IsPressed(state) && pressSource == input.activationSource;
                    if (!matchingPress) {
                        if (IsPressed(state))
                            ClearTransient(false);
                        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::Cancelled);
                    }
                    SetPressed(state, false);
                    SetRepeating(state, false);
                    repeatArmed = false;
                    repeatNextTick = 0;
                    if (input.kind == UiControlInputKind::AdjustRelease) {
                        SetEditing(state, false);
                        adjustment = UiControlAdjustment::Count;
                        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::Released);
                    }
                    if (kind == UiControlKind::Slider) {
                        SetEditing(state, false);
                        adjustment = UiControlAdjustment::Count;
                        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::Released);
                    }
                    const PendingKind pendingKind = kind == UiControlKind::Button   ? PendingKind::Activate
                                                    : kind == UiControlKind::Toggle ? PendingKind::Toggle
                                                                                    : PendingKind::Submit;
                    if (const auto queued = Queue({pendingKind, input.activationSource, input.sequence, false, 0.0}); queued.HasError())
                        return Result<UiControlTransitionKind>::Failure(queued.ErrorValue());
                    return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::DefaultPending);
                }

                case UiControlInputKind::TextInput:
                    if (kind != UiControlKind::TextInput)
                        return Failure<UiControlTransitionKind>(UiErrors::ControlInputInvalid);
                    if (!IsEnabled(state))
                        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::IgnoredDisabled);
                    if (!IsFocused(state) || !std::get<UiTextInputControlState>(state).editing)
                        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::IgnoredUnfocused);
                    if (input.text.size > std::get<UiTextInputControlDescriptor>(descriptor).maximumTextBytes -
                                              std::get<UiTextInputControlState>(state).text.size)
                        return Failure<UiControlTransitionKind>(UiErrors::ControlCapacityExceeded);
                    {
                        auto &text = std::get<UiTextInputControlState>(state).text;
                        std::copy(input.text.View().begin(), input.text.View().end(), text.bytes.begin() + text.size);
                        text.size = static_cast<std::uint16_t>(text.size + input.text.size);
                    }
                    return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::TextEdited);

                case UiControlInputKind::RepeatTick:
                    if (!IsEnabled(state) || !IsPressed(state) || !repeatArmed || !base.repeat.enabled)
                        return Result<UiControlTransitionKind>::Success(IsEnabled(state) ? UiControlTransitionKind::NoOp
                                                                                         : UiControlTransitionKind::IgnoredDisabled);
                    if (input.activationSource != pressSource)
                        return Failure<UiControlTransitionKind>(UiErrors::ControlInputInvalid);
                    if (input.tick < repeatNextTick)
                        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::NoOp);
                    if (input.tick > std::numeric_limits<std::uint64_t>::max() - base.repeat.intervalTicks)
                        return Failure<UiControlTransitionKind>(UiErrors::ControlSequenceInvalid);
                    repeatNextTick = input.tick + base.repeat.intervalTicks;
                    SetRepeating(state, true);
                    if (kind == UiControlKind::Slider) {
                        const auto &slider = std::get<UiSliderControlDescriptor>(descriptor);
                        const double next = AdjustedSliderValue(slider, SliderValue(state), adjustment);
                        if (next == SliderValue(state))
                            return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::NoOp);
                        if (const auto queued = Queue({PendingKind::ValueChanged, input.activationSource, input.sequence, true, next});
                            queued.HasError())
                            return Result<UiControlTransitionKind>::Failure(queued.ErrorValue());
                        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::DefaultPending);
                    }
                    if (kind == UiControlKind::Button || kind == UiControlKind::Toggle) {
                        const PendingKind pendingKind = kind == UiControlKind::Button ? PendingKind::Activate : PendingKind::Toggle;
                        if (const auto queued = Queue({pendingKind, input.activationSource, input.sequence, true, 0.0}); queued.HasError())
                            return Result<UiControlTransitionKind>::Failure(queued.ErrorValue());
                        return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::DefaultPending);
                    }
                    return Result<UiControlTransitionKind>::Success(UiControlTransitionKind::NoOp);

                case UiControlInputKind::Count:
                    break;
            }
            return Failure<UiControlTransitionKind>(UiErrors::ControlInputInvalid);
        }

        UiControlDescriptor descriptor;
        UiControlState state;
        UiActionText editStartText;
        PendingDefault pendingDefault;
        UiControlActivationSource pressSource{UiControlActivationSource::Programmatic};
        UiControlAdjustment adjustment{UiControlAdjustment::Count};
        std::uint64_t lastSequence{};
        std::uint64_t lastTick{};
        std::uint64_t repeatNextTick{};
        bool pending{};
        bool repeatArmed{};
        UiControlLifecycleState lifecycle{UiControlLifecycleState::Active};
    };

    /** @copydoc UiControlStateMachine::Create */
    Result<UiControlStateMachine> UiControlStateMachine::Create(const UiControlDescriptor &descriptor) {
        if (const auto valid = ValidateUiControlDescriptor(descriptor); valid.HasError())
            return Result<UiControlStateMachine>::Failure(valid.ErrorValue());
        try {
            return Result<UiControlStateMachine>::Success(UiControlStateMachine{std::make_unique<Storage>(descriptor)});
        } catch (const std::bad_alloc &) {
            return Failure<UiControlStateMachine>(UiErrors::CapacityExceeded);
        }
    }

    /** @copydoc UiControlStateMachine::UiControlStateMachine */
    UiControlStateMachine::UiControlStateMachine(std::unique_ptr<Storage> storage) noexcept : storage_(std::move(storage)) {}

    /** @copydoc UiControlStateMachine::~UiControlStateMachine */
    UiControlStateMachine::~UiControlStateMachine() {
        Shutdown();
    }

    /** @copydoc UiControlStateMachine::UiControlStateMachine */
    UiControlStateMachine::UiControlStateMachine(UiControlStateMachine &&) noexcept = default;

    /** @copydoc UiControlStateMachine::operator= */
    UiControlStateMachine &UiControlStateMachine::operator=(UiControlStateMachine &&) noexcept = default;

    /** @copydoc UiControlStateMachine::Kind */
    UiControlKind UiControlStateMachine::Kind() const noexcept {
        return storage_ ? KindOf(storage_->descriptor) : UiControlKind::Count;
    }

    /** @copydoc UiControlStateMachine::Owner */
    const UiActionOwnerContext &UiControlStateMachine::Owner() const noexcept {
        return storage_ ? BaseOf(storage_->descriptor).owner : InvalidOwner();
    }

    /** @copydoc UiControlStateMachine::Element */
    UiElementHandle UiControlStateMachine::Element() const noexcept {
        return storage_ ? BaseOf(storage_->descriptor).element : InvalidElement();
    }

    /** @copydoc UiControlStateMachine::LifecycleState */
    UiControlLifecycleState UiControlStateMachine::LifecycleState() const noexcept {
        return storage_ ? storage_->lifecycle : UiControlLifecycleState::Stopped;
    }

    /** @copydoc UiControlStateMachine::Snapshot */
    Result<UiControlState> UiControlStateMachine::Snapshot() const {
        if (!storage_ || storage_->lifecycle == UiControlLifecycleState::Stopped)
            return Failure<UiControlState>(UiErrors::ControlLifecycleUnavailable);
        return Result<UiControlState>::Success(storage_->state);
    }

    /** @copydoc UiControlStateMachine::Handle */
    Result<UiControlEventResult> UiControlStateMachine::Handle(UiControlInput input) {
        if (!storage_ || storage_->lifecycle != UiControlLifecycleState::Active)
            return Failure<UiControlEventResult>(UiErrors::ControlLifecycleUnavailable);
        if (!input.IsValid() || !IsPressSource(input.kind, input.activationSource) ||
            ((input.kind == UiControlInputKind::TextInput) && !IsValidControlText(input.text)) ||
            (input.kind != UiControlInputKind::TextInput && input.text.size != 0))
            return Failure<UiControlEventResult>(UiErrors::ControlInputInvalid);
        const UiControlDescriptorBase &base = BaseOf(storage_->descriptor);
        if (input.source.owner != base.owner || input.source.element != base.element)
            return Failure<UiControlEventResult>(UiErrors::ControlSourceStale);
        if (input.sequence <= storage_->lastSequence || (input.tick != 0 && input.tick < storage_->lastTick))
            return Failure<UiControlEventResult>(UiErrors::ControlSequenceInvalid);
        if (storage_->pending && input.kind != UiControlInputKind::Cancel && input.kind != UiControlInputKind::FocusLost)
            return Failure<UiControlEventResult>(UiErrors::ControlDefaultPending);
        const auto transition = storage_->ApplyInput(input);
        if (transition.HasError())
            return Result<UiControlEventResult>::Failure(transition.ErrorValue());
        storage_->lastSequence = input.sequence;
        if (input.tick != 0)
            storage_->lastTick = input.tick;
        return Result<UiControlEventResult>::Success({transition.Value(), storage_->state, storage_->pending});
    }

    /** @copydoc UiControlStateMachine::ApplyDefault */
    Result<std::optional<UiControlDefaultAction>> UiControlStateMachine::ApplyDefault() {
        if (!storage_ || storage_->lifecycle != UiControlLifecycleState::Active)
            return Failure<std::optional<UiControlDefaultAction>>(UiErrors::ControlLifecycleUnavailable);
        if (!storage_->pending)
            return Result<std::optional<UiControlDefaultAction>>::Success(std::nullopt);

        const UiControlDescriptorBase &base = BaseOf(storage_->descriptor);
        UiControlDefaultAction action;
        action.source = {base.owner, base.element};
        action.action = base.action;
        action.activationSource = storage_->pendingDefault.source;
        action.eventSequence = storage_->pendingDefault.sequence;
        action.repeated = storage_->pendingDefault.repeated;
        action.payload = base.payload;

        switch (storage_->pendingDefault.kind) {
            case PendingKind::Activate:
                action.kind = UiControlActionKind::Activate;
                break;
            case PendingKind::Toggle: {
                action.kind = UiControlActionKind::Toggle;
                const bool next = !std::get<UiToggleControlState>(storage_->state).checked;
                if (const auto added = action.payload.Add(next); added.HasError())
                    return Failure<std::optional<UiControlDefaultAction>>(UiErrors::ControlDefaultInvalid);
                break;
            }
            case PendingKind::ValueChanged:
                action.kind = UiControlActionKind::ValueChanged;
                if (const auto added = action.payload.Add(storage_->pendingDefault.value); added.HasError())
                    return Failure<std::optional<UiControlDefaultAction>>(UiErrors::ControlDefaultInvalid);
                break;
            case PendingKind::Submit:
                action.kind = UiControlActionKind::Submit;
                if (const auto added = action.payload.Add(std::get<UiTextInputControlState>(storage_->state).text); added.HasError())
                    return Failure<std::optional<UiControlDefaultAction>>(UiErrors::ControlDefaultInvalid);
                break;
        }
        if (!action.IsValid())
            return Failure<std::optional<UiControlDefaultAction>>(UiErrors::ControlDefaultInvalid);

        if (storage_->pendingDefault.kind == PendingKind::Toggle)
            std::get<UiToggleControlState>(storage_->state).checked = !std::get<UiToggleControlState>(storage_->state).checked;
        else if (storage_->pendingDefault.kind == PendingKind::ValueChanged)
            std::get<UiSliderControlState>(storage_->state).value = storage_->pendingDefault.value;
        else if (storage_->pendingDefault.kind == PendingKind::Submit &&
                 std::get<UiTextInputControlDescriptor>(storage_->descriptor).submitEndsEditing)
            std::get<UiTextInputControlState>(storage_->state).editing = false;

        storage_->pending = false;
        return Result<std::optional<UiControlDefaultAction>>::Success(std::optional<UiControlDefaultAction>{std::move(action)});
    }

    /** @copydoc UiControlStateMachine::SuppressDefault */
    Result<void> UiControlStateMachine::SuppressDefault() {
        if (!storage_ || storage_->lifecycle != UiControlLifecycleState::Active)
            return Failure(UiErrors::ControlLifecycleUnavailable);
        storage_->pending = false;
        return Result<void>::Success();
    }

    /** @copydoc UiControlStateMachine::SetAvailability */
    Result<void> UiControlStateMachine::SetAvailability(const UiControlAvailability availability) {
        if (!storage_ || storage_->lifecycle != UiControlLifecycleState::Active)
            return Failure(UiErrors::ControlLifecycleUnavailable);
        if (!IsKnown(availability, UiControlAvailability::Count))
            return Failure(UiErrors::ControlInputInvalid);
        if (availability == UiControlAvailability::Disabled) {
            storage_->ClearTransient(true);
            storage_->SetAvailabilityProjection(availability);
        } else {
            storage_->SetAvailabilityProjection(availability);
        }
        return Result<void>::Success();
    }

    /** @copydoc UiControlStateMachine::BeginRetirement */
    Result<void> UiControlStateMachine::BeginRetirement() {
        if (!storage_ || storage_->lifecycle != UiControlLifecycleState::Active)
            return Failure(UiErrors::ControlLifecycleUnavailable);
        storage_->ClearTransient(true);
        storage_->lifecycle = UiControlLifecycleState::Retiring;
        return Result<void>::Success();
    }

    /** @copydoc UiControlStateMachine::Shutdown */
    void UiControlStateMachine::Shutdown() noexcept {
        if (!storage_ || storage_->lifecycle == UiControlLifecycleState::Stopped)
            return;
        storage_->ClearTransient(true);
        storage_->SetAvailabilityProjection(UiControlAvailability::Disabled);
        storage_->lifecycle = UiControlLifecycleState::Stopped;
    }
}  // namespace Horo::Runtime::Ui
