#include "SdlInputBackend.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <unordered_map>

namespace Horo::Input {
    namespace {
        /** @brief Returns the exact SDL window for a window-local input event, or no ID for global device events. */
        std::optional<SDL_WindowID> EventWindowId(const SDL_Event &event) noexcept {
            switch (event.type) {
                case SDL_EVENT_KEY_DOWN:
                case SDL_EVENT_KEY_UP:
                    return event.key.windowID;
                case SDL_EVENT_TEXT_INPUT:
                    return event.text.windowID;
                case SDL_EVENT_TEXT_EDITING:
                    return event.edit.windowID;
                case SDL_EVENT_MOUSE_MOTION:
                    return event.motion.windowID;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    return event.button.windowID;
                case SDL_EVENT_MOUSE_WHEEL:
                    return event.wheel.windowID;
                case SDL_EVENT_FINGER_DOWN:
                case SDL_EVENT_FINGER_UP:
                case SDL_EVENT_FINGER_MOTION:
                case SDL_EVENT_FINGER_CANCELED:
                    return event.tfinger.windowID;
                case SDL_EVENT_WINDOW_FOCUS_GAINED:
                case SDL_EVENT_WINDOW_FOCUS_LOST:
                case SDL_EVENT_WINDOW_MOUSE_ENTER:
                case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                case SDL_EVENT_WINDOW_RESIZED:
                    return event.window.windowID;
                default:
                    return std::nullopt;
            }
        }

        const ErrorDomainId SdlInputDomain{"horo.input.sdl"};
        const ErrorCodeDescriptor HapticsStaleDevice{SdlInputDomain,
                                                     ErrorCode{"input.haptics.stale_device"},
                                                     ErrorSeverity::Error,
                                                     "Gamepad is unavailable.",
                                                     "Reconnect the gamepad before requesting haptics.",
                                                     true,
                                                     true};
        const ErrorCodeDescriptor HapticsUnsupported{SdlInputDomain,
                                                     ErrorCode{"input.haptics.unsupported"},
                                                     ErrorSeverity::Error,
                                                     "Gamepad haptics are unsupported.",
                                                     "Use a device with haptics support.",
                                                     false,
                                                     true};
        const ErrorCodeDescriptor HapticsFailed{SdlInputDomain,
                                                ErrorCode{"input.haptics.failed"},
                                                ErrorSeverity::Error,
                                                "Gamepad haptics could not start.",
                                                "Verify the device connection and retry.",
                                                true,
                                                true};

        Key MapKey(const SDL_Scancode key) noexcept {
            using enum Key;
            if (key >= SDL_SCANCODE_A && key <= SDL_SCANCODE_Z)
                return static_cast<Key>(static_cast<int>(Key::A) + key - SDL_SCANCODE_A);
            if (key >= SDL_SCANCODE_1 && key <= SDL_SCANCODE_9)
                return static_cast<Key>(static_cast<int>(Key::Digit1) + key - SDL_SCANCODE_1);
            switch (key) {
                case SDL_SCANCODE_0:
                    return Digit0;
                case SDL_SCANCODE_ESCAPE:
                    return Escape;
                case SDL_SCANCODE_RETURN:
                    return Enter;
                case SDL_SCANCODE_TAB:
                    return Tab;
                case SDL_SCANCODE_SPACE:
                    return Space;
                case SDL_SCANCODE_BACKSPACE:
                    return Backspace;
                case SDL_SCANCODE_DELETE:
                    return Delete;
                case SDL_SCANCODE_LEFT:
                    return Left;
                case SDL_SCANCODE_RIGHT:
                    return Right;
                case SDL_SCANCODE_UP:
                    return Up;
                case SDL_SCANCODE_DOWN:
                    return Down;
                case SDL_SCANCODE_HOME:
                    return Home;
                case SDL_SCANCODE_END:
                    return End;
                case SDL_SCANCODE_PAGEUP:
                    return PageUp;
                case SDL_SCANCODE_PAGEDOWN:
                    return PageDown;
                case SDL_SCANCODE_F1:
                    return F1;
                case SDL_SCANCODE_F2:
                    return F2;
                case SDL_SCANCODE_F3:
                    return F3;
                case SDL_SCANCODE_F4:
                    return F4;
                case SDL_SCANCODE_F5:
                    return F5;
                case SDL_SCANCODE_F6:
                    return F6;
                case SDL_SCANCODE_F7:
                    return F7;
                case SDL_SCANCODE_F8:
                    return F8;
                case SDL_SCANCODE_F9:
                    return F9;
                case SDL_SCANCODE_F10:
                    return F10;
                case SDL_SCANCODE_F11:
                    return F11;
                case SDL_SCANCODE_F12:
                    return F12;
                default:
                    return Unknown;
            }
        }

        std::optional<PointerButton> MapPointerButton(const std::uint8_t button) noexcept {
            using enum PointerButton;
            switch (button) {
                case SDL_BUTTON_LEFT:
                    return Primary;
                case SDL_BUTTON_RIGHT:
                    return Secondary;
                case SDL_BUTTON_MIDDLE:
                    return Middle;
                case SDL_BUTTON_X1:
                    return Auxiliary1;
                case SDL_BUTTON_X2:
                    return Auxiliary2;
                default:
                    return std::nullopt;
            }
        }

        std::optional<GamepadButton> MapGamepadButton(const std::uint8_t button) noexcept {
            using enum GamepadButton;
            switch (static_cast<SDL_GamepadButton>(button)) {
                case SDL_GAMEPAD_BUTTON_SOUTH:
                    return South;
                case SDL_GAMEPAD_BUTTON_EAST:
                    return East;
                case SDL_GAMEPAD_BUTTON_WEST:
                    return West;
                case SDL_GAMEPAD_BUTTON_NORTH:
                    return North;
                case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
                    return LeftShoulder;
                case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
                    return RightShoulder;
                case SDL_GAMEPAD_BUTTON_LEFT_STICK:
                    return LeftStick;
                case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
                    return RightStick;
                case SDL_GAMEPAD_BUTTON_START:
                    return Start;
                case SDL_GAMEPAD_BUTTON_BACK:
                    return Select;
                case SDL_GAMEPAD_BUTTON_DPAD_UP:
                    return DPadUp;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                    return DPadDown;
                case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
                    return DPadLeft;
                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
                    return DPadRight;
                default:
                    return std::nullopt;
            }
        }

        std::optional<GamepadAxis> MapGamepadAxis(const std::uint8_t axis) noexcept {
            using enum GamepadAxis;
            switch (static_cast<SDL_GamepadAxis>(axis)) {
                case SDL_GAMEPAD_AXIS_LEFTX:
                    return LeftX;
                case SDL_GAMEPAD_AXIS_LEFTY:
                    return LeftY;
                case SDL_GAMEPAD_AXIS_RIGHTX:
                    return RightX;
                case SDL_GAMEPAD_AXIS_RIGHTY:
                    return RightY;
                case SDL_GAMEPAD_AXIS_LEFT_TRIGGER:
                    return LeftTrigger;
                case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER:
                    return RightTrigger;
                default:
                    return std::nullopt;
            }
        }

        Error SdlError(const ErrorCodeDescriptor &descriptor, const char *fallback) {
            const char *native = SDL_GetError();
            return MakeError(descriptor, native != nullptr && *native != '\0' ? native : fallback);
        }
    }  // namespace

    struct SdlInputBackend::Impl {
        struct Device {
            GamepadDeviceId id;
            SDL_Gamepad *gamepad{nullptr};
            SDL_Joystick *joystick{nullptr};
        };

        /** @brief Window-local native identity mapping; terminal slots cannot be reused within a collection frame. */
        struct TouchSlot final {
            SDL_TouchID device{};
            SDL_FingerID finger{};
            TouchContactId id;
            std::uint64_t timestamp{};
            bool terminal{};
        };

        RawInputCollector collector;
        std::unordered_map<SDL_JoystickID, Device> devices;
        WindowInputState windowState{};
        SDL_WindowID windowId{0};
        bool neutralizeOnBeginFrame{false};
        std::array<TouchSlot, MaximumTouchContacts> touches{};
        std::uint64_t nextTouchGeneration{1};
        std::uint64_t lastTouchTimestamp{};
        int pointerWidth{};
        int pointerHeight{};
        bool touchAdmissionClosed{};

        void HandleKeyboardEvent(const SDL_Event &event);
        void HandlePointerEvent(const SDL_Event &event);
        void HandleWindowEvent(const SDL_Event &event);
        void HandleGamepadEvent(const SDL_Event &event);
        void HandleJoystickEvent(const SDL_Event &event);
        void HandleTouchEvent(const SDL_TouchFingerEvent &event);
        void CancelTouches(TouchCancellationReason reason) noexcept;
    };

    /** @brief Closes the current native touch batch; later motion cannot revive cancelled slots. */
    void SdlInputBackend::Impl::CancelTouches(const TouchCancellationReason reason) noexcept {
        collector.CancelTouchContacts(reason);
        for (auto &touch : touches)
            touch.terminal = touch.id.IsValid();
        touchAdmissionClosed = true;
    }

    /** @brief Converts only exact-window native contacts into bounded owned physical incarnations. */
    void SdlInputBackend::Impl::HandleTouchEvent(const SDL_TouchFingerEvent &event) {
        if (touchAdmissionClosed || !windowState.focused || pointerWidth <= 0 || pointerHeight <= 0 || event.touchID == SDL_MOUSE_TOUCHID)
            return;
        if (!std::isfinite(event.x) || !std::isfinite(event.y) || event.touchID == 0 || event.timestamp < lastTouchTimestamp) {
            CancelTouches(TouchCancellationReason::MalformedSource);
            return;
        }
        lastTouchTimestamp = event.timestamp;
        auto found = std::ranges::find_if(touches, [&event](const TouchSlot &entry) {
            return entry.id.IsValid() && entry.device == event.touchID && entry.finger == event.fingerID;
        });
        if (event.type == SDL_EVENT_FINGER_DOWN) {
            if (found != touches.end()) {
                CancelTouches(TouchCancellationReason::MalformedSource);
                return;
            }
            found = std::ranges::find_if(touches, [](const TouchSlot &entry) {
                return !entry.id.IsValid();
            });
            if (found == touches.end() || nextTouchGeneration == std::numeric_limits<std::uint64_t>::max()) {
                CancelTouches(TouchCancellationReason::CapacityExceeded);
                return;
            }
            const auto slot = static_cast<std::uint64_t>(found - touches.begin()) + 1;
            *found = {event.touchID, event.fingerID, {slot, nextTouchGeneration++}, event.timestamp, false};
        } else if (found == touches.end() || found->terminal)
            return;  // Stale motion/release never manufactures a new contact.
        if (event.timestamp < found->timestamp) {
            CancelTouches(TouchCancellationReason::MalformedSource);
            return;
        }
        found->timestamp = event.timestamp;
        // SDL normalized coordinates are deliberately not clamped: captured fingers may leave the window.
        const auto collected =
            collector.SetTouchContact(found->id, event.x * pointerWidth, event.y * pointerHeight, event.type != SDL_EVENT_FINGER_UP);
        if (collected != TouchCollectionStatus::Accepted) {
            CancelTouches(collected == TouchCollectionStatus::CapacityExceeded ? TouchCancellationReason::CapacityExceeded
                                                                               : TouchCancellationReason::MalformedSource);
            return;
        }
        if (event.type == SDL_EVENT_FINGER_CANCELED)
            (void)collector.CancelTouchContact(found->id);
        found->terminal = event.type == SDL_EVENT_FINGER_UP || event.type == SDL_EVENT_FINGER_CANCELED;
    }

    /** @brief Applies keyboard, text-input, and text-composition events to the collector. */
    void SdlInputBackend::Impl::HandleKeyboardEvent(const SDL_Event &event) {
        switch (event.type) {
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                if (!event.key.repeat)
                    collector.SetKey(MapKey(event.key.scancode), event.key.down);
                collector.SetModifiers(ModifierState{
                    .control = (event.key.mod & SDL_KMOD_CTRL) != 0,
                    .shift = (event.key.mod & SDL_KMOD_SHIFT) != 0,
                    .alt = (event.key.mod & SDL_KMOD_ALT) != 0,
                    .command = (event.key.mod & SDL_KMOD_GUI) != 0,
                });
                break;
            case SDL_EVENT_TEXT_INPUT:
                if (event.text.text)
                    collector.AppendText(event.text.text);
                break;
            case SDL_EVENT_TEXT_EDITING:
                collector.SetTextComposition(event.edit.text != nullptr ? event.edit.text : "", event.edit.start, event.edit.length);
                break;
            default:
                break;
        }
    }

    /** @brief Applies pointer motion, buttons, wheel, and device presence events to the collector. */
    void SdlInputBackend::Impl::HandlePointerEvent(const SDL_Event &event) {
        switch (event.type) {
            case SDL_EVENT_MOUSE_MOTION:
                if (event.motion.which == SDL_TOUCH_MOUSEID)
                    break;
                collector.SetPointerPosition(event.motion.x, event.motion.y);
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                if (event.button.which == SDL_TOUCH_MOUSEID)
                    break;
                if (const auto button = MapPointerButton(event.button.button))
                    collector.SetPointerButton(*button, event.button.down);
                collector.SetPointerPosition(event.button.x, event.button.y);
                break;
            case SDL_EVENT_MOUSE_WHEEL:
                if (event.wheel.which == SDL_TOUCH_MOUSEID)
                    break;
                collector.AddPointerWheel(event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.x : event.wheel.x,
                                          event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y);
                break;
            case SDL_EVENT_MOUSE_ADDED:
                windowState.pointerDeviceAvailable = true;
                collector.SetWindowState(windowState);
                break;
            case SDL_EVENT_MOUSE_REMOVED:
                // The public snapshot intentionally does not expose SDL mouse IDs. A
                // removal conservatively invalidates the active pointer capture; a
                // subsequent add event restores pointer availability.
                windowState.pointerDeviceAvailable = false;
                collector.SetWindowState(windowState);
                break;
            default:
                break;
        }
    }

    /** @brief Applies window focus and hover events to the collector. */
    void SdlInputBackend::Impl::HandleWindowEvent(const SDL_Event &event) {
        switch (event.type) {
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
                windowState.focused = true;
                collector.SetWindowState(windowState);
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                collector.Neutralize();
                CancelTouches(TouchCancellationReason::SurfaceLost);
                windowState.focused = false;
                collector.SetWindowState(windowState);
                break;
            case SDL_EVENT_WINDOW_MOUSE_ENTER:
                windowState.pointerInside = true;
                collector.SetWindowState(windowState);
                break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
                windowState.pointerInside = false;
                collector.SetWindowState(windowState);
                break;
            case SDL_EVENT_WINDOW_RESIZED:
                pointerWidth = event.window.data1;
                pointerHeight = event.window.data2;
                CancelTouches(TouchCancellationReason::SurfaceLost);
                break;
            default:
                break;
        }
    }

    /** @brief Opens and closes gamepads and forwards their button and axis state. */
    void SdlInputBackend::Impl::HandleGamepadEvent(const SDL_Event &event) {
        switch (event.type) {
            case SDL_EVENT_GAMEPAD_ADDED: {
                if (devices.contains(event.gdevice.which))
                    break;
                SDL_Gamepad *gamepad = SDL_OpenGamepad(event.gdevice.which);
                if (!gamepad)
                    break;
                const char *name = SDL_GetGamepadName(gamepad);
                const GamepadDeviceId id = collector.ConnectGamepad(name ? name : "SDL Gamepad", true);
                devices.try_emplace(event.gdevice.which, id, gamepad, nullptr);
                break;
            }
            case SDL_EVENT_GAMEPAD_REMOVED: {
                const auto found = devices.find(event.gdevice.which);
                if (found == devices.end())
                    break;
                (void)collector.DisconnectGamepad(found->second.id);
                if (found->second.gamepad)
                    SDL_CloseGamepad(found->second.gamepad);
                devices.erase(found);
                break;
            }
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                if (const auto found = devices.find(event.gbutton.which); found != devices.end())
                    if (const auto button = MapGamepadButton(event.gbutton.button))
                        (void)collector.SetGamepadButton(found->second.id, *button, event.gbutton.down);
                break;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                if (const auto found = devices.find(event.gaxis.which); found != devices.end())
                    if (const auto axis = MapGamepadAxis(event.gaxis.axis)) {
                        const float normalized = event.gaxis.value < 0 ? static_cast<float>(event.gaxis.value) / 32768.0F
                                                                       : static_cast<float>(event.gaxis.value) / 32767.0F;
                        (void)collector.SetGamepadAxis(found->second.id, *axis, normalized);
                    }
                break;
            default:
                break;
        }
    }

    /** @brief Opens and closes raw joysticks and forwards their raw button and axis state. */
    void SdlInputBackend::Impl::HandleJoystickEvent(const SDL_Event &event) {
        switch (event.type) {
            case SDL_EVENT_JOYSTICK_ADDED: {
                if (devices.contains(event.jdevice.which) || SDL_IsGamepad(event.jdevice.which))
                    break;
                SDL_Joystick *joystick = SDL_OpenJoystick(event.jdevice.which);
                if (!joystick)
                    break;
                const char *name = SDL_GetJoystickName(joystick);
                const auto buttons = static_cast<std::size_t>(std::clamp(SDL_GetNumJoystickButtons(joystick), 0, 256));
                const auto axes = static_cast<std::size_t>(std::clamp(SDL_GetNumJoystickAxes(joystick), 0, 256));
                const GamepadDeviceId id = collector.ConnectGamepad(name ? name : "SDL Joystick", false, buttons, axes);
                devices.try_emplace(event.jdevice.which, id, nullptr, joystick);
                break;
            }
            case SDL_EVENT_JOYSTICK_REMOVED: {
                const auto found = devices.find(event.jdevice.which);
                if (found == devices.end())
                    break;
                (void)collector.DisconnectGamepad(found->second.id);
                if (found->second.gamepad)
                    SDL_CloseGamepad(found->second.gamepad);
                else if (found->second.joystick)
                    SDL_CloseJoystick(found->second.joystick);
                devices.erase(found);
                break;
            }
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP:
                if (const auto found = devices.find(event.jbutton.which); found != devices.end() && found->second.joystick)
                    (void)collector.SetRawGamepadButton(found->second.id, event.jbutton.button, event.jbutton.down);
                break;
            case SDL_EVENT_JOYSTICK_AXIS_MOTION:
                if (const auto found = devices.find(event.jaxis.which); found != devices.end() && found->second.joystick) {
                    const float normalized = event.jaxis.value < 0 ? static_cast<float>(event.jaxis.value) / 32768.0F
                                                                   : static_cast<float>(event.jaxis.value) / 32767.0F;
                    (void)collector.SetRawGamepadAxis(found->second.id, event.jaxis.axis, normalized);
                }
                break;
            default:
                break;
        }
    }

    SdlInputBackend::SdlInputBackend() : impl_(std::make_unique<Impl>()) {}

    SdlInputBackend::~SdlInputBackend() {
        for (auto &[nativeId, device] : impl_->devices) {
            static_cast<void>(nativeId);
            if (device.gamepad)
                SDL_CloseGamepad(device.gamepad);
            else if (device.joystick)
                SDL_CloseJoystick(device.joystick);
        }
    }

    void SdlInputBackend::BeginFrame(const FrameNumber frame) {
        impl_->collector.BeginFrame(frame);
        for (auto &touch : impl_->touches)
            if (touch.terminal)
                touch = {};
        impl_->touchAdmissionClosed = false;
        if (impl_->neutralizeOnBeginFrame) {
            impl_->collector.Neutralize();
            impl_->neutralizeOnBeginFrame = false;
        }
    }

    /** @copydoc SdlInputBackend::BindWindow */
    void SdlInputBackend::BindWindow(const SDL_WindowID windowId) noexcept {
        if (impl_->windowId == windowId)
            return;
        impl_->collector.Neutralize();
        impl_->CancelTouches(TouchCancellationReason::SurfaceLost);
        impl_->windowId = windowId;
        impl_->pointerWidth = impl_->pointerHeight = 0;
        if (auto *window = SDL_GetWindowFromID(windowId))
            (void)SDL_GetWindowSize(window, &impl_->pointerWidth, &impl_->pointerHeight);
        impl_->neutralizeOnBeginFrame = true;
    }

    void SdlInputBackend::ProcessEvent(const SDL_Event &event) {
        if (const auto eventWindow = EventWindowId(event); eventWindow && (impl_->windowId == 0 || *eventWindow != impl_->windowId))
            return;
        switch (event.type) {
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
            case SDL_EVENT_TEXT_INPUT:
            case SDL_EVENT_TEXT_EDITING:
                impl_->HandleKeyboardEvent(event);
                break;
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            case SDL_EVENT_MOUSE_WHEEL:
            case SDL_EVENT_MOUSE_ADDED:
            case SDL_EVENT_MOUSE_REMOVED:
                impl_->HandlePointerEvent(event);
                break;
            case SDL_EVENT_FINGER_DOWN:
            case SDL_EVENT_FINGER_UP:
            case SDL_EVENT_FINGER_MOTION:
            case SDL_EVENT_FINGER_CANCELED:
                impl_->HandleTouchEvent(event.tfinger);
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED:
            case SDL_EVENT_WINDOW_FOCUS_LOST:
            case SDL_EVENT_WINDOW_MOUSE_ENTER:
            case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            case SDL_EVENT_WINDOW_RESIZED:
                impl_->HandleWindowEvent(event);
                break;
            case SDL_EVENT_GAMEPAD_ADDED:
            case SDL_EVENT_GAMEPAD_REMOVED:
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                impl_->HandleGamepadEvent(event);
                break;
            case SDL_EVENT_JOYSTICK_ADDED:
            case SDL_EVENT_JOYSTICK_REMOVED:
            case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
            case SDL_EVENT_JOYSTICK_BUTTON_UP:
            case SDL_EVENT_JOYSTICK_AXIS_MOTION:
                impl_->HandleJoystickEvent(event);
                break;
            default:
                break;
        }
    }

    /** @copydoc SdlInputBackend::PollEvents */
    void SdlInputBackend::PollEvents() {
        SDL_Event event;
        while (SDL_PollEvent(&event))
            ProcessEvent(event);
    }

    const RawInputSnapshot &SdlInputBackend::Commit() {
        return impl_->collector.Commit();
    }

    RawInputCollector &SdlInputBackend::Collector() noexcept {
        return impl_->collector;
    }

    /** @copydoc SdlInputBackend::Haptics */
    IGamepadHaptics *SdlInputBackend::Haptics() noexcept {
        return this;
    }

    /** @copydoc SdlInputBackend::StartTextInput */
    bool SdlInputBackend::StartTextInput(SDL_Window *window, const SdlTextInputArea area) const noexcept {
        return SetTextInputArea(window, area) && SDL_StartTextInput(window);
    }

    /** @copydoc SdlInputBackend::SetTextInputArea */
    bool SdlInputBackend::SetTextInputArea(SDL_Window *window, const SdlTextInputArea area) const noexcept {
        if (window == nullptr || impl_->windowId == 0 || SDL_GetWindowID(window) != impl_->windowId || area.width <= 0 || area.height <= 0)
            return false;
        const SDL_Rect nativeArea{area.x, area.y, area.width, area.height};
        return SDL_SetTextInputArea(window, &nativeArea, 0);
    }

    /** @copydoc SdlInputBackend::StopTextInput */
    void SdlInputBackend::StopTextInput(SDL_Window *window) const noexcept {
        if (window != nullptr && impl_->windowId != 0 && SDL_GetWindowID(window) == impl_->windowId)
            SDL_StopTextInput(window);
    }

    Result<void> SdlInputBackend::PlayRumble(const GamepadDeviceId id, const RumbleEffect effect) {
        const auto found = std::ranges::find_if(impl_->devices, [id](const auto &entry) {
            return entry.second.id == id;
        });
        if (found == impl_->devices.end())
            return Result<void>::Failure(SdlError(HapticsStaleDevice, "Gamepad is unavailable."));
        if (!found->second.gamepad)
            return Result<void>::Failure(SdlError(HapticsUnsupported, "Gamepad haptics are unsupported."));
        const auto toAmplitude = [](const float value) {
            return static_cast<std::uint16_t>(std::clamp(value, 0.0F, 1.0F) * 65535.0F);
        };
        if (const bool rumbleOk = SDL_RumbleGamepad(found->second.gamepad, toAmplitude(effect.lowFrequency),
                                                    toAmplitude(effect.highFrequency), effect.durationMilliseconds);
            !rumbleOk)
            return Result<void>::Failure(SdlError(HapticsFailed, "Unable to start gamepad rumble."));
        return Result<void>::Success();
    }

    Result<void> SdlInputBackend::Stop(const GamepadDeviceId id) {
        return PlayRumble(id, {});
    }
}  // namespace Horo::Input
