#pragma once

/**
 * @file Input.h
 * @brief Backend-neutral input snapshots, action routing, capture, gamepads, profiles, and tick frames.
 */

#include "Horo/Foundation/Result.h"

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Input {
    using FrameNumber = std::uint64_t;
    using SimulationTick = std::uint64_t;
    using PlayerId = std::uint8_t;

    /** @brief Stable physical-key identity independent of a native window API. */
    enum class Key : std::uint16_t {
        Unknown,
        A,
        B,
        C,
        D,
        E,
        F,
        G,
        H,
        I,
        J,
        K,
        L,
        M,
        N,
        O,
        P,
        Q,
        R,
        S,
        T,
        U,
        V,
        W,
        X,
        Y,
        Z,
        Digit0,
        Digit1,
        Digit2,
        Digit3,
        Digit4,
        Digit5,
        Digit6,
        Digit7,
        Digit8,
        Digit9,
        Escape,
        Enter,
        Tab,
        Space,
        Backspace,
        Delete,
        Left,
        Right,
        Up,
        Down,
        Home,
        End,
        PageUp,
        PageDown,
        F1,
        F2,
        F3,
        F4,
        F5,
        F6,
        F7,
        F8,
        F9,
        F10,
        F11,
        F12,
        Count,
    };

    /** @brief Backend-neutral pointer buttons used by snapshots and bindings. */
    enum class PointerButton : std::uint8_t {
        Primary,
        Secondary,
        Middle,
        Auxiliary1,
        Auxiliary2,
        Count,
    };

    /** @brief Canonical gamepad buttons shared by every mapped backend. */
    enum class GamepadButton : std::uint8_t {
        South,
        East,
        West,
        North,
        LeftShoulder,
        RightShoulder,
        LeftStick,
        RightStick,
        Start,
        Select,
        DPadUp,
        DPadDown,
        DPadLeft,
        DPadRight,
        Count,
    };

    /** @brief Canonical normalized gamepad axes shared by every mapped backend. */
    enum class GamepadAxis : std::uint8_t {
        LeftX,
        LeftY,
        RightX,
        RightY,
        LeftTrigger,
        RightTrigger,
        Count,
    };

    /** @brief Held and frame-edge state for one digital control. */
    struct ButtonState {
        bool down{false};
        bool pressed{false};
        bool released{false};
    };

    /** @brief Logical keyboard modifier state for the committed frame. */
    struct ModifierState {
        bool control{false};
        bool shift{false};
        bool alt{false};
        bool command{false};
        [[nodiscard]] friend constexpr bool operator==(const ModifierState &, const ModifierState &) noexcept = default;
    };

    /** @brief Pointer position, frame deltas, wheel deltas, and buttons. */
    struct PointerState {
        float x{0.0F};
        float y{0.0F};
        float deltaX{0.0F};
        float deltaY{0.0F};
        float wheelX{0.0F};
        float wheelY{0.0F};
        std::array<ButtonState, static_cast<std::size_t>(PointerButton::Count)> buttons{};
    };

    /** @brief Focus and pointer availability state for the collection surface. */
    struct WindowInputState {
        bool focused{true};
        bool pointerInside{true};
        bool pointerDeviceAvailable{true};
    };

    /** @brief Current native IME pre-edit text and selection range for the focused text surface. */
    struct TextCompositionState {
        std::string text;
        std::int32_t selectionStart{0};
        std::int32_t selectionLength{0};
        bool active{false};
    };

    /** @brief Slot plus session generation handle that rejects stale device access. */
    struct GamepadDeviceId {
        std::uint32_t slot{0};
        std::uint64_t sessionGeneration{0};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return sessionGeneration != 0;
        }

        [[nodiscard]] friend constexpr bool operator==(GamepadDeviceId, GamepadDeviceId) noexcept = default;
    };

    /** @brief Canonical and bounded raw state for one connected gamepad or joystick. */
    struct GamepadState {
        GamepadDeviceId id{};
        std::string name;
        bool canonicalMapping{true};
        std::array<ButtonState, static_cast<std::size_t>(GamepadButton::Count)> buttons{};
        std::array<float, static_cast<std::size_t>(GamepadAxis::Count)> axes{};
        std::vector<ButtonState> rawButtons;
        std::vector<float> rawAxes;
    };

    /** @brief Immutable value snapshot committed once after platform event polling. */
    struct RawInputSnapshot {
        FrameNumber frame{0};
        std::array<ButtonState, static_cast<std::size_t>(Key::Count)> keyboard{};
        PointerState pointer{};
        std::vector<GamepadState> gamepads;
        std::string text;
        TextCompositionState composition{};
        ModifierState modifiers{};
        WindowInputState window{};

        [[nodiscard]] const ButtonState &State(Key key) const noexcept;
        [[nodiscard]] const ButtonState &State(PointerButton button) const noexcept;
        [[nodiscard]] const GamepadState *FindGamepad(GamepadDeviceId id) const noexcept;
    };

    /** @brief Mutable platform/virtual collection endpoint; commits immutable frame snapshots. */
    class RawInputCollector {
    public:
        RawInputCollector();
        ~RawInputCollector();
        RawInputCollector(const RawInputCollector &) = delete;
        RawInputCollector &operator=(const RawInputCollector &) = delete;

        void BeginFrame(FrameNumber frame);
        void SetKey(Key key, bool down);
        void SetPointerButton(PointerButton button, bool down);
        void SetPointerPosition(float x, float y);
        void AddPointerWheel(float x, float y);
        void AppendText(std::string_view utf8);
        void SetTextComposition(std::string_view utf8, std::int32_t selectionStart, std::int32_t selectionLength);
        void SetModifiers(ModifierState modifiers) noexcept;
        void SetWindowState(WindowInputState state) noexcept;
        /** @brief Release all held controls and zero axes after focus or device ownership is lost. */
        void Neutralize() noexcept;
        [[nodiscard]] GamepadDeviceId ConnectGamepad(std::string name, bool canonicalMapping = true, std::size_t rawButtonCount = 0,
                                                     std::size_t rawAxisCount = 0);
        [[nodiscard]] bool DisconnectGamepad(GamepadDeviceId id);
        [[nodiscard]] bool SetGamepadButton(GamepadDeviceId id, GamepadButton button, bool down);
        [[nodiscard]] bool SetGamepadAxis(GamepadDeviceId id, GamepadAxis axis, float value);
        [[nodiscard]] bool SetRawGamepadButton(GamepadDeviceId id, std::size_t button, bool down);
        [[nodiscard]] bool SetRawGamepadAxis(GamepadDeviceId id, std::size_t axis, float value);
        [[nodiscard]] const RawInputSnapshot &Commit();

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    /** @brief Stable semantic action identifier. */
    class ActionId {
    public:
        ActionId() = default;

        explicit ActionId(std::string value) : value_(std::move(value)) {}

        [[nodiscard]] bool IsValid() const noexcept {
            return !value_.empty();
        }

        [[nodiscard]] const std::string &Value() const noexcept {
            return value_;
        }

        [[nodiscard]] friend bool operator==(const ActionId &, const ActionId &) noexcept = default;

    private:
        std::string value_;
    };

    /** @brief Normative routing priority, ordered from lowest to highest. */
    enum class InputContextKind : std::uint8_t {
        GlobalNonMutating,
        Gameplay,
        EditorWorkspace,
        EditorToolCapture,
        FocusedGuiWidget,
        ModalRoot,
        ModalChild,
        NativeDialog,
    };

    /** @brief Stable identifier shared by action descriptors and live contexts. */
    class InputContextId {
    public:
        InputContextId() = default;

        explicit InputContextId(std::string value) : value_(std::move(value)) {}

        [[nodiscard]] bool IsValid() const noexcept {
            return !value_.empty();
        }

        [[nodiscard]] const std::string &Value() const noexcept {
            return value_;
        }

        [[nodiscard]] friend bool operator==(const InputContextId &, const InputContextId &) noexcept = default;

    private:
        std::string value_;
    };

    /** @brief Shape of a resolved semantic action value. */
    enum class ActionValueType : std::uint8_t {
        Digital,
        Axis1D,
        Axis2D
    };
    /** @brief Physical or virtual source represented by an input binding. */
    enum class BindingControlKind : std::uint8_t {
        Key,
        PointerButton,
        PointerWheelX,
        PointerWheelY,
        GamepadButton,
        GamepadAxis,
        RawGamepadButton,
        RawGamepadAxis,
    };
    /** @brief Deadzone transformation applied before action aggregation. */
    enum class DeadzoneKind : std::uint8_t {
        None,
        Axial,
        Radial,
        Threshold
    };

    /** @brief Typed binding from one control, modifiers, or chord into an action component. */
    struct InputBinding {
        BindingControlKind kind{BindingControlKind::Key};
        Key key{Key::Unknown};
        PointerButton pointerButton{PointerButton::Primary};
        GamepadButton gamepadButton{GamepadButton::South};
        GamepadAxis gamepadAxis{GamepadAxis::LeftX};
        std::uint16_t rawControl{0};
        std::array<Key, 4> chord{};
        std::uint8_t chordSize{0};
        ModifierState requiredModifiers{};
        float scale{1.0F};
        std::uint8_t component{0};
        DeadzoneKind deadzoneKind{DeadzoneKind::None};
        float deadzone{0.0F};
        float digitalThreshold{0.5F};
        [[nodiscard]] friend bool operator==(const InputBinding &, const InputBinding &) noexcept = default;
    };

    /** @brief Registered semantic action contract and its engine defaults. */
    struct ActionDescriptor {
        ActionId id;
        ActionValueType valueType{ActionValueType::Digital};
        InputContextId context;
        bool required{false};
        std::vector<InputBinding> defaultBindings;
    };

    /** @brief Resolved digital, 1D, or 2D value and frame-edge semantics. */
    struct ActionValue {
        float x{0.0F};
        float y{0.0F};
        bool down{false};
        bool pressed{false};
        bool released{false};
    };

    /** @brief Complete profile-level binding replacement for one action. */
    struct BindingOverride {
        ActionId action;
        std::vector<InputBinding> bindings;
    };

    /** @brief Schema-versioned collection of per-action binding overrides. */
    struct InputBindingProfile {
        std::uint32_t schemaVersion{1};
        std::string profileId{"default"};
        std::vector<BindingOverride> overrides;
    };

    /** @brief Typed validation failure or conflict category. */
    enum class BindingDiagnosticCode : std::uint8_t {
        InvalidAction,
        DuplicateBinding,
        RequiredActionUnbound,
        UnsupportedControl,
        ReservedShortcut,
        AmbiguousChord,
        InvalidDeadzone,
        InvalidSchema,
        MalformedProfile,
        DeviceExclusivityViolation,
        AmbiguousContext,
    };

    /** @brief One actionable profile validation diagnostic. */
    struct BindingDiagnostic {
        BindingDiagnosticCode code{BindingDiagnosticCode::InvalidAction};
        ActionId action;
        std::string message;
        bool blocking{true};
    };

    /** @brief Validation result that preserves every discovered diagnostic. */
    struct BindingValidationReport {
        std::vector<BindingDiagnostic> diagnostics;
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /**
     * @brief Validates effective defaults and overrides without mutating live routing state.
     * @param actions Registered action descriptors.
     * @param profile Candidate binding profile.
     * @return All typed diagnostics; an empty report is valid.
     */
    [[nodiscard]] BindingValidationReport ValidateBindingProfile(std::span<const ActionDescriptor> actions,
                                                                 const InputBindingProfile &profile);
    /**
     * @brief Layer a higher-priority binding profile over a lower-priority profile.
     * @param lower Profile supplying bindings not mentioned by @p higher.
     * @param higher Profile whose per-action overrides take precedence.
     * @return A schema-compatible merged profile, or an error without partial application.
     */
    [[nodiscard]] Result<InputBindingProfile> MergeBindingProfiles(const InputBindingProfile &lower, const InputBindingProfile &higher);
    /** @brief Parses one complete schema-versioned profile without partial recovery. */
    [[nodiscard]] Result<InputBindingProfile> ParseBindingProfile(std::string_view json);
    /** @brief Serializes a structurally valid schema profile to deterministic JSON. */
    [[nodiscard]] Result<std::string> SerializeBindingProfile(const InputBindingProfile &profile);
    /** @brief Loads and parses a bounded profile file. */
    [[nodiscard]] Result<InputBindingProfile> LoadBindingProfile(const std::filesystem::path &path);
    /** @brief Atomically replaces a profile file after validation and serialization. */
    [[nodiscard]] Result<void> SaveBindingProfileAtomically(const std::filesystem::path &path, const InputBindingProfile &profile);

    /** @brief Deterministic reason delivered when exclusive capture is cancelled. */
    enum class CaptureCancellationReason : std::uint8_t {
        Explicit,
        Escape,
        FocusLost,
        ModalOpened,
        OwnerDestroyed,
        DeviceDisconnected,
        ContextRemoved,
    };

    /** @brief Narrow callback implemented by an interaction that owns capture. */
    class IInputCaptureOwner {
    public:
        virtual ~IInputCaptureOwner() = default;
        virtual void OnInputCaptureCancelled(CaptureCancellationReason reason) noexcept = 0;
    };

    class InputRouter;

    /** @brief Move-only RAII registration for one live routing context. */
    class InputContextToken {
    public:
        InputContextToken() = default;
        ~InputContextToken();
        InputContextToken(InputContextToken &&other) noexcept;
        InputContextToken &operator=(InputContextToken &&other) noexcept;
        InputContextToken(const InputContextToken &) = delete;
        InputContextToken &operator=(const InputContextToken &) = delete;
        void Reset() noexcept;
        [[nodiscard]] bool IsActive() const noexcept;

    private:
        friend class InputRouter;

        InputContextToken(InputRouter *router, std::uint64_t token) noexcept : router_(router), token_(token) {}

        InputRouter *router_{nullptr};
        std::uint64_t token_{0};
    };

    /** @brief Move-only RAII ownership of the router's exclusive pointer capture. */
    class PointerCaptureToken {
    public:
        PointerCaptureToken() = default;
        ~PointerCaptureToken();
        PointerCaptureToken(PointerCaptureToken &&other) noexcept;
        PointerCaptureToken &operator=(PointerCaptureToken &&other) noexcept;
        PointerCaptureToken(const PointerCaptureToken &) = delete;
        PointerCaptureToken &operator=(const PointerCaptureToken &) = delete;
        void Release() noexcept;
        [[nodiscard]] bool IsActive() const noexcept;

    private:
        friend class InputRouter;

        PointerCaptureToken(InputRouter *router, std::uint64_t token) noexcept : router_(router), token_(token) {}

        InputRouter *router_{nullptr};
        std::uint64_t token_{0};
    };

    /** @brief Resolves actions through ordered RAII contexts and owns exclusive pointer capture. */
    class InputRouter {
    public:
        InputRouter();
        ~InputRouter();
        InputRouter(const InputRouter &) = delete;
        InputRouter &operator=(const InputRouter &) = delete;

        /** @brief Installs the committed snapshot and clears per-frame consumption. */
        void BeginFrame(const RawInputSnapshot &snapshot);
        /** @brief Registers a context until the returned move-only token is destroyed. */
        [[nodiscard]] InputContextToken PushContext(InputContextId id, InputContextKind kind);
        /** @brief Acquires exclusive pointer capture for the currently eligible context. */
        [[nodiscard]] Result<PointerCaptureToken> CapturePointer(const InputContextToken &context, PointerButton button,
                                                                 IInputCaptureOwner &owner);
        /** @brief Cancels active capture synchronously and notifies its owner once. */
        void CancelCapture(CaptureCancellationReason reason) noexcept;
        /** @brief Reports whether any owner currently holds pointer capture. */
        [[nodiscard]] bool HasCapture() const noexcept;
        /** @brief Reports whether a registered context has strictly higher priority. */
        [[nodiscard]] bool HasHigherPriorityContext(InputContextKind kind) const noexcept;
        /** @brief Reports whether this token is the highest-priority, most-recent eligible context. */
        [[nodiscard]] bool IsContextActive(const InputContextToken &context) const noexcept;
        /** @brief Returns the current committed snapshot, or an empty snapshot before the first frame. */
        [[nodiscard]] const RawInputSnapshot &Snapshot() const noexcept;
        /** @brief Atomically validates and replaces action descriptors and the active profile. */
        [[nodiscard]] Result<void> SetActionMap(std::vector<ActionDescriptor> actions, InputBindingProfile profile = {});
        /** @brief Returns the registered semantic action descriptors. */
        [[nodiscard]] std::span<const ActionDescriptor> Actions() const noexcept;
        /** @brief Returns the active validated binding profile. */
        [[nodiscard]] const InputBindingProfile &Profile() const noexcept;
        /** @brief Atomically validates and applies a complete profile. */
        [[nodiscard]] Result<void> SetProfile(InputBindingProfile profile);
        /** @brief Resolves and consumes an action transition for an eligible matching context. */
        [[nodiscard]] ActionValue ReadAction(const InputContextToken &context, const ActionId &action,
                                             std::optional<PlayerId> player = std::nullopt);
        /** @brief Consumes a key press once at the eligible context. */
        [[nodiscard]] bool ConsumeKey(const InputContextToken &context, Key key);
        /** @brief Consumes a pointer-button press once at the eligible context. */
        [[nodiscard]] bool ConsumePointerButton(const InputContextToken &context, PointerButton button);
        /** @brief Assigns a currently connected generation-safe gamepad to one player. */
        [[nodiscard]] bool AssignGamepad(PlayerId player, GamepadDeviceId gamepad);
        /** @brief Removes an assignment for the exact generation-safe gamepad ID. */
        void UnassignGamepad(GamepadDeviceId gamepad) noexcept;
        /** @brief Finds the player assigned to the exact connected device generation. */
        [[nodiscard]] std::optional<PlayerId> PlayerForGamepad(GamepadDeviceId gamepad) const noexcept;

    private:
        friend class InputContextToken;
        friend class PointerCaptureToken;
        void RemoveContext(std::uint64_t token) noexcept;
        void ReleaseCapture(std::uint64_t token) noexcept;
        [[nodiscard]] bool TokenActive(std::uint64_t token) const noexcept;
        [[nodiscard]] bool CaptureActive(std::uint64_t token) const noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    /** @brief Headless-capable composition that owns collection, immutable frame commit, and semantic routing. */
    class InputService {
    public:
        void BeginFrame(FrameNumber frame);
        [[nodiscard]] RawInputCollector &Collector() noexcept;
        [[nodiscard]] InputRouter &Router() noexcept;
        [[nodiscard]] const RawInputSnapshot &CommitFrame();

    private:
        RawInputCollector collector_;
        InputRouter router_;
    };

    /** @brief Canonical per-tick gameplay command projection. */
    struct GameplayInputFrame {
        SimulationTick tick{0};
        float moveX{0.0F};
        float moveY{0.0F};
        float lookX{0.0F};
        float lookY{0.0F};
        bool jumpPressed{false};
        bool interactPressed{false};
        bool moveDown{false};
        bool movePressed{false};
        bool moveReleased{false};
        [[nodiscard]] friend bool operator==(const GameplayInputFrame &, const GameplayInputFrame &) noexcept = default;
    };

    /** @brief Captures routed actions before fixed update and emits device-independent tick commands. */
    class GameplayInputFrameBuilder {
    public:
        /** @brief Binds the semantic actions for one player; use a separate builder for each player. */
        GameplayInputFrameBuilder(ActionId move, ActionId look, ActionId jump, ActionId interact);
        /**
         * @brief Latches the ledger-filtered projection once after UI consumption and before fixed updates.
         * @param router Router holding the committed presentation snapshot.
         * @param context Gameplay context; inactive or unfocused input neutralizes held and pending commands.
         * @param player Optional player assignment owned by this builder.
         * @details Edges survive presentation frames with no fixed tick and fire on the next tick only.
         *          Call once per committed snapshot after all higher-priority input consumers have run.
         */
        void Capture(InputRouter &router, const InputContextToken &context, std::optional<PlayerId> player = std::nullopt);
        /**
         * @brief Returns a value command for one fixed tick without accessing device or router state.
         * @param tick Tick assigned by the fixed-step scheduler; successive calls must use successive ticks.
         * @return Held axes and at most one pending edge per action, stamped with @p tick.
         */
        [[nodiscard]] GameplayInputFrame Consume(SimulationTick tick) noexcept;
        /** @brief Clears pending and held commands at a session or ownership boundary. */
        void Reset() noexcept;

    private:
        ActionId move_;
        ActionId look_;
        ActionId jump_;
        ActionId interact_;
        FrameNumber capturedFrame_{0};
        bool hasCapturedFrame_{false};
        float moveX_{0.0F};
        float moveY_{0.0F};
        float lookX_{0.0F};
        float lookY_{0.0F};
        bool pendingJump_{false};
        bool pendingInteract_{false};
        bool moveDown_{false};
        bool pendingMovePressed_{false};
        bool pendingMoveReleased_{false};
    };

    /** @brief In-memory deterministic record/replay sequence of resolved gameplay frames. */
    class GameplayInputRecording {
    public:
        void Record(const GameplayInputFrame &frame);
        void ResetReplay() noexcept;
        [[nodiscard]] std::optional<GameplayInputFrame> Next();

        [[nodiscard]] std::span<const GameplayInputFrame> Frames() const noexcept {
            return frames_;
        }

    private:
        std::vector<GameplayInputFrame> frames_;
        std::size_t replayIndex_{0};
    };

    /** @brief Single-source player-to-gamepad assignment table using generation-safe IDs. */
    class InputDeviceAssignments {
    public:
        [[nodiscard]] bool Assign(PlayerId player, GamepadDeviceId gamepad);
        void Unassign(GamepadDeviceId gamepad) noexcept;
        void RetainConnected(std::span<const GamepadState> gamepads) noexcept;
        [[nodiscard]] std::optional<PlayerId> PlayerFor(GamepadDeviceId gamepad) const noexcept;

    private:
        std::vector<std::pair<GamepadDeviceId, PlayerId>> assignments_;
    };

    /** @brief Backend-neutral dual-frequency gamepad rumble request. */
    struct RumbleEffect {
        float lowFrequency{0.0F};
        float highFrequency{0.0F};
        std::uint32_t durationMilliseconds{0};
    };

    /** @brief Optional gamepad haptics capability implemented by platform adapters. */
    class IGamepadHaptics {
    public:
        virtual ~IGamepadHaptics() = default;
        [[nodiscard]] virtual Result<void> PlayRumble(GamepadDeviceId id, RumbleEffect effect) = 0;
        [[nodiscard]] virtual Result<void> Stop(GamepadDeviceId id) = 0;
    };

    /** @brief Deterministic headless gamepad that enters through RawInputCollector. */
    class VirtualGamepad {
    public:
        explicit VirtualGamepad(RawInputCollector &collector) noexcept : collector_(&collector) {}

        ~VirtualGamepad();
        VirtualGamepad(const VirtualGamepad &) = delete;
        VirtualGamepad &operator=(const VirtualGamepad &) = delete;
        VirtualGamepad(VirtualGamepad &&other) noexcept;
        VirtualGamepad &operator=(VirtualGamepad &&other) noexcept;

        [[nodiscard]] GamepadDeviceId Connect(std::string name = "Virtual Gamepad");
        void Disconnect();
        [[nodiscard]] bool Press(GamepadButton button);
        [[nodiscard]] bool Release(GamepadButton button);
        [[nodiscard]] bool SetAxis(GamepadAxis axis, float value);

        [[nodiscard]] GamepadDeviceId Id() const noexcept {
            return id_;
        }

    private:
        RawInputCollector *collector_;
        GamepadDeviceId id_{};
    };
}  // namespace Horo::Input
