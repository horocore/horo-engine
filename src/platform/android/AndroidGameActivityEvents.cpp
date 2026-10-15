#include "AndroidGameActivityInput.h"

#include <android/input.h>
#include <android/keycodes.h>
#include <limits>

namespace Horo::Platform::Android {
    namespace {
        bool IsPad(const std::int32_t source) noexcept {
            return (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD ||
                   (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK || (source & AINPUT_SOURCE_DPAD) == AINPUT_SOURCE_DPAD;
        }

        Input::Key MapKey(const std::int32_t code) noexcept {
            if (code >= AKEYCODE_A && code <= AKEYCODE_Z)
                return static_cast<Input::Key>(static_cast<int>(Input::Key::A) + code - AKEYCODE_A);
            if (code >= AKEYCODE_0 && code <= AKEYCODE_9)
                return static_cast<Input::Key>(static_cast<int>(Input::Key::Digit0) + code - AKEYCODE_0);
            if (code >= AKEYCODE_F1 && code <= AKEYCODE_F12)
                return static_cast<Input::Key>(static_cast<int>(Input::Key::F1) + code - AKEYCODE_F1);
            switch (code) {
                case AKEYCODE_ESCAPE:
                case AKEYCODE_BACK:
                    return Input::Key::Escape;
                case AKEYCODE_ENTER:
                    return Input::Key::Enter;
                case AKEYCODE_TAB:
                    return Input::Key::Tab;
                case AKEYCODE_SPACE:
                    return Input::Key::Space;
                case AKEYCODE_DEL:
                    return Input::Key::Backspace;
                case AKEYCODE_FORWARD_DEL:
                    return Input::Key::Delete;
                case AKEYCODE_DPAD_LEFT:
                    return Input::Key::Left;
                case AKEYCODE_DPAD_RIGHT:
                    return Input::Key::Right;
                case AKEYCODE_DPAD_UP:
                    return Input::Key::Up;
                case AKEYCODE_DPAD_DOWN:
                    return Input::Key::Down;
                case AKEYCODE_MOVE_HOME:
                    return Input::Key::Home;
                case AKEYCODE_MOVE_END:
                    return Input::Key::End;
                case AKEYCODE_PAGE_UP:
                    return Input::Key::PageUp;
                case AKEYCODE_PAGE_DOWN:
                    return Input::Key::PageDown;
                default:
                    return Input::Key::Unknown;
            }
        }

        Input::GamepadButton MapButton(const std::int32_t code) noexcept {
            using enum Input::GamepadButton;
            switch (code) {
                case AKEYCODE_BUTTON_A:
                    return South;
                case AKEYCODE_BUTTON_B:
                    return East;
                case AKEYCODE_BUTTON_X:
                    return West;
                case AKEYCODE_BUTTON_Y:
                    return North;
                case AKEYCODE_BUTTON_L1:
                    return LeftShoulder;
                case AKEYCODE_BUTTON_R1:
                    return RightShoulder;
                case AKEYCODE_BUTTON_THUMBL:
                    return LeftStick;
                case AKEYCODE_BUTTON_THUMBR:
                    return RightStick;
                case AKEYCODE_BUTTON_START:
                    return Start;
                case AKEYCODE_BUTTON_SELECT:
                    return Select;
                case AKEYCODE_DPAD_UP:
                    return DPadUp;
                case AKEYCODE_DPAD_DOWN:
                    return DPadDown;
                case AKEYCODE_DPAD_LEFT:
                    return DPadLeft;
                case AKEYCODE_DPAD_RIGHT:
                    return DPadRight;
                default:
                    return Count;
            }
        }
    }  // namespace

    /** @brief Project actual GameActivity key/button events into canonical Input controls. */
    void GameActivityInput::KeyEvent(const GameActivityKeyEvent &event) noexcept {
        if (event.eventTime <= minimumInputTimestamp_)
            return;
        if (event.action != AKEY_EVENT_ACTION_DOWN && event.action != AKEY_EVENT_ACTION_UP)
            return;
        const bool down = event.action == AKEY_EVENT_ACTION_DOWN;
        if (down && event.repeatCount > 0)
            return;
        static_cast<void>(session_.Modifiers({.control = (event.metaState & AMETA_CTRL_ON) != 0,
                                              .shift = (event.metaState & AMETA_SHIFT_ON) != 0,
                                              .alt = (event.metaState & AMETA_ALT_ON) != 0,
                                              .command = (event.metaState & AMETA_META_ON) != 0}));
        if (IsPad(event.source)) {
            if (session_.ConnectGamepad(event.deviceId) == InputAdmission::Accepted)
                static_cast<void>(session_.Button(event.deviceId, MapButton(event.keyCode), down));
        } else
            static_cast<void>(session_.Key(MapKey(event.keyCode), down));
    }

    /** @brief Merge one admitted physical joystick's axes and independent direction hat. */
    void GameActivityInput::PadMotion(const GameActivityMotionEvent &event) noexcept {
        if (session_.ConnectGamepad(event.deviceId) != InputAdmission::Accepted)
            return;
        constexpr std::array axes{AMOTION_EVENT_AXIS_X,  AMOTION_EVENT_AXIS_Y,        AMOTION_EVENT_AXIS_Z,
                                  AMOTION_EVENT_AXIS_RZ, AMOTION_EVENT_AXIS_LTRIGGER, AMOTION_EVENT_AXIS_RTRIGGER};
        for (std::size_t i = 0; i < axes.size(); ++i)
            static_cast<void>(session_.Axis(event.deviceId, static_cast<Input::GamepadAxis>(i),
                                            GameActivityPointerAxes_getAxisValue(&event.pointers[0], axes[i])));
        const float hatX = GameActivityPointerAxes_getAxisValue(&event.pointers[0], AMOTION_EVENT_AXIS_HAT_X);
        const float hatY = GameActivityPointerAxes_getAxisValue(&event.pointers[0], AMOTION_EVENT_AXIS_HAT_Y);
        static_cast<void>(session_.Hat(event.deviceId, hatX, hatY));
    }

    /** @brief Project one bounded touch pointer, stopping the buffer on cancellation or exhausted identity. */
    bool GameActivityInput::TouchPoint(const GameActivityMotionEvent &event, const std::uint32_t i, const std::uint32_t changed) noexcept {
        const auto action = event.action & AMOTION_EVENT_ACTION_MASK;
        const auto &point = event.pointers[i];
        if (point.id < 0 || static_cast<std::size_t>(point.id) >= touches_.size()) {
            session_.CancelTouches();
            touches_ = {};
            return false;
        }
        auto &id = touches_[static_cast<std::size_t>(point.id)];
        const bool start = i == changed && (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_POINTER_DOWN);
        const bool end = i == changed && (action == AMOTION_EVENT_ACTION_UP || action == AMOTION_EVENT_ACTION_POINTER_UP);
        if (start) {
            if (nextTouchGeneration_ == std::numeric_limits<std::uint64_t>::max()) {
                status_ = InputAdmission::Capacity;
                session_.Shutdown();
                return false;
            }
            id = {static_cast<std::uint64_t>(point.id) + 1, nextTouchGeneration_++};
        }
        if (id.IsValid()) {
            const auto admitted = session_.Touch(id, GameActivityPointerAxes_getX(&point), GameActivityPointerAxes_getY(&point), !end);
            if (admitted == InputAdmission::Invalid || admitted == InputAdmission::Capacity) {
                session_.CancelTouches();
                touches_ = {};
                return false;
            }
        }
        if (end)
            id = {};
        return true;
    }

    /** @brief Project bounded joystick axes or generation-safe multi-touch, never native identities into gameplay. */
    void GameActivityInput::MotionEvent(const GameActivityMotionEvent &event) noexcept {
        // Pinned GameActivity 3.0.5 motion timestamps are milliseconds; key timestamps are nanoseconds.
        if (event.eventTime <= minimumInputTimestamp_ / 1'000'000)
            return;
        if (event.pointerCount == 0 || event.pointerCount > GAMEACTIVITY_MAX_NUM_POINTERS_IN_MOTION_EVENT) {
            session_.CancelTouches();
            touches_ = {};
            return;
        }
        if (IsPad(event.source)) {
            PadMotion(event);
            return;
        }
        const auto action = event.action & AMOTION_EVENT_ACTION_MASK;
        if (action == AMOTION_EVENT_ACTION_CANCEL) {
            session_.CancelTouches();
            touches_ = {};
            return;
        }
        const auto changed = static_cast<std::uint32_t>((event.action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                                                        AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        if (changed >= event.pointerCount) {
            session_.CancelTouches();
            touches_ = {};
            return;
        }
        for (std::uint32_t i = 0; i < event.pointerCount; ++i)
            if (!TouchPoint(event, i, changed))
                return;
    }
}  // namespace Horo::Platform::Android
