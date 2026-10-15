#include "Horo/Foundation/Utf8.h"
#include "Horo/Runtime/Input.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace Horo::Input {
    namespace {
        template <typename Enum> constexpr std::size_t Index(Enum value) noexcept {
            return static_cast<std::size_t>(value);
        }

        void Advance(ButtonState &state) noexcept {
            state.pressed = false;
            state.released = false;
        }

        void Set(ButtonState &state, const bool down) noexcept {
            if (state.down == down)
                return;
            state.down = down;
            state.pressed = down;
            state.released = !down;
        }
    }  // namespace

    const ButtonState &RawInputSnapshot::State(const Key key) const noexcept {
        static const ButtonState empty{};
        return Index(key) < keyboard.size() ? keyboard[Index(key)] : empty;
    }

    const ButtonState &RawInputSnapshot::State(const PointerButton button) const noexcept {
        static const ButtonState empty{};
        return Index(button) < pointer.buttons.size() ? pointer.buttons[Index(button)] : empty;
    }

    const GamepadState *RawInputSnapshot::FindGamepad(const GamepadDeviceId id) const noexcept {
        const auto found = std::ranges::find(gamepads, id, &GamepadState::id);
        return found == gamepads.end() ? nullptr : std::to_address(found);
    }

    struct RawInputCollector::Impl {
        std::array<RawInputSnapshot, 2> snapshots;
        std::size_t write{0};
        std::uint64_t nextGeneration{1};
    };

    RawInputCollector::RawInputCollector() : impl_(std::make_unique<Impl>()) {
        for (RawInputSnapshot &snapshot : impl_->snapshots) {
            snapshot.gamepads.reserve(4);
            snapshot.text.reserve(64);
        }
    }

    RawInputCollector::~RawInputCollector() = default;

    void RawInputCollector::BeginFrame(const FrameNumber frame) {
        RawInputSnapshot &next = impl_->snapshots[impl_->write];
        const RawInputSnapshot &previous = impl_->snapshots[1 - impl_->write];
        next = previous;
        next.frame = frame;
        next.text.clear();
        next.pointer.deltaX = 0.0F;
        next.pointer.deltaY = 0.0F;
        next.pointer.wheelX = 0.0F;
        next.pointer.wheelY = 0.0F;
        next.touchOverflow = false;
        for (auto &touch : next.touches) {
            if (!touch.contact.down)
                touch = {};
            else
                Advance(touch.contact);
        }
        for (ButtonState &state : next.keyboard)
            Advance(state);
        for (ButtonState &state : next.pointer.buttons)
            Advance(state);
        for (GamepadState &pad : next.gamepads) {
            for (ButtonState &state : pad.buttons)
                Advance(state);
            for (ButtonState &state : pad.rawButtons)
                Advance(state);
        }
    }

    void RawInputCollector::SetKey(const Key key, const bool down) {
        auto &keys = impl_->snapshots[impl_->write].keyboard;
        if (Index(key) < keys.size())
            Set(keys[Index(key)], down);
    }

    void RawInputCollector::SetPointerButton(const PointerButton button, const bool down) {
        auto &buttons = impl_->snapshots[impl_->write].pointer.buttons;
        if (Index(button) < buttons.size())
            Set(buttons[Index(button)], down);
    }

    void RawInputCollector::SetPointerPosition(const float x, const float y) {
        PointerState &pointer = impl_->snapshots[impl_->write].pointer;
        pointer.deltaX += x - pointer.x;
        pointer.deltaY += y - pointer.y;
        pointer.x = x;
        pointer.y = y;
    }

    void RawInputCollector::AddPointerWheel(const float x, const float y) {
        PointerState &pointer = impl_->snapshots[impl_->write].pointer;
        pointer.wheelX += x;
        pointer.wheelY += y;
    }

    /** @copydoc RawInputCollector::SetTouchContact */
    TouchCollectionStatus RawInputCollector::SetTouchContact(const TouchContactId id, const float x, const float y,
                                                             const bool down) noexcept {
        if (!id.IsValid() || !std::isfinite(x) || !std::isfinite(y))
            return TouchCollectionStatus::Invalid;
        auto &snapshot = impl_->snapshots[impl_->write];
        if (!snapshot.window.focused || !snapshot.window.pointerDeviceAvailable)
            return TouchCollectionStatus::Unavailable;
        if (snapshot.touchOverflow)
            return TouchCollectionStatus::CapacityExceeded;
        auto found = std::ranges::find(snapshot.touches, id, &TouchContactState::id);
        if (found != snapshot.touches.end() && (!found->contact.down || found->cancelled))
            return TouchCollectionStatus::Stale;
        if (found == snapshot.touches.end()) {
            if (!down)
                return TouchCollectionStatus::Stale;
            found = std::ranges::find_if(snapshot.touches, [](const TouchContactState &touch) {
                return !touch.id.IsValid();
            });
            if (found == snapshot.touches.end()) {
                snapshot.touchOverflow = true;
                for (auto &touch : snapshot.touches) {
                    touch.cancelled = true;
                    Set(touch.contact, false);
                }
                return TouchCollectionStatus::CapacityExceeded;
            }
            found->id = id;
        }
        found->x = x;
        found->y = y;
        if (found->contact.down != down) {
            found->contact.down = down;
            found->contact.pressed |= down;
            found->contact.released |= !down;
        }
        return TouchCollectionStatus::Accepted;
    }

    /** @copydoc RawInputCollector::CancelTouchContact */
    TouchCollectionStatus RawInputCollector::CancelTouchContact(const TouchContactId id) noexcept {
        using enum TouchCollectionStatus;
        if (!id.IsValid())
            return Invalid;
        auto &touches = impl_->snapshots[impl_->write].touches;
        const auto found = std::ranges::find(touches, id, &TouchContactState::id);
        if (found == touches.end() || !found->contact.down)
            return Stale;
        found->cancelled = true;
        Set(found->contact, false);
        return Accepted;
    }

    /** @copydoc RawInputCollector::CancelTouchContacts */
    void RawInputCollector::CancelTouchContacts(const TouchCancellationReason reason) noexcept {
        auto &snapshot = impl_->snapshots[impl_->write];
        snapshot.touchOverflow |= reason == TouchCancellationReason::CapacityExceeded;
        for (auto &touch : snapshot.touches) {
            if (!touch.id.IsValid())
                continue;
            touch.cancelled = true;
            Set(touch.contact, false);
        }
    }

    void RawInputCollector::AppendText(const std::string_view utf8) {
        RawInputSnapshot &snapshot = impl_->snapshots[impl_->write];
        if (constexpr std::size_t maximumTextBytes = 4096;
            utf8.size() > maximumTextBytes - snapshot.text.size() || !IsValidUtf8ScalarSequence(utf8))
            return;
        snapshot.text.append(utf8);
        if (!utf8.empty() && snapshot.composition.active) {
            snapshot.composition = {};
            ++snapshot.compositionRevision;
        }
    }

    void RawInputCollector::SetTextComposition(const std::string_view utf8, const std::int32_t selectionStart,
                                               const std::int32_t selectionLength) {
        TextCompositionState &composition = impl_->snapshots[impl_->write].composition;
        if (constexpr std::size_t maximumCompositionBytes = 4096;
            utf8.size() > maximumCompositionBytes || !IsValidUtf8ScalarSequence(utf8)) {
            composition = {};
            ++impl_->snapshots[impl_->write].compositionRevision;
            return;
        }
        composition.text.assign(utf8);
        const auto characterCount = static_cast<std::int32_t>(std::ranges::count_if(utf8, [](const char byte) {
            return (std::to_integer<unsigned int>(static_cast<std::byte>(byte)) & 0xC0U) != 0x80U;
        }));
        composition.selectionStart = std::clamp(selectionStart, 0, characterCount);
        composition.selectionLength = std::clamp(selectionLength, 0, characterCount - composition.selectionStart);
        composition.active = !composition.text.empty();
        ++impl_->snapshots[impl_->write].compositionRevision;
    }

    /** @copydoc RawInputCollector::SetSensorSample */
    bool RawInputCollector::SetSensorSample(const SensorKind sensor, SensorSample sample) noexcept {
        auto &sensors = impl_->snapshots[impl_->write].sensors;
        if (Index(sensor) >= sensors.size() || sample.availability > SensorAvailability::Available)
            return false;
        if (sample.availability == SensorAvailability::Available) {
            if (sample.timestampNanoseconds <= 0 || sample.timestampNanoseconds < sensors[Index(sensor)].timestampNanoseconds ||
                !std::ranges::all_of(sample.value, [](float value) {
                return std::isfinite(value);
            }))
                return false;
        } else {
            sample.value = {};
            sample.counter = 0;
            sample.timestampNanoseconds = 0;
        }
        sensors[Index(sensor)] = sample;
        return true;
    }

    void RawInputCollector::SetModifiers(const ModifierState modifiers) noexcept {
        impl_->snapshots[impl_->write].modifiers = modifiers;
    }

    void RawInputCollector::SetWindowState(const WindowInputState state) noexcept {
        impl_->snapshots[impl_->write].window = state;
        if (!state.focused || !state.pointerDeviceAvailable)
            Neutralize();
    }

    void RawInputCollector::Neutralize() noexcept {
        RawInputSnapshot &snapshot = impl_->snapshots[impl_->write];
        for (ButtonState &state : snapshot.keyboard)
            Set(state, false);
        for (ButtonState &state : snapshot.pointer.buttons)
            Set(state, false);
        for (auto &touch : snapshot.touches) {
            if (touch.id.IsValid()) {
                touch.cancelled = true;
                Set(touch.contact, false);
            }
        }
        for (auto &sensor : snapshot.sensors) {
            if (sensor.availability == SensorAvailability::Available)
                sensor = {.availability = SensorAvailability::Suspended};
        }
        snapshot.modifiers = {};
        for (GamepadState &gamepad : snapshot.gamepads) {
            for (ButtonState &state : gamepad.buttons)
                Set(state, false);
            for (ButtonState &state : gamepad.rawButtons)
                Set(state, false);
            gamepad.axes.fill(0.0F);
            std::ranges::fill(gamepad.rawAxes, 0.0F);
        }
        snapshot.text.clear();
        if (snapshot.composition.active) {
            snapshot.composition = {};
            ++snapshot.compositionRevision;
        }
    }

    GamepadDeviceId RawInputCollector::ConnectGamepad(std::string name, const bool canonicalMapping, const std::size_t rawButtonCount,
                                                      const std::size_t rawAxisCount) {
        auto &pads = impl_->snapshots[impl_->write].gamepads;
        std::uint32_t slot = 0;
        while (std::ranges::any_of(pads, [slot](const GamepadState &pad) {
            return pad.id.slot == slot;
        }))
            ++slot;
        const GamepadDeviceId id{slot, impl_->nextGeneration++};
        GamepadState state{.id = id, .name = std::move(name), .canonicalMapping = canonicalMapping};
        state.rawButtons.resize(rawButtonCount);
        state.rawAxes.resize(rawAxisCount);
        pads.push_back(std::move(state));
        return id;
    }

    bool RawInputCollector::DisconnectGamepad(const GamepadDeviceId id) {
        auto &pads = impl_->snapshots[impl_->write].gamepads;
        return std::erase_if(pads, [id](const GamepadState &pad) {
            return pad.id == id;
        }) != 0;
    }

    bool RawInputCollector::SetGamepadButton(const GamepadDeviceId id, const GamepadButton button, const bool down) {
        auto &pads = impl_->snapshots[impl_->write].gamepads;
        const auto pad = std::ranges::find(pads, id, &GamepadState::id);
        if (pad == pads.end() || Index(button) >= pad->buttons.size())
            return false;
        Set(pad->buttons[Index(button)], down);
        return true;
    }

    bool RawInputCollector::SetGamepadAxis(const GamepadDeviceId id, const GamepadAxis axis, const float value) {
        auto &pads = impl_->snapshots[impl_->write].gamepads;
        const auto pad = std::ranges::find(pads, id, &GamepadState::id);
        if (pad == pads.end() || Index(axis) >= pad->axes.size() || !std::isfinite(value))
            return false;
        pad->axes[Index(axis)] = std::clamp(value, -1.0F, 1.0F);
        return true;
    }

    bool RawInputCollector::SetRawGamepadButton(const GamepadDeviceId id, const std::size_t button, const bool down) {
        auto &pads = impl_->snapshots[impl_->write].gamepads;
        const auto pad = std::ranges::find(pads, id, &GamepadState::id);
        if (pad == pads.end() || button >= pad->rawButtons.size())
            return false;
        Set(pad->rawButtons[button], down);
        return true;
    }

    bool RawInputCollector::SetRawGamepadAxis(const GamepadDeviceId id, const std::size_t axis, const float value) {
        auto &pads = impl_->snapshots[impl_->write].gamepads;
        const auto pad = std::ranges::find(pads, id, &GamepadState::id);
        if (pad == pads.end() || axis >= pad->rawAxes.size() || !std::isfinite(value))
            return false;
        pad->rawAxes[axis] = std::clamp(value, -1.0F, 1.0F);
        return true;
    }

    const RawInputSnapshot &RawInputCollector::Commit() {
        const std::size_t committed = impl_->write;
        impl_->write = 1 - impl_->write;
        return impl_->snapshots[committed];
    }

}  // namespace Horo::Input
