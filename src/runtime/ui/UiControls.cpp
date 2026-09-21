#include "Horo/Foundation/Utf8.h"
#include "Horo/Runtime/Ui/UiControlsInternal.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#include <cmath>
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
    }  // namespace UiControlDetail

    /** @copydoc UiControlRepeatPolicy::IsValid */
    bool UiControlRepeatPolicy::IsValid() const noexcept {
        if (!enabled)
            return initialDelayTicks == 0 && intervalTicks == 0;
        return initialDelayTicks > 0 && initialDelayTicks <= MaximumUiControlRepeatTicks && intervalTicks > 0 &&
               intervalTicks <= MaximumUiControlRepeatTicks;
    }

    /** @copydoc UiControlDescriptorBase::IsValid */
    bool UiControlDescriptorBase::IsValid() const noexcept {
        return UiActionSource{owner, element}.IsValid() && action.IsValid() && UiControlDetail::IsValidControlPayload(payload) &&
               repeat.IsValid();
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
               maximumTextBytes <= MaximumUiActionTextBytes && UiControlDetail::IsValidControlText(initialText) &&
               initialText.size <= maximumTextBytes;
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
        return UiControlDetail::KindOf(descriptor);
    }

    /** @copydoc UiControlKindOf */
    UiControlKind UiControlKindOf(const UiControlState &state) noexcept {
        return UiControlDetail::KindOf(state);
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
               IsKnown(activationSource, UiControlActivationSource::Count) && eventSequence != 0 &&
               UiControlDetail::IsValidControlPayload(payload);
    }
}  // namespace Horo::Runtime::Ui
