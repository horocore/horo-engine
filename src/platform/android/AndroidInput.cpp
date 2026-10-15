#include "Horo/Platform/AndroidInput.h"

#include <algorithm>
#include <cmath>
#include <new>

namespace Horo::Platform::Android {
    namespace {
        template <class T> constexpr std::size_t Index(T value) noexcept {
            return static_cast<std::size_t>(value);
        }
    }  // namespace

    /** @copydoc AndroidInputSession::AndroidInputSession */
    AndroidInputSession::AndroidInputSession(Input::RawInputCollector &collector) noexcept : collector_(collector) {}

    AndroidInputSession::~AndroidInputSession() {
        Shutdown();
    }

    /** @brief Reject wrong-thread and retired calls before touching borrowed Input storage. */
    InputAdmission AndroidInputSession::Check(const bool requireFocus) const noexcept {
        using enum InputAdmission;
        if (owner_ != std::this_thread::get_id())
            return WrongThread;
        if (stopped_)
            return Stopped;
        return requireFocus && !focused_ ? Unavailable : Accepted;
    }

    /** @brief Publish loss evidence before another native sensor can enter the Input frame. */
    void AndroidInputSession::RefreshSensors() noexcept {
        using enum SensorAvailability;
        for (std::size_t i = 0; i < sensors_.size(); ++i) {
            auto availability = Available;
            if (failed_[i])
                availability = NativeFailure;
            else if (!supported_[i])
                availability = Unsupported;
            else if (!enabled_[i])
                availability = Disabled;
            else if (i == Index(SensorKind::StepCounter) && permission_ != PermissionState::Granted)
                availability = PermissionRequired;
            else if (!focused_ || stopped_)
                availability = Suspended;
            if (availability != Available || sensors_[i].availability != availability)
                sensors_[i] = {.availability = availability == Available ? AwaitingSample : availability};
            static_cast<void>(collector_.SetSensorSample(static_cast<SensorKind>(i), sensors_[i]));
        }
    }

    /** @copydoc AndroidInputSession::SetFocus */
    InputAdmission AndroidInputSession::SetFocus(const bool resumed, const bool focused) noexcept {
        using enum InputAdmission;
        if (const auto check = Check(false); check != Accepted)
            return check;
        focused_ = resumed && focused;
        collector_.SetWindowState({.focused = focused_, .pointerDeviceAvailable = focused_});
        if (!focused_)
            for (auto &pad : pads_) {
                pad.directionKeys = {};
                pad.directionHat = {};
            }
        RefreshSensors();
        return Accepted;
    }

    /** @copydoc AndroidInputSession::SetPermission */
    InputAdmission AndroidInputSession::SetPermission(const PermissionState state) noexcept {
        using enum InputAdmission;
        using enum PermissionState;
        if (const auto check = Check(false); check != Accepted)
            return check;
        if (state > Revoked)
            return Invalid;
        const bool lostGrant = permission_ == Granted && state != Granted;
        permission_ = state;
        if (lostGrant) {
            collector_.Neutralize();
            for (auto &pad : pads_) {
                pad.directionKeys = {};
                pad.directionHat = {};
            }
            sensors_ = {};
        }
        RefreshSensors();
        return Accepted;
    }

    /** @copydoc AndroidInputSession::Key */
    InputAdmission AndroidInputSession::Key(const Input::Key key, const bool down) noexcept {
        using enum InputAdmission;
        if (const auto check = Check(); check != Accepted)
            return check;
        if (key <= Input::Key::Unknown || key >= Input::Key::Count)
            return Invalid;
        collector_.SetKey(key, down);
        return Accepted;
    }

    /** @copydoc AndroidInputSession::Modifiers */
    InputAdmission AndroidInputSession::Modifiers(const Input::ModifierState modifiers) noexcept {
        using enum InputAdmission;
        if (const auto check = Check(); check != Accepted)
            return check;
        collector_.SetModifiers(modifiers);
        return Accepted;
    }

    /** @copydoc AndroidInputSession::Touch */
    InputAdmission AndroidInputSession::Touch(const Input::TouchContactId id, const float x, const float y, const bool down) noexcept {
        using enum InputAdmission;
        if (const auto check = Check(); check != Accepted)
            return check;
        switch (collector_.SetTouchContact(id, x, y, down)) {
            case Input::TouchCollectionStatus::Accepted:
                return Accepted;
            case Input::TouchCollectionStatus::Invalid:
                return Invalid;
            case Input::TouchCollectionStatus::Stale:
                return Stale;
            case Input::TouchCollectionStatus::CapacityExceeded:
                return Capacity;
            default:
                return Unavailable;
        }
    }

    /** @copydoc AndroidInputSession::CancelTouches */
    void AndroidInputSession::CancelTouches() noexcept {
        if (Check(false) == InputAdmission::Accepted)
            collector_.CancelTouchContacts(Input::TouchCancellationReason::SurfaceLost);
    }

    /** @copydoc AndroidInputSession::ConnectGamepad */
    InputAdmission AndroidInputSession::ConnectGamepad(const std::int32_t nativeDevice) {
        using enum InputAdmission;
        if (const auto check = Check(false); check != Accepted)
            return check;
        if (nativeDevice < 0)
            return Invalid;
        if (std::ranges::find(pads_, nativeDevice, &Pad::nativeDevice) != pads_.end())
            return Accepted;
        const auto free = std::ranges::find(pads_, -1, &Pad::nativeDevice);
        if (free == pads_.end())
            return Capacity;
        try {
            const auto id = collector_.ConnectGamepad("Android canonical controller");
            *free = {.nativeDevice = nativeDevice, .id = id};
        } catch (const std::bad_alloc &) {
            return Capacity;
        }
        return Accepted;
    }

    /** @copydoc AndroidInputSession::DisconnectGamepad */
    InputAdmission AndroidInputSession::DisconnectGamepad(const std::int32_t nativeDevice) {
        using enum InputAdmission;
        if (const auto check = Check(false); check != Accepted)
            return check;
        const auto pad = std::ranges::find(pads_, nativeDevice, &Pad::nativeDevice);
        if (nativeDevice < 0 || pad == pads_.end())
            return Stale;
        static_cast<void>(collector_.DisconnectGamepad(pad->id));
        *pad = {};
        return Accepted;
    }

    /** @copydoc AndroidInputSession::Button */
    InputAdmission AndroidInputSession::Button(const std::int32_t device, const Input::GamepadButton button, const bool down) noexcept {
        using enum InputAdmission;
        if (const auto check = Check(); check != Accepted)
            return check;
        const auto pad = std::ranges::find(pads_, device, &Pad::nativeDevice);
        if (device < 0 || pad == pads_.end())
            return Stale;
        if (constexpr auto firstDirection = Index(Input::GamepadButton::DPadUp);
            Index(button) >= firstDirection && Index(button) < firstDirection + pad->directionKeys.size()) {
            const auto direction = Index(button) - firstDirection;
            pad->directionKeys[direction] = down;
            return collector_.SetGamepadButton(pad->id, button, down || pad->directionHat[direction]) ? Accepted : Invalid;
        }
        return collector_.SetGamepadButton(pad->id, button, down) ? Accepted : Invalid;
    }

    /** @copydoc AndroidInputSession::Hat */
    InputAdmission AndroidInputSession::Hat(const std::int32_t device, const float x, const float y) noexcept {
        using enum InputAdmission;
        if (const auto check = Check(); check != Accepted)
            return check;
        const auto pad = std::ranges::find(pads_, device, &Pad::nativeDevice);
        if (device < 0 || pad == pads_.end())
            return Stale;
        if (!std::isfinite(x) || !std::isfinite(y))
            return Invalid;
        pad->directionHat = {y < -0.5F, y > 0.5F, x < -0.5F, x > 0.5F};
        for (std::size_t i = 0; i < pad->directionHat.size(); ++i)
            static_cast<void>(collector_.SetGamepadButton(pad->id,
                                                          static_cast<Input::GamepadButton>(Index(Input::GamepadButton::DPadUp) + i),
                                                          pad->directionKeys[i] || pad->directionHat[i]));
        return Accepted;
    }

    /** @copydoc AndroidInputSession::Axis */
    InputAdmission AndroidInputSession::Axis(const std::int32_t device, const Input::GamepadAxis axis, const float value) noexcept {
        using enum InputAdmission;
        if (const auto check = Check(); check != Accepted)
            return check;
        const auto pad = std::ranges::find(pads_, device, &Pad::nativeDevice);
        if (device < 0 || pad == pads_.end())
            return Stale;
        return collector_.SetGamepadAxis(pad->id, axis, value) ? Accepted : Invalid;
    }

    /** @copydoc AndroidInputSession::ConfigureSensor */
    InputAdmission AndroidInputSession::ConfigureSensor(const SensorKind sensor, const bool supported, const bool enabled) noexcept {
        using enum InputAdmission;
        if (const auto check = Check(false); check != Accepted)
            return check;
        if (Index(sensor) >= sensors_.size())
            return Invalid;
        failed_[Index(sensor)] = false;
        supported_[Index(sensor)] = supported;
        enabled_[Index(sensor)] = enabled;
        RefreshSensors();
        return Accepted;
    }

    /** @copydoc AndroidInputSession::FailSensor */
    InputAdmission AndroidInputSession::FailSensor(const SensorKind sensor) noexcept {
        using enum InputAdmission;
        if (const auto check = Check(false); check != Accepted)
            return check;
        if (Index(sensor) >= sensors_.size())
            return Invalid;
        failed_[Index(sensor)] = true;
        RefreshSensors();
        return Accepted;
    }

    /** @copydoc AndroidInputSession::Sample */
    InputAdmission AndroidInputSession::Sample(const SensorKind sensor, const std::array<float, 3> value,
                                               const std::int64_t timestampNanoseconds, const std::uint64_t counter) noexcept {
        using enum InputAdmission;
        using enum SensorAvailability;
        if (const auto check = Check(); check != Accepted)
            return check;
        if (Index(sensor) >= sensors_.size())
            return Invalid;
        auto &sample = sensors_[Index(sensor)];
        if (sample.availability != Available && sample.availability != AwaitingSample)
            return Unavailable;
        const SensorSample next{Available, value, counter, timestampNanoseconds};
        if (!collector_.SetSensorSample(sensor, next))
            return Invalid;
        sample = next;
        return Accepted;
    }

    /** @copydoc AndroidInputSession::Sensor */
    SensorSample AndroidInputSession::Sensor(const SensorKind sensor) const noexcept {
        if (Check(false) != InputAdmission::Accepted || Index(sensor) >= sensors_.size())
            return {};
        return sensors_[Index(sensor)];
    }

    /** @copydoc AndroidInputSession::Permission */
    PermissionState AndroidInputSession::Permission() const noexcept {
        return owner_ == std::this_thread::get_id() ? permission_ : PermissionState::NotRequested;
    }

    /** @copydoc AndroidInputSession::Shutdown */
    void AndroidInputSession::Shutdown() noexcept {
        if (Check(false) != InputAdmission::Accepted)
            return;
        collector_.SetWindowState({.focused = false, .pointerDeviceAvailable = false});
        for (auto &pad : pads_) {
            if (pad.nativeDevice >= 0)
                static_cast<void>(collector_.DisconnectGamepad(pad.id));
            pad = {};
        }
        stopped_ = true;
        focused_ = false;
        RefreshSensors();
    }
}  // namespace Horo::Platform::Android
