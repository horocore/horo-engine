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
        /** @brief Reconciles a typed owner value after lifecycle admission, preserving all state on rejection. */
        [[nodiscard]] Result<void> ReconcileValue(const UiActionValue &value);
        /** @brief Validates and reconciles the toggle's boolean value. */
        [[nodiscard]] Result<void> ReconcileToggle(const UiActionValue &value);
        /** @brief Validates and reconciles the slider's finite constrained scalar. */
        [[nodiscard]] Result<void> ReconcileSlider(const UiActionValue &value);
        /** @brief Validates and reconciles the complete text draft and cancellation baseline. */
        [[nodiscard]] Result<void> ReconcileText(const UiActionValue &value);
        /** @brief Checks text kind, exact source, ordering and edit admission without mutation. */
        [[nodiscard]] Result<void> AdmitTextEdit(const UiActionSource &source, std::uint64_t sequence) const;
        /** @brief Detects a pending default or a busy asynchronous action that prevents interaction replacement. */
        [[nodiscard]] bool BlocksInteractionReplacement() const noexcept;

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
        std::optional<UiAsyncActionSnapshot> asyncAction;
        UiControlAvailability configuredAvailability{UiControlAvailability::Enabled};
        UiActionText editStartText;
        std::unique_ptr<UiTextEditBuffer> textEditor;
        UiControlDetail::PendingDefault pendingDefault;
        UiControlActivationSource pressSource{UiControlActivationSource::Programmatic};
        UiControlAdjustment adjustment{UiControlAdjustment::Count};
        std::uint64_t lastSequence{};
        std::uint64_t lastTick{};
        std::uint64_t textResetRevision{}; /**< Owner-thread reset fence; saturation permanently rejects new reload stamps. */
        std::uint64_t repeatNextTick{};
        bool pending{};
        bool repeatArmed{};
        UiControlLifecycleState lifecycle{UiControlLifecycleState::Active};
    };
}  // namespace Horo::Runtime::Ui
