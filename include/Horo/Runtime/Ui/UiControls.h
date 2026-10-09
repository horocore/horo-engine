#pragma once

#include "Horo/Runtime/Ui/UiTextEditing.h"

/**
 * @file UiControls.h
 * @brief Typed Runtime UI interactive-control state machines and default actions.
 */

#include "Horo/Runtime/Ui/UiActions.h"
#include "Horo/Runtime/Ui/UiAsyncActions.h"

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
        Busy,
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

    /** @brief Text-control editing options; the descriptor alone owns its UTF-8 byte ceiling. */
    struct UiTextInputEditingOptions final {
        std::uint16_t maximumGraphemes{MaximumUiActionTextBytes};
        std::uint16_t undoDepth{MaximumUiTextUndoDepth};
        UiTextValidation validation{UiTextValidation::Any};
        bool password{};
        [[nodiscard]] bool operator==(const UiTextInputEditingOptions &) const noexcept = default;
    };

    /** @brief Typed descriptor for bounded text editing and submission. */
    struct UiTextInputControlDescriptor final {
        UiControlDescriptorBase base;                             /**< Shared owner and activation contract. */
        UiActionText initialText;                                 /**< Initial bounded UTF-8 value. */
        std::uint16_t maximumTextBytes{MaximumUiActionTextBytes}; /**< Maximum UTF-8 byte count. */
        bool submitEndsEditing{true};                             /**< Whether a successful submit leaves editing mode. */
        UiTextInputEditingOptions editing;                        /**< Grapheme, validation, history and password policy. */

        /** @brief Combines the single byte ceiling with editing options. @return Effective buffer policy. */
        [[nodiscard]] UiTextEditPolicy EditPolicy() const noexcept;

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

        /** @brief Compares the projected state using the logical bounded text value. */
        [[nodiscard]] std::strong_ordering operator<=>(const UiTextInputControlState &other) const noexcept {
            if (const auto comparison = availability <=> other.availability; comparison != 0)
                return comparison;
            if (focused != other.focused)
                return focused ? std::strong_ordering::greater : std::strong_ordering::less;
            if (pressed != other.pressed)
                return pressed ? std::strong_ordering::greater : std::strong_ordering::less;
            if (editing != other.editing)
                return editing ? std::strong_ordering::greater : std::strong_ordering::less;
            return text.View() <=> other.text.View();
        }

        [[nodiscard]] bool operator==(const UiTextInputControlState &other) const noexcept {
            return (*this <=> other) == 0;
        }
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

    /** @brief Opaque copied actual control state used to reject a prepared reload after owner mutation. */
    class UiControlReloadStamp final {
    private:
        friend class UiControlStateMachine;
        UiControlState state_;
        UiControlDescriptor descriptor_;
        UiActionText editStartText_;
        std::uint64_t sequence_{};
        std::uint64_t tick_{};
        std::uint64_t textResetRevision_{};
        bool pending_{};
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
        [[nodiscard]] Result<UiControlEventResult> Handle(const UiControlInput &input);

        /** @brief Applies normalized editing after the route has admitted its UI-local default.
         * @param source Exact presented owner and element; no native input object is retained.
         * @param sequence Strictly increasing sequence shared with Handle inputs.
         * @param command Bounded edit or host clipboard copy.
         * @return Edit/clipboard outcome or typed stale, disabled, unfocused, pending or validation failure.
         * @post Text submission still uses Handle/ApplyDefault and never writes gameplay/provider state.
         */
        [[nodiscard]] Result<UiTextEditResult> EditText(const UiActionSource &source, std::uint64_t sequence,
                                                        const UiTextEditCommand &command);
        /** @brief Copies logical caret/selection and semantic text for the owning input adapter.
         * @return Text edit state or typed kind/lifecycle failure; not a render projection.
         */
        [[nodiscard]] Result<UiTextEditSnapshot> TextEditSnapshot() const;
        /** @brief Copies display-safe text for layout/accessibility/render extraction.
         * @return Plain text or one password mask per grapheme, or typed kind/lifecycle failure.
         */
        [[nodiscard]] Result<UiTextEditDisplay> TextDisplay() const;

        /**
         * @brief Copies the staged default action without changing the control value or pending decision.
         * @return Empty when no action is staged, otherwise the exact action ApplyDefault would emit; no allocation occurs.
         * @note The owner may admit a binding write before applying the UI-local default, or suppress a refused write.
         */
        [[nodiscard]] Result<std::optional<UiControlDefaultAction>> PeekDefault() const;

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
         * @brief Reconciles a value control to authoritative provider state at an owner safe point.
         * @param value Boolean for Toggle, finite in-range double for Slider, or bounded valid UTF-8 text for TextInput.
         * @return Success or typed lifecycle/value failure; rejected values leave all control state unchanged.
         * @post Availability and focus are preserved; press, repeat, pending default and editing state are cleared.
         * @note The caller fences provider outcome ownership before calling; this operation performs no allocation.
         */
        [[nodiscard]] Result<void> ReconcileValue(const UiActionValue &value);

        /**
         * @brief Preserves compatible logical form state from an actual old owner during private reload preparation.
         * @param source Read-only old control, lifetime-pinned by the generation being reconciled.
         * @param preserveFocus Whether the replacement focus owner admitted this authored element.
         * @return True when kind, stable action/arguments, scope and new value constraints agree; false leaves the
         * replacement's authored initial state unchanged. Lifecycle failures are typed errors.
         * @post Copies values and compatible text edit draft/cancel baseline. Clears press, repeat, pending actions,
         * async operation projections and input ordering; no old runtime handle or native IME state migrates.
         * @pre Both owners are active and owner-thread serialized; the caller proves the same authored element and type.
         */
        [[nodiscard]] Result<bool> ReconcileReload(const UiControlStateMachine &source, bool preserveFocus);
        /** @brief Copies current logical/transient source evidence without allocation.
         * @return Opaque exact stamp or lifecycle failure; no handle or callback is retained.
         */
        [[nodiscard]] Result<UiControlReloadStamp> CaptureReloadStamp() const;
        /** @brief Compares actual state, draft baseline, ordering and pending decision with copied preparation evidence.
         * @param stamp Owner-copied previous evidence. @return True only while active and unchanged.
         */
        [[nodiscard]] bool MatchesReloadStamp(const UiControlReloadStamp &stamp) const noexcept;

        /**
         * @brief Changes availability at an owner safe point and cancels transient interaction state when disabling.
         * @param availability Enabled or Disabled; Busy is derived exclusively from the action owner projection.
         * @return Success or typed lifecycle/state failure.
         */
        [[nodiscard]] Result<void> SetAvailability(UiControlAvailability availability);

        /**
         * @brief Copies the current action owner's projection and derives effective busy availability.
         * @param actions Router-owned operation store for this exact source generation.
         * @return Success or typed stale/lifecycle failure. Pending work clears transient activation;
         * terminal state restores the separately configured availability. No callback or operation is owned by the control.
         */
        [[nodiscard]] Result<void> ObserveAsyncActions(const UiAsyncActionStore &actions);
        /** @brief Copies retained progress/terminal/error presentation state. @return Optional immutable projection or lifecycle failure.
         */
        [[nodiscard]] Result<std::optional<UiAsyncActionSnapshot>> AsyncAction() const;

        /** @brief Closes input/default admission and releases transient focus/press state. @return Success or lifecycle failure. */
        [[nodiscard]] Result<void> BeginRetirement();
        /** @brief Idempotently stops the machine and clears pending actions. */
        void Shutdown() noexcept;

    private:
        struct Storage;
        friend class UiAnimationOwner;
        /** @brief Checks storage readiness and exact semantic audience before preparing an interaction-only replacement. */
        [[nodiscard]] Result<void> CanPrepareInteractionReplacement(const UiActionOwnerContext &owner) const;
        /** @brief Load-time reservation for a distinct inactive immutable action-source generation. @return Reservation or failure. */
        [[nodiscard]] Result<void> ReserveInteractionReplacement();
        /** @brief Copies compatible logical state into a new source generation only after pending work is drained. @return Admission. */
        [[nodiscard]] Result<void> PrepareInteractionReplacement(const UiActionOwnerContext &owner);
        /** @brief Checks copied source state and replacement admission without publishing. @return Admission or stale failure. */
        [[nodiscard]] Result<void> CanPublishInteractionReplacement(const UiActionOwnerContext &owner) const;
        /** @brief Borrows the copied replacement state only while its private reservation is admitted. @return State. */
        [[nodiscard]] const UiControlState &PreparedInteractionState() const noexcept;
        /** @brief Swaps already prepared uniquely owned generations without allocation; the old descriptor is never mutated. */
        void PublishInteractionReplacement() noexcept;
        /** @brief Drops the unpublished replacement reservation without altering active logical state. */
        void AbandonInteractionReplacement() noexcept;
        /** @brief Releases the inactive generation's terminal projection pins at explicit owner quiescence. @return Released count. */
        [[nodiscard]] std::size_t DrainInteractionReplacement() noexcept;
        explicit UiControlStateMachine(std::unique_ptr<Storage> storage) noexcept;
        std::unique_ptr<Storage> storage_;
        std::unique_ptr<Storage> replacement_;
        std::optional<UiControlReloadStamp> replacementSource_;
        bool replacementSwap_{};
    };
}  // namespace Horo::Runtime::Ui
