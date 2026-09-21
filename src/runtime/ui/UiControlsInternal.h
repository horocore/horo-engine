#pragma once

#include "Horo/Runtime/Ui/UiControls.h"

namespace Horo::Runtime::Ui::UiControlDetail {
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

    [[nodiscard]] bool IsValidControlText(const UiActionText &text) noexcept;
    [[nodiscard]] bool IsValidControlPayload(const UiActionPayload &payload) noexcept;
    [[nodiscard]] const UiControlDescriptorBase &BaseOf(const UiControlDescriptor &descriptor) noexcept;
    [[nodiscard]] UiControlKind KindOf(const UiControlDescriptor &descriptor) noexcept;
    [[nodiscard]] UiControlState InitialState(const UiControlDescriptor &descriptor);
    [[nodiscard]] bool IsEnabled(const UiControlState &state) noexcept;
    [[nodiscard]] bool IsFocused(const UiControlState &state) noexcept;
    [[nodiscard]] bool IsPressed(const UiControlState &state) noexcept;
    void SetFocused(UiControlState &state, bool focused) noexcept;
    void SetPressed(UiControlState &state, bool pressed) noexcept;
    void SetRepeating(UiControlState &state, bool repeating) noexcept;
    void SetEditing(UiControlState &state, bool editing) noexcept;
    [[nodiscard]] double SliderValue(const UiControlState &state) noexcept;
    [[nodiscard]] UiControlKind KindOf(const UiControlState &state) noexcept;
    [[nodiscard]] bool IsPressSource(UiControlInputKind kind, UiControlActivationSource source) noexcept;
    [[nodiscard]] std::uint64_t EventTick(const UiControlInput &input) noexcept;
    [[nodiscard]] double AdjustedSliderValue(const UiSliderControlDescriptor &descriptor, double current,
                                             UiControlAdjustment adjustment) noexcept;
    [[nodiscard]] const UiActionOwnerContext &InvalidOwner() noexcept;
    [[nodiscard]] UiElementHandle InvalidElement() noexcept;
}  // namespace Horo::Runtime::Ui::UiControlDetail

namespace Horo::Runtime::Ui {
    struct UiControlStateMachine::Storage final {
        explicit Storage(UiControlDescriptor source);

        void ClearTransient(bool clearFocus) noexcept;
        void SetAvailabilityProjection(UiControlAvailability availability) noexcept;
        [[nodiscard]] Result<void> ArmRepeat(const UiControlRepeatPolicy &policy, std::uint64_t tick);
        [[nodiscard]] Result<void> Queue(const UiControlDetail::PendingDefault &queued);

        [[nodiscard]] Result<UiControlTransitionKind> ApplyInput(const UiControlInput &input);
        [[nodiscard]] Result<UiControlTransitionKind> HandleFocusGained();
        [[nodiscard]] Result<UiControlTransitionKind> HandleFocusLost();
        [[nodiscard]] Result<UiControlTransitionKind> HandleCancel();
        [[nodiscard]] Result<UiControlTransitionKind> HandlePointerPress(const UiControlInput &input);
        [[nodiscard]] Result<UiControlTransitionKind> HandleSubmitPress(const UiControlInput &input);
        [[nodiscard]] Result<UiControlTransitionKind> HandleAdjustPress(const UiControlInput &input);
        [[nodiscard]] Result<UiControlTransitionKind> HandleRelease(const UiControlInput &input);
        [[nodiscard]] Result<UiControlTransitionKind> HandleTextInput(const UiControlInput &input);
        [[nodiscard]] Result<UiControlTransitionKind> HandleRepeatTick(const UiControlInput &input);

        UiControlDescriptor descriptor;
        UiControlState state;
        UiActionText editStartText;
        UiControlDetail::PendingDefault pendingDefault;
        UiControlActivationSource pressSource{UiControlActivationSource::Programmatic};
        UiControlAdjustment adjustment{UiControlAdjustment::Count};
        std::uint64_t lastSequence{};
        std::uint64_t lastTick{};
        std::uint64_t repeatNextTick{};
        bool pending{};
        bool repeatArmed{};
        UiControlLifecycleState lifecycle{UiControlLifecycleState::Active};
    };
}  // namespace Horo::Runtime::Ui
