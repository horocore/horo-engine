#pragma once

/**
 * @file UiControls.h
 * @brief Typed Runtime UI interactive-control state machines and default actions.
 */

#include "Horo/Runtime/Ui/UiActions.h"

#include <compare>
#include <cstdint>
#include <memory>
#include <optional>
#include <variant>

namespace Horo::Runtime::Ui {
    inline constexpr std::uint64_t MaximumUiControlRepeatTicks = 1'000'000'000;

    /** @brief Closed Runtime UI control taxonomy implemented by this state-machine contract. */
    enum class UiControlKind : std::uint8_t {
        Button,
        Toggle,
        Slider,
        TextInput,
        Count,
    };

    /** @brief Explicit availability projection used by every interactive control. */
    enum class UiControlAvailability : std::uint8_t {
        Enabled,
        Disabled,
        Count,
    };

    /** @brief Normalized source of a control interaction; raw devices stay outside Runtime UI. */
    enum class UiControlActivationSource : std::uint8_t {
        Pointer,
        Keyboard,
        Gamepad,
        Accessibility,
        Programmatic,
        Count,
    };

    /** @brief Input edge or owner event consumed by one control state machine. */
    enum class UiControlInputKind : std::uint8_t {
        PointerPress,
        PointerRelease,
        SubmitPress,
        SubmitRelease,
        AdjustPress,
        AdjustRelease,
        Cancel,
        FocusGained,
        FocusLost,
        TextInput,
        RepeatTick,
        Count,
    };

    /** @brief Direction of one typed scalar-control adjustment. */
    enum class UiControlAdjustment : std::uint8_t {
        Decrease,
        Increase,
        Count,
    };

    /** @brief Stable semantic outcome emitted by one applied control default action. */
    enum class UiControlActionKind : std::uint8_t {
        Activate,
        Toggle,
        ValueChanged,
        Submit,
        Count,
    };

    /** @brief Bounded state transition observed after one accepted control input. */
    enum class UiControlTransitionKind : std::uint8_t {
        NoOp,
        IgnoredDisabled,
        IgnoredUnfocused,
        Focused,
        Unfocused,
        Pressed,
        Released,
        Cancelled,
        TextEdited,
        DefaultPending,
        Count,
    };

    /** @brief Explicit lifecycle of one owner-bound interactive control. */
    enum class UiControlLifecycleState : std::uint8_t {
        Active,
        Retiring,
        Stopped,
    };

    /** @brief Finite deterministic repeat policy driven by owner-supplied ticks. */
    struct UiControlRepeatPolicy final {
        bool enabled{};                    /**< Whether held activation may repeat. */
        std::uint64_t initialDelayTicks{}; /**< Ticks from press to the first repeat. */
        std::uint64_t intervalTicks{};     /**< Ticks between subsequent repeats. */

        /** @brief Validates disabled/positive bounded delay and interval combinations. @return Whether the policy is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Shared typed ownership, command, availability and repeat descriptor for one control. */
    struct UiControlDescriptorBase final {
        UiActionOwnerContext owner;   /**< Exact runtime/canvas/document/revision generation. */
        UiElementHandle element;      /**< Exact retained-tree element represented by the control. */
        UiActionId action;            /**< Stable action identity emitted by default activation. */
        UiActionPayload payload;      /**< Bounded caller-authored action arguments. */
        bool initiallyEnabled{true};  /**< Initial availability; runtime changes use SetAvailability. */
        bool focusable{true};         /**< Whether keyboard/accessibility focus may enter this control. */
        UiControlRepeatPolicy repeat; /**< Held activation policy for this control. */

        /** @brief Validates owner, element, action, payload and repeat evidence. @return Whether the base is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Typed descriptor for a command-emitting push button. */
    struct UiButtonControlDescriptor final {
        UiControlDescriptorBase base; /**< Shared owner and activation contract. */

        /** @brief Validates the button descriptor. @return Whether it can be activated safely. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Typed descriptor for a boolean toggle control. */
    struct UiToggleControlDescriptor final {
        UiControlDescriptorBase base; /**< Shared owner and activation contract. */
        bool initiallyChecked{};      /**< Initial authored boolean value. */

        /** @brief Validates the toggle descriptor and value-bearing action capacity. @return Whether it is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Typed descriptor for a bounded stepped scalar control. */
    struct UiSliderControlDescriptor final {
        UiControlDescriptorBase base; /**< Shared owner and activation contract. */
        double minimum{};             /**< Inclusive finite minimum. */
        double maximum{1.0};          /**< Inclusive finite maximum. */
        double step{1.0};             /**< Positive finite adjustment step. */
        double initialValue{};        /**< Initial value in the inclusive range. */

        /** @brief Validates finite range, step and value evidence. @return Whether it is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Typed descriptor for bounded text editing and submission. */
    struct UiTextInputControlDescriptor final {
        UiControlDescriptorBase base;                             /**< Shared owner and activation contract. */
        UiActionText initialText;                                 /**< Initial bounded UTF-8 value. */
        std::uint16_t maximumTextBytes{MaximumUiActionTextBytes}; /**< Maximum UTF-8 byte count. */
        bool submitEndsEditing{true};                             /**< Whether a successful submit leaves editing mode. */

        /** @brief Validates UTF-8, text capacity and submit policy. @return Whether it is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Closed typed descriptor for one core interactive control. */
    using UiControlDescriptor =
        std::variant<UiButtonControlDescriptor, UiToggleControlDescriptor, UiSliderControlDescriptor, UiTextInputControlDescriptor>;

    /**
     * @brief Validates one closed control descriptor without consulting ambient runtime state.
     * @param descriptor Typed control descriptor to inspect.
     * @return Success or UiErrors::ControlDescriptorInvalid.
     */
    [[nodiscard]] Result<void> ValidateUiControlDescriptor(const UiControlDescriptor &descriptor);

    /**
     * @brief Returns the closed control kind represented by a descriptor.
     * @param descriptor Typed control descriptor to inspect.
     * @return Exact kind, or Count only for an impossible/unsupported value.
     */
    [[nodiscard]] UiControlKind UiControlKindOf(const UiControlDescriptor &descriptor) noexcept;

    /** @brief Immutable button state projection. */
    struct UiButtonControlState final {
        UiControlAvailability availability{UiControlAvailability::Enabled};
        bool focused{};
        bool pressed{};
        bool repeating{};
        [[nodiscard]] auto operator<=>(const UiButtonControlState &) const noexcept = default;
    };

    /** @brief Immutable toggle state projection. */
    struct UiToggleControlState final {
        UiControlAvailability availability{UiControlAvailability::Enabled};
        bool focused{};
        bool pressed{};
        bool checked{};
        bool repeating{};
        [[nodiscard]] auto operator<=>(const UiToggleControlState &) const noexcept = default;
    };

    /** @brief Immutable slider state projection. */
    struct UiSliderControlState final {
        UiControlAvailability availability{UiControlAvailability::Enabled};
        bool focused{};
        bool pressed{};
        bool editing{};
        bool repeating{};
        double value{};
        [[nodiscard]] auto operator<=>(const UiSliderControlState &) const noexcept = default;
    };

    /** @brief Immutable text-input state projection. */
    struct UiTextInputControlState final {
        UiControlAvailability availability{UiControlAvailability::Enabled};
        bool focused{};
        bool pressed{};
        bool editing{};
        UiActionText text;
    };

    /** @brief Closed typed state projection matching exactly one control descriptor kind. */
    using UiControlState = std::variant<UiButtonControlState, UiToggleControlState, UiSliderControlState, UiTextInputControlState>;

    /**
     * @brief Returns the closed control kind represented by a state projection.
     * @param state Typed control state to inspect.
     * @return Exact state kind.
     */
    [[nodiscard]] UiControlKind UiControlKindOf(const UiControlState &state) noexcept;

    /** @brief One normalized, owner-addressed control input; no native device types are retained. */
    struct UiControlInput final {
        UiActionSource source;     /**< Exact presented owner and target evidence. */
        UiControlInputKind kind{}; /**< Input edge or lifecycle event. */
        UiControlActivationSource activationSource{UiControlActivationSource::Programmatic}; /**< Semantic source modality. */
        std::uint64_t sequence{};                                                            /**< Non-zero owner-ordered input sequence. */
        std::uint64_t tick{};                                       /**< Optional monotonic owner tick; required for RepeatTick. */
        UiControlAdjustment adjustment{UiControlAdjustment::Count}; /**< Direction for AdjustPress/AdjustRelease. */
        UiActionText text;                                          /**< Bounded UTF-8 payload for TextInput; empty otherwise. */

        /** @brief Validates representation before owner-specific matching. @return Whether the input is well formed. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief One typed default action emitted only after routing has allowed application. */
    struct UiControlDefaultAction final {
        UiActionSource source;                                /**< Exact owner and control target that produced the action. */
        UiActionId action;                                    /**< Stable authored action identity. */
        UiControlActionKind kind{UiControlActionKind::Count}; /**< Semantic action operation. */
        UiControlActivationSource activationSource{UiControlActivationSource::Programmatic}; /**< Origin modality. */
        std::uint64_t eventSequence{}; /**< Input sequence that prepared this action. */
        bool repeated{};               /**< Whether this was produced by held-input repeat. */
        UiActionPayload payload;       /**< Bounded arguments, with control value appended when applicable. */

        /** @brief Validates action identity, source correlation and bounded payload. @return Whether the action is valid. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief State projection returned after one accepted input edge. */
    struct UiControlEventResult final {
        UiControlTransitionKind transition{UiControlTransitionKind::NoOp};
        UiControlState state;
        bool defaultActionPending{}; /**< True only while ApplyDefault/SuppressDefault must resolve the route outcome. */
    };

    /** @brief Preallocated owner-thread state machine for one typed interactive control. */
    class UiControlStateMachine final {
    public:
        /**
         * @brief Creates a control and reserves all mutable state for its owner generation.
         * @param descriptor Typed immutable control contract.
         * @return Active state machine or typed descriptor/capacity failure.
         */
        [[nodiscard]] static Result<UiControlStateMachine> Create(const UiControlDescriptor &descriptor);
        ~UiControlStateMachine();
        UiControlStateMachine(UiControlStateMachine &&) noexcept;
        UiControlStateMachine &operator=(UiControlStateMachine &&) noexcept;
        UiControlStateMachine(const UiControlStateMachine &) = delete;
        UiControlStateMachine &operator=(const UiControlStateMachine &) = delete;

        /** @brief Returns the exact control kind. @return Closed kind or Count after move. */
        [[nodiscard]] UiControlKind Kind() const noexcept;
        /** @brief Returns immutable owner evidence. @return Owner context or invalid context after move. */
        [[nodiscard]] const UiActionOwnerContext &Owner() const noexcept;
        /** @brief Returns the exact controlled element. @return Handle or invalid handle after move. */
        [[nodiscard]] UiElementHandle Element() const noexcept;
        /** @brief Returns lifecycle state. @return Active, Retiring or Stopped. */
        [[nodiscard]] UiControlLifecycleState LifecycleState() const noexcept;

        /**
         * @brief Copies the current typed state without allocation.
         * @return State while active or retiring, or lifecycle failure after shutdown.
         */
        [[nodiscard]] Result<UiControlState> Snapshot() const;

        /**
         * @brief Consumes one normalized input edge and stages, but does not apply, a default action.
         * @param input Owner-addressed event from the last presented interaction generation.
         * @return Transition and state projection, or typed stale, disabled, ordering, capacity or lifecycle failure.
         * @post A successful DefaultPending result must be resolved by ApplyDefault or SuppressDefault before another input.
         */
        [[nodiscard]] Result<UiControlEventResult> Handle(UiControlInput input);

        /**
         * @brief Applies the one staged default action in deterministic owner order.
         * @return Empty when no action is staged, otherwise one typed action; no allocation occurs.
         */
        [[nodiscard]] Result<std::optional<UiControlDefaultAction>> ApplyDefault();

        /**
         * @brief Suppresses the staged default action after a routed handler prevented it.
         * @return Success; repeated suppression with no staged action is harmless.
         */
        [[nodiscard]] Result<void> SuppressDefault();

        /**
         * @brief Changes availability at an owner safe point and cancels transient interaction state when disabling.
         * @param availability New explicit availability.
         * @return Success or typed lifecycle/state failure.
         */
        [[nodiscard]] Result<void> SetAvailability(UiControlAvailability availability);

        /** @brief Closes input/default admission and releases transient focus/press state. @return Success or lifecycle failure. */
        [[nodiscard]] Result<void> BeginRetirement();
        /** @brief Idempotently stops the machine and clears pending actions. */
        void Shutdown() noexcept;

    private:
        struct Storage;
        explicit UiControlStateMachine(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
    };
}  // namespace Horo::Runtime::Ui
