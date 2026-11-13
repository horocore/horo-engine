#pragma once

/**
 * @file Input.h
 * @brief Backend-neutral input snapshots, action routing, capture, gamepads, profiles, and tick frames.
 */

#include "Horo/Foundation/Result.h"

#include <array>
#include <compare>
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
    inline constexpr std::size_t MaximumConsumedGamepadTransitions = 4'096;
    /** @brief Maximum effective binding count for an action opting into semantic accessibility mapping. */
    inline constexpr std::size_t MaximumAccessibilityBindingsPerAction = 64;
    /** @brief Maximum typed action-name byte count for an action opting into semantic accessibility mapping. */
    inline constexpr std::size_t MaximumAccessibilityActionIdBytes = 256;
    /** @brief Maximum live/retiring (context, binding, player) incarnations; consumed identities retire at an owner frame boundary. */
    inline constexpr std::size_t MaximumAccessibilityMappings = 1024;

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
        Equal,
        Minus,
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

    /** @brief Canonical non-XR sensor vocabulary; native sensor handles remain in the platform adapter. */
    enum class SensorKind : std::uint8_t {
        Accelerometer,
        Gyroscope,
        StepCounter,
        Count
    };
    /** @brief Typed publication state; unavailable evidence always has zero values and timestamp. */
    enum class SensorAvailability : std::uint8_t {
        Unsupported,
        Disabled,
        PermissionRequired,
        Suspended,
        AwaitingSample,
        NativeFailure,
        Available
    };

    /** @brief Copied SI motion sample or exact monotonically counted steps at a monotonic native timestamp. */
    struct SensorSample final {
        SensorAvailability availability{SensorAvailability::Unsupported};
        std::array<float, 3> value{};
        std::uint64_t counter{};
        std::int64_t timestampNanoseconds{};
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

    inline constexpr std::size_t MaximumTouchContacts = 16;

    /** @brief Host-issued contact incarnation; IDs cannot be reused until their release frame has drained. */
    struct TouchContactId final {
        std::uint64_t value{};
        std::uint64_t generation{};

        /** @brief Checks the non-zero source incarnation. @return Whether this contact is addressable. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0 && generation != 0;
        }

        [[nodiscard]] friend constexpr bool operator==(TouchContactId, TouchContactId) noexcept = default;
    };

    /** @brief One hoverless physical contact in the collector's window-local coordinate units, never a UI/player identity. */
    struct TouchContactState final {
        TouchContactId id;
        float x{};
        float y{};
        ButtonState contact;
        bool cancelled{}; /**< Loss/overflow differs from a release eligible to activate or drop. */
    };

    /** @brief Typed bounded collection outcome; failure never admits a partial new contact. */
    enum class TouchCollectionStatus : std::uint8_t {
        Accepted,
        Invalid,
        Stale,
        CapacityExceeded,
        Unavailable
    };

    /** @brief Physical-source interruption; never a release eligible to activate a control. */
    enum class TouchCancellationReason : std::uint8_t {
        SurfaceLost,
        MalformedSource,
        CapacityExceeded
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

    /** @brief Text and pre-edit state delivered once to the explicitly focused text context. */
    struct TextInputDelivery {
        std::string committed;
        TextCompositionState composition;
        bool compositionChanged{false}; /**< Includes cancellation and completed composition. */
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
        SensorSample gyroscope; /**< Owned SI sample for this exact device incarnation; unsupported unless explicitly collected. */
    };

    /** @brief Host-provided monotonic sample boundary for deterministic semantic hold/repeat interpretation.
     * @details The host chooses a stable real or replay time domain for a router session. Input never samples a clock implicitly.
     * Equal timestamps are permitted. Missing time is explicit; frame ordinal is never interpreted as elapsed time.
     */
    struct InputSampleTime final {
        std::uint64_t microseconds{};
        [[nodiscard]] constexpr auto operator<=>(const InputSampleTime &) const noexcept = default;
    };

    /** @brief Immutable value snapshot committed once after platform event polling. */
    struct RawInputSnapshot {
        FrameNumber frame{0};
        std::array<ButtonState, static_cast<std::size_t>(Key::Count)> keyboard{};
        PointerState pointer{};
        std::array<TouchContactState, MaximumTouchContacts> touches{}; /**< Fixed slots, including this frame's terminal contacts. */
        bool touchOverflow{}; /**< Overflow cancels all held contacts; UI must not interpret their releases as activation. */
        std::vector<GamepadState> gamepads;
        std::string text;
        TextCompositionState composition{};
        std::uint64_t compositionRevision{0}; /**< Advances on native pre-edit updates and cancellation. */
        ModifierState modifiers{};
        WindowInputState window{};

        std::array<SensorSample, static_cast<std::size_t>(SensorKind::Count)> sensors{}; /**< Owned permission-filtered sensor evidence. */
        std::optional<InputSampleTime> sampleTime; /**< Exact host sample boundary; never synthesized from physical transitions. */

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

        /** @brief Starts collection with an explicit optional host time boundary.
         * @param frame Correlation ordinal, never a duration. @param sampleTime Stable monotonic/replay time, or unavailable.
         * @details Every call replaces time metadata, including clearing an earlier timestamp when none is provided.
         */
        void BeginFrame(FrameNumber frame, std::optional<InputSampleTime> sampleTime = std::nullopt);
        void SetKey(Key key, bool down);
        void SetPointerButton(PointerButton button, bool down);
        void SetPointerPosition(float x, float y);
        void AddPointerWheel(float x, float y);
        /**
         * @brief Collects one normalized hoverless contact without allocation.
         * @param id Exact host-issued physical contact incarnation, unrelated to a UI pointer ID.
         * @param x Finite window-local X, in the same units as PointerState. @param y Finite window-local Y.
         * @param down Whether the contact is held; an unknown release is stale.
         * @return Typed admission result. Capacity exhaustion cancels every admitted contact until the next collection frame.
         * @pre BeginFrame was called; host serializes collection and burns IDs on device reconnect/reuse.
         */
        [[nodiscard]] TouchCollectionStatus SetTouchContact(TouchContactId id, float x, float y, bool down) noexcept;
        /** @brief Cancels one exact physical incarnation, retaining terminal evidence until the next frame.
         * @param id Current contact. @return Accepted, Invalid or Stale; no replacement contact is created.
         */
        [[nodiscard]] TouchCollectionStatus CancelTouchContact(TouchContactId id) noexcept;
        /** @brief Cancels all touch contacts without changing keyboard/mouse state.
         * @param reason Typed physical-source failure; CapacityExceeded closes collection for the current frame.
         */
        void CancelTouchContacts(TouchCancellationReason reason) noexcept;
        void AppendText(std::string_view utf8);
        void SetTextComposition(std::string_view utf8, std::int32_t selectionStart, std::int32_t selectionLength);
        /** @brief Replace one finite monotonic sensor sample; invalid data never changes the last valid sample.
         * @param sensor Canonical sensor kind. @param sample Owned value; unavailable values are forcibly zeroed.
         * @return False for invalid kind, nonfinite data, nonpositive or regressing available timestamp.
         */
        [[nodiscard]] bool SetSensorSample(SensorKind sensor, SensorSample sample) noexcept;
        void SetModifiers(ModifierState modifiers) noexcept;
        void SetWindowState(WindowInputState state) noexcept;
        /** @brief Release all held controls and zero axes after focus or device ownership is lost. */
        void Neutralize() noexcept;
        [[nodiscard]] GamepadDeviceId ConnectGamepad(std::string name, bool canonicalMapping = true, std::size_t rawButtonCount = 0,
                                                     std::size_t rawAxisCount = 0);
        [[nodiscard]] bool DisconnectGamepad(GamepadDeviceId id);
        [[nodiscard]] bool SetGamepadButton(GamepadDeviceId id, GamepadButton button, bool down);
        [[nodiscard]] bool SetGamepadAxis(GamepadDeviceId id, GamepadAxis axis, float value);
        /** @brief Publishes copied SI gyro evidence for one exact connected device incarnation without allocation.
         * @param id Current device handle. @param sample Available radians/second or an explicit unavailable state.
         * @return False without mutation for stale devices, invalid samples or available data while unfocused.
         * Available timestamps must be positive and nonregressing; unavailable data is zeroed and resets the timestamp domain.
         * @pre Host serializes collection and explicitly owns native sensor enablement.
         */
        [[nodiscard]] bool SetGamepadGyroscopeSample(GamepadDeviceId id, SensorSample sample) noexcept;
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
        GyroscopeAxis, /**< Explicit platform or assigned-controller motion source. */
    };
    /** @brief Deadzone transformation applied before action aggregation. */
    enum class DeadzoneKind : std::uint8_t {
        None,
        Axial,
        Radial,
        Threshold
    };

    /** @brief Deterministic mapping policy; durations use the owner's monotonic microsecond clock. */
    struct DigitalAffordancePolicy final {
        bool toggle{};
        std::uint64_t holdMicroseconds{};
        bool repeat{};
        std::uint64_t repeatDelayMicroseconds{400000};
        std::uint64_t repeatIntervalMicroseconds{100000};
        /** @brief Checks bounded hold/delay and positive bounded repeat interval. @return True when admitted. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool operator==(const DigitalAffordancePolicy &) const noexcept = default;
    };

    /** @brief Inert per-binding semantic accessibility policy; never rewrites raw input facts. */
    struct BindingAccessibilityPolicy final {
        DigitalAffordancePolicy digital;
        bool stickyModifiers{};
        /** @brief Checks the bounded digital policy. @return Whether all policy fields are admitted. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Reports whether semantic interpretation is requested. @return True for toggle, hold, repeat or sticky modifiers. */
        [[nodiscard]] bool IsEnabled() const noexcept;
        [[nodiscard]] bool operator==(const BindingAccessibilityPolicy &) const noexcept = default;
    };

    /** @brief Native device-coordinate component; no camera/screen orientation is inferred. */
    enum class SensorAxis : std::uint8_t {
        X,
        Y,
        Z,
        Count
    };

    /** @brief Physical motion source; unavailable sources never fall back to another sensor. */
    enum class GyroscopeSource : std::uint8_t {
        Platform,
        AssignedGamepad,
        Count
    };

    /** @brief Gyroscope normalization before the ordinary binding deadzone, scale and component projection.
     * @details The platform supplies radians/second in its documented device coordinates. A magnitude equal
     * to radiansPerSecondAtFullScale produces a unit axis; scale supplies sensitivity/inversion. This is a
     * rate control like a stick, never an integrated angle or frame-dependent mouse delta. The gameplay
     * owner applies its normal tick duration. Unavailable samples contribute zero without enabling sensors.
     */
    struct GyroscopeBindingPolicy final {
        SensorAxis axis{SensorAxis::X};
        float radiansPerSecondAtFullScale{1.0F};
        GyroscopeSource source{GyroscopeSource::Platform}; /**< AssignedGamepad uses the ordinary exact player/device filter. */
        /** @brief Checks the source, axis and finite inclusive [0.001, 1000] rad/s calibration. @return Whether admitted. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] bool operator==(const GyroscopeBindingPolicy &) const noexcept = default;
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
        float digitalThreshold{
            0.5F}; /**< Nonzero filtered axes at/above this magnitude are held; zero remains neutral even at threshold zero. */
        BindingAccessibilityPolicy accessibility; /**< Digital-action semantic interpretation, persisted by the canonical profile codec. */
        GyroscopeBindingPolicy gyroscope;         /**< Used only by GyroscopeAxis; other controls retain its neutral default. */
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
        bool repeated{false}; /**< Semantic mapping repeat, separate from the activation press edge. */
    };

    /** @brief Presentation modality derived from eligible semantic input, never device assignment. */
    enum class InputModality : std::uint8_t {
        Unknown,
        KeyboardMouse,
        Gamepad,
        Touch,
        Pen,
        Accessibility,
        Motion /**< Platform motion sensor, without implied gamepad or player assignment. */
    };

    /** @brief Stable canonical control identity; glyph packages may realize it without owning input. */
    struct InputGlyphId {
        BindingControlKind kind{BindingControlKind::Key};
        std::uint16_t control{};
        [[nodiscard]] friend constexpr bool operator==(InputGlyphId, InputGlyphId) noexcept = default;
    };

    /** @brief Copied canonical binding and exact device generation supplying a routed action. */
    struct ActionSource {
        InputModality modality{InputModality::Unknown};
        InputGlyphId glyph;
        std::optional<GamepadDeviceId> gamepad;
        [[nodiscard]] friend constexpr bool operator==(const ActionSource &, const ActionSource &) noexcept = default;
    };

    /** @brief Typed action resolution outcome, including atomic ledger admission refusal. */
    enum class ActionReadStatus : std::uint8_t {
        Resolved,
        Unavailable,
        CapacityExceeded,
        ClockUnavailable /**< A timed mapping has no explicit host sample clock; its old active state is withdrawn. */
    };

    /** @brief Semantic value with post-filter, routing-eligible modality evidence. Releases are never meaningful. */
    struct ActionEvidence {
        ActionValue value;
        std::optional<ActionSource> source;
        bool meaningful{};
        ActionReadStatus status{ActionReadStatus::Unavailable};
    };

    /** @brief Owned semantic withdrawal emitted when a context ends before its action reader can drain release.
     * @details The immutable typed name lease survives profile/map replacement and router destruction. The record names
     * only its original context/player; it never authorizes delivery to a replacement context or invokes a dead owner.
     */
    struct RetiredInputActionRelease final {
        std::uint64_t contextIdentity{};
        std::shared_ptr<const ActionId> action;
        std::optional<PlayerId> player;
    };

    /** @brief Glyph realization availability, independent of navigation capability. */
    enum class InputGlyphSupport : std::uint8_t {
        CanonicalLabel,
        Unsupported
    };

    /** @brief Borrowed static canonical fallback; no icon package or native API is required. */
    struct InputGlyphPresentation {
        InputGlyphId id;
        InputGlyphSupport support{InputGlyphSupport::Unsupported};
        std::string_view label;
    };

    /** @brief Resolves a typed binding to a stable canonical source. @param binding Effective binding. @param device Exact gamepad when
     * known. @return Copied source; raw controls have Unknown modality. */
    [[nodiscard]] ActionSource CanonicalActionSource(const InputBinding &binding, std::optional<GamepadDeviceId> device = {}) noexcept;
    /** @brief Resolves a glyph without allocation or device-name checks. @param id Stable canonical control. @return Static label or typed
     * Unsupported. */
    [[nodiscard]] InputGlyphPresentation CanonicalGlyph(InputGlyphId id) noexcept;

    /** @brief Complete profile-level binding replacement for one action. */
    struct BindingOverride {
        ActionId action;
        std::vector<InputBinding> bindings;
    };

    /** @brief Single canonical profile wire/in-memory version; legacy version one is upgraded only by ParseBindingProfile. */
    inline constexpr std::uint32_t CurrentInputBindingProfileSchema = 2;

    /** @brief Schema-versioned collection of per-action binding overrides. */
    struct InputBindingProfile {
        std::uint32_t schemaVersion{CurrentInputBindingProfileSchema};
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
        InvalidAccessibility,
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
    /** @brief Parses one complete profile and returns the single canonical version-two model without partial recovery.
     * @param json Bounded complete JSON document; legacy version one must not contain accessibility or gyroscope policy fields.
     * @return Canonical profile or typed malformed/version failure. No file is rewritten and no router state changes.
     * @details Legacy neutral bindings migrate on read; new accessibility fields require exact booleans and bounded integer
     * microseconds. Floating versions, unknown policy fields and unsupported versions are rejected before numeric narrowing.
     * Gyroscope controls require a typed axis and bounded SI calibration; control kinds never wrap through a byte-sized enum.
     * @throws std::bad_alloc When owned parse/model storage cannot be allocated.
     */
    [[nodiscard]] Result<InputBindingProfile> ParseBindingProfile(std::string_view json);
    /** @brief Serializes a canonical profile and its typed accessibility policies to deterministic version-two JSON.
     * @param profile Canonical in-memory version and domain-valid binding policies.
     * @return Complete JSON or typed schema/policy failure; legacy in-memory versions are not silently reinterpreted.
     * @details Call ValidateBindingProfile with the real action map before live installation; serialization does not own that map.
     * @throws std::bad_alloc When output/model storage cannot be allocated.
     */
    [[nodiscard]] Result<std::string> SerializeBindingProfile(const InputBindingProfile &profile);
    /** @brief Loads and parses a bounded profile file. */
    [[nodiscard]] Result<InputBindingProfile> LoadBindingProfile(const std::filesystem::path &path);
    /** @brief Atomically replaces a profile file after validation and serialization. */
    [[nodiscard]] Result<void> SaveBindingProfileAtomically(const std::filesystem::path &path, const InputBindingProfile &profile);

    /** @brief Deterministic reason delivered when exclusive capture is cancelled. */
    enum class CaptureCancellationReason : std::uint8_t {
        Explicit,
        Released,
        Escape,
        FocusLost,
        ModalOpened,
        ContextPreempted,
        OwnerDestroyed,
        DeviceDisconnected,
        ContextRemoved,
    };

    class InputRouter;
    class InputActionCollection;
    class InputCollectionFrame;
    class InputActionSourceToken;
    struct InputCollectionLifetime;

    /** @brief Narrow callback implemented by an interaction that owns capture. */
    class IInputCaptureOwner {
    public:
        IInputCaptureOwner() = default;
        virtual ~IInputCaptureOwner();
        IInputCaptureOwner(const IInputCaptureOwner &) = delete;
        IInputCaptureOwner &operator=(const IInputCaptureOwner &) = delete;
        IInputCaptureOwner(IInputCaptureOwner &&) = delete;
        IInputCaptureOwner &operator=(IInputCaptureOwner &&) = delete;
        virtual void OnInputCaptureCancelled(CaptureCancellationReason reason) noexcept = 0;

    private:
        friend class InputRouter;
        InputRouter *capturingRouter_{nullptr};
    };

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

    /** @brief Copied routing generation and bounded-scan evidence for one exact context. */
    struct InputRoutingState final {
        std::uint64_t contextIdentity{};       /**< Zero for foreign or removed tokens. */
        std::uint64_t configurationRevision{}; /**< Non-wrapping; zero after exhaustion. */
        std::uint64_t assignmentRevision{};    /**< Non-wrapping; zero after exhaustion. */
        std::size_t contexts{};
        std::size_t gamepads{};
        std::size_t previousGamepads{};

        /** @brief Checks finite scan bounds. @param maximumContexts Live context bound. @param maximumGamepads Current/previous device
         * bound. @return Whether routing fits both bounds. */
        [[nodiscard]] bool WithinLimits(const std::size_t maximumContexts, const std::size_t maximumGamepads) const noexcept {
            return contexts <= maximumContexts && gamepads <= maximumGamepads && previousGamepads <= maximumGamepads;
        }
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
        /** @brief Borrows the canonical collection port for a host-owned context and optional existing device assignment.
         * @param context Exact live context; registration cannot name another context.
         * @param assignedDevice Optional currently connected gamepad whose existing player assignment is copied, never changed.
         * @return Assignment-bound port or typed inactive/invalid failure. An empty device binds unfiltered host input.
         * @pre Query on the router's owner thread; const access preserves the existing lifetime/context fences.
         * @post Returns a weak port without registering a source, changing assignments or consuming input.
         * No XR/native dependency or alternate commit owner is introduced. */
        [[nodiscard]] Result<InputActionCollection> ActionCollection(const InputContextToken &context,
                                                                     std::optional<GamepadDeviceId> assignedDevice = std::nullopt) const;
        /** @brief Cancels released pointer capture and reclaims drained semantic mapping identities.
         * @details Pending old-owner releases remain until delivered. The next committed sample also performs this cleanup.
         */
        void EndFrame() noexcept;
        /** @brief Registers a context until the returned move-only token is destroyed.
         * @param id Host-owned context category. @param kind Routing priority and interaction role.
         * @return New non-reusable context incarnation, or an empty token if routing identities are exhausted.
         * @details Preempted semantic mappings withdraw to their original owner before a replacement can read.
         */
        [[nodiscard]] InputContextToken PushContext(InputContextId id, InputContextKind kind);
        /** @brief Acquires exclusive pointer capture for the currently eligible context.
         * @param context Exact eligible live owner. @param button Initiating pointer button.
         * @param owner Capture callback owner, which must obey its existing lifetime contract.
         * @return Exclusive capture or a typed inactive, busy or routing-identity-exhausted error.
         */
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
        /**
         * @brief Grants text focus to one eligible GUI, modal, or native-dialog context.
         * @param context Exact live context owning the focused text surface.
         * @return False for inactive or non-text contexts; otherwise true. A focus transfer discards
         *         already-collected text and pre-edit for the current frame.
         */
        [[nodiscard]] bool FocusText(const InputContextToken &context) noexcept;
        /** @brief Releases text focus only if @p context still owns it. */
        void BlurText(const InputContextToken &context) noexcept;
        /**
         * @brief Takes this frame's committed text and current pre-edit at most once for the focus owner.
         * @param context Exact context previously passed to FocusText.
         * @return No delivery for an inactive, unfocused, preempted, or already-served context.
         *         Pre-edit from an earlier focus owner is never transferred.
         */
        [[nodiscard]] std::optional<TextInputDelivery> TakeText(const InputContextToken &context);
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
        /**
         * @brief Resolves and consumes the same action as ReadAction, including canonical source evidence.
         * @param context Exact live matching context.
         * @param action Registered action identity, or an old identity awaiting withdrawal after configuration replacement.
         * @param player Optional exact gamepad assignment filter.
         * @return Neutral evidence when blocked/missing. CapacityExceeded atomically refuses this action's transitions;
         *         previously admitted consumers are preserved. ClockUnavailable refuses timed mapping without explicit sample time.
         *         Meaningful evidence comes only from unconsumed press edges.
         * @details Call this or ReadAction once per action per frame. Exact simultaneous sources prefer keyboard/mouse,
         *          then gamepad in snapshot order. Accessibility activation/release/repeat edges are delivered once per committed
         *          sample; subsequent reads preserve held state. Repeat is distinct from pressed. A still-live preempted owner may
         *          drain its pending cancellation release even while routing is blocked. Missing-clock reads can carry that release.
         *          New mappings require neutral physical input before a fresh press. This does not poll a backend or change assignment.
         */
        [[nodiscard]] ActionEvidence ReadActionEvidence(const InputContextToken &context, const ActionId &action,
                                                        std::optional<PlayerId> player = std::nullopt);
        /** @brief Returns the last ReadAction/ReadActionEvidence outcome for legacy value-only consumers. @return Typed
         * unavailable/admission result. */
        [[nodiscard]] ActionReadStatus LastActionStatus() const noexcept;
        /** @brief Drains one bounded old-context withdrawal without allocation or invoking gameplay/UI callbacks.
         * @param release Output written only on success; its immutable action-name lease may outlive this router.
         * @return True for one retired-owner release, false leaves output unchanged.
         * @details The host drains at its ordinary input/owner cutoff. Live but preempted owners consume their own release
         * through ReadActionEvidence instead. Undrained retired withdrawals retain a fixed mapping slot; exhaustion is explicit.
         */
        [[nodiscard]] bool TryDequeueRetiredActionRelease(RetiredInputActionRelease &release) noexcept;
        /** @brief Copies context identity, configuration/assignment generations and current/previous scan sizes.
         * @param context Borrowed token; a foreign/removed token yields zero identity.
         * @return Routing evidence without retaining a snapshot or context borrow. */
        [[nodiscard]] InputRoutingState RoutingState(const InputContextToken &context) const noexcept;
        /** @brief Checks an action-map context against an exact live token, including suspended tokens. @param context Live token. @param
         * id Descriptor context identity. @return Whether both name the same Input-owned context. */
        [[nodiscard]] bool ContextMatches(const InputContextToken &context, const InputContextId &id) const noexcept;
        /** @brief Consumes a key press once at the eligible context. */
        [[nodiscard]] bool ConsumeKey(const InputContextToken &context, Key key);
        /** @brief Consumes a pointer-button press once at the eligible context. */
        [[nodiscard]] bool ConsumePointerButton(const InputContextToken &context, PointerButton button);
        /** @brief Consumes an exact contact press once at the eligible context.
         * @param context Exact live context. @param contact Physical source incarnation from this committed frame.
         * @return Whether this context admitted the press; held/release delivery stays with the owner that admitted it.
         */
        [[nodiscard]] bool ConsumeTouchContact(const InputContextToken &context, TouchContactId contact);
        /** @brief Claims one mouse-position/hover delivery for this frame's highest-priority context.
         * @param context Exact live routing token. @return Whether this frame's position was admitted once.
         * @note Surface-exit positions are admitted so the prior hover target can be neutralized.
         */
        [[nodiscard]] bool ConsumePointerMotion(const InputContextToken &context);
        /** @brief Assigns a currently connected generation-safe gamepad to one player. */
        [[nodiscard]] bool AssignGamepad(PlayerId player, GamepadDeviceId gamepad);
        /** @brief Removes an assignment for the exact generation-safe gamepad ID. */
        void UnassignGamepad(GamepadDeviceId gamepad) noexcept;
        /** @brief Finds the player assigned to the exact connected device generation. */
        [[nodiscard]] std::optional<PlayerId> PlayerForGamepad(GamepadDeviceId gamepad) const noexcept;

    private:
        friend class InputActionCollection;
        friend class InputCollectionFrame;
        friend class InputActionSourceToken;
        friend struct InputCollectionLifetime;
        void CommitRawFrame(const RawInputSnapshot &snapshot);
        friend class IInputCaptureOwner;
        friend class InputContextToken;
        friend class PointerCaptureToken;
        void RemoveContext(std::uint64_t token) noexcept;
        void ReleaseCapture(std::uint64_t token) noexcept;
        void OnCaptureOwnerDestroyed(const IInputCaptureOwner *owner) noexcept;
        [[nodiscard]] bool TokenActive(std::uint64_t token) const noexcept;
        [[nodiscard]] bool CaptureActive(std::uint64_t token) const noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };

    /** @brief Headless-capable composition that owns collection, immutable frame commit, and semantic routing. */
    class InputService {
    public:
        /** @brief Starts the canonical collection path with host-owned semantic timing.
         * @param frame Correlation ordinal. @param sampleTime Explicit time carried unchanged into the committed immutable snapshot.
         */
        void BeginFrame(FrameNumber frame, std::optional<InputSampleTime> sampleTime = std::nullopt);
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
         *          Jump and interact accept activation and configured semantic repeat edges. Multiple
         *          pending edges coalesce into one command; catch-up ticks do not repeat that command.
         *          A missing timed sample clock or mapping capacity failure clears the complete pending
         *          projection. LastCaptureStatus preserves that failure until a new snapshot is captured.
         *          Call once per committed snapshot after all higher-priority input consumers have run.
         */
        void Capture(InputRouter &router, const InputContextToken &context, std::optional<PlayerId> player = std::nullopt);
        /** @brief Returns the last committed capture outcome; repeated captures of the same sample preserve it.
         * @return Resolved after an eligible capture, Unavailable after reset/focus/context loss, or the original
         * ClockUnavailable/CapacityExceeded failure. Optional absent actions remain neutral in an eligible capture.
         */
        [[nodiscard]] ActionReadStatus LastCaptureStatus() const noexcept;
        /**
         * @brief Returns a value command for one fixed tick without accessing device or router state.
         * @param tick Tick assigned by the fixed-step scheduler; successive calls must use successive ticks.
         * @return Held axes and at most one pending edge per action, stamped with @p tick.
         */
        [[nodiscard]] GameplayInputFrame Consume(SimulationTick tick) noexcept;
        /** @brief Clears pending and held commands at a session or ownership boundary. */
        void Reset() noexcept;

    private:
        /** @brief Clears held and pending values without reopening capture of an already observed snapshot. */
        void ClearCapturedInput() noexcept;
        ActionId move_;
        ActionId look_;
        ActionId jump_;
        ActionId interact_;
        FrameNumber capturedFrame_{0};
        bool hasCapturedFrame_{false};
        ActionReadStatus lastCaptureStatus_{ActionReadStatus::Unavailable};
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
        /** @brief Publishes gyro facts through the ordinary device-owned collector path.
         * @param sample Copied SI evidence. @return Whether the current connected incarnation admitted it.
         */
        [[nodiscard]] bool SetGyroscopeSample(SensorSample sample) noexcept;

        [[nodiscard]] GamepadDeviceId Id() const noexcept {
            return id_;
        }

    private:
        RawInputCollector *collector_;
        GamepadDeviceId id_{};
    };
}  // namespace Horo::Input
