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
        if (owner_ != std::this_thread::get_id())
            return InputAdmission::WrongThread;
        if (stopped_)
            return InputAdmission::Stopped;
        return requireFocus && !focused_ ? InputAdmission::Unavailable : InputAdmission::Accepted;
    }

    /** @brief Publish loss evidence before another native sensor can enter the Input frame. */
    void AndroidInputSession::RefreshSensors() noexcept {
        for (std::size_t i = 0; i < sensors_.size(); ++i) {
            auto availability = SensorAvailability::Available;
            if (failed_[i])
                availability = SensorAvailability::NativeFailure;
            else if (!supported_[i])
                availability = SensorAvailability::Unsupported;
            else if (!enabled_[i])
                availability = SensorAvailability::Disabled;
            else if (i == Index(SensorKind::StepCounter) && permission_ != PermissionState::Granted)
                availability = SensorAvailability::PermissionRequired;
            else if (!focused_ || stopped_)
                availability = SensorAvailability::Suspended;
            if (availability != SensorAvailability::Available || sensors_[i].availability != availability)
                sensors_[i] = {.availability =
                                   availability == SensorAvailability::Available ? SensorAvailability::AwaitingSample : availability};
            static_cast<void>(collector_.SetSensorSample(static_cast<SensorKind>(i), sensors_[i]));
        }
    }

    /** @copydoc AndroidInputSession::SetFocus */
    InputAdmission AndroidInputSession::SetFocus(const bool resumed, const bool focused) noexcept {
        if (const auto check = Check(false); check != InputAdmission::Accepted)
            return check;
        focused_ = resumed && focused;
        collector_.SetWindowState({.focused = focused_, .pointerDeviceAvailable = focused_});
        if (!focused_)
            for (auto &pad : pads_) {
                pad.directionKeys = {};
                pad.directionHat = {};
            }
        RefreshSensors();
        return InputAdmission::Accepted;
    }

    /** @copydoc AndroidInputSession::SetPermission */
    InputAdmission AndroidInputSession::SetPermission(const PermissionState state) noexcept {
        if (const auto check = Check(false); check != InputAdmission::Accepted)
            return check;
        if (state > PermissionState::Revoked)
            return InputAdmission::Invalid;
        const bool lostGrant = permission_ == PermissionState::Granted && state != PermissionState::Granted;
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
        return InputAdmission::Accepted;
    }

    /** @copydoc AndroidInputSession::Key */
    InputAdmission AndroidInputSession::Key(const Input::Key key, const bool down) noexcept {
        if (const auto check = Check(); check != InputAdmission::Accepted)
            return check;
        if (key <= Input::Key::Unknown || key >= Input::Key::Count)
            return InputAdmission::Invalid;
        collector_.SetKey(key, down);
        return InputAdmission::Accepted;
    }

    /** @copydoc AndroidInputSession::Modifiers */
    InputAdmission AndroidInputSession::Modifiers(const Input::ModifierState modifiers) noexcept {
        if (const auto check = Check(); check != InputAdmission::Accepted)
            return check;
        collector_.SetModifiers(modifiers);
        return InputAdmission::Accepted;
    }

    /** @copydoc AndroidInputSession::Touch */
    InputAdmission AndroidInputSession::Touch(const Input::TouchContactId id, const float x, const float y, const bool down) noexcept {
        if (const auto check = Check(); check != InputAdmission::Accepted)
            return check;
        switch (collector_.SetTouchContact(id, x, y, down)) {
            case Input::TouchCollectionStatus::Accepted:
                return InputAdmission::Accepted;
            case Input::TouchCollectionStatus::Invalid:
                return InputAdmission::Invalid;
            case Input::TouchCollectionStatus::Stale:
                return InputAdmission::Stale;
            case Input::TouchCollectionStatus::CapacityExceeded:
                return InputAdmission::Capacity;
            default:
                return InputAdmission::Unavailable;
        }
    }

    /** @copydoc AndroidInputSession::CancelTouches */
    void AndroidInputSession::CancelTouches() noexcept {
        if (Check(false) == InputAdmission::Accepted)
            collector_.CancelTouchContacts(Input::TouchCancellationReason::SurfaceLost);
    }

    /** @copydoc AndroidInputSession::ConnectGamepad */
    InputAdmission AndroidInputSession::ConnectGamepad(const std::int32_t nativeDevice) {
        if (const auto check = Check(false); check != InputAdmission::Accepted)
            return check;
        if (nativeDevice < 0)
            return InputAdmission::Invalid;
        if (std::ranges::find(pads_, nativeDevice, &Pad::nativeDevice) != pads_.end())
            return InputAdmission::Accepted;
        const auto free = std::ranges::find(pads_, -1, &Pad::nativeDevice);
        if (free == pads_.end())
            return InputAdmission::Capacity;
        try {
            const auto id = collector_.ConnectGamepad("Android canonical controller");
            *free = {.nativeDevice = nativeDevice, .id = id};
        } catch (const std::bad_alloc &) {
            return InputAdmission::Capacity;
        }
        return InputAdmission::Accepted;
    }

    /** @copydoc AndroidInputSession::DisconnectGamepad */
    InputAdmission AndroidInputSession::DisconnectGamepad(const std::int32_t nativeDevice) {
        if (const auto check = Check(false); check != InputAdmission::Accepted)
            return check;
        const auto pad = std::ranges::find(pads_, nativeDevice, &Pad::nativeDevice);
        if (nativeDevice < 0 || pad == pads_.end())
            return InputAdmission::Stale;
        static_cast<void>(collector_.DisconnectGamepad(pad->id));
        *pad = {};
        return InputAdmission::Accepted;
    }

    /** @copydoc AndroidInputSession::Button */
    InputAdmission AndroidInputSession::Button(const std::int32_t device, const Input::GamepadButton button, const bool down) noexcept {
        if (const auto check = Check(); check != InputAdmission::Accepted)
            return check;
        const auto pad = std::ranges::find(pads_, device, &Pad::nativeDevice);
        if (device < 0 || pad == pads_.end())
            return InputAdmission::Stale;
        constexpr auto firstDirection = Index(Input::GamepadButton::DPadUp);
        if (Index(button) >= firstDirection && Index(button) < firstDirection + pad->directionKeys.size()) {
            const auto direction = Index(button) - firstDirection;
            pad->directionKeys[direction] = down;
            return collector_.SetGamepadButton(pad->id, button, down || pad->directionHat[direction]) ? InputAdmission::Accepted
                                                                                                      : InputAdmission::Invalid;
        }
        return collector_.SetGamepadButton(pad->id, button, down) ? InputAdmission::Accepted : InputAdmission::Invalid;
    }

    /** @copydoc AndroidInputSession::Hat */
    InputAdmission AndroidInputSession::Hat(const std::int32_t device, const float x, const float y) noexcept {
        if (const auto check = Check(); check != InputAdmission::Accepted)
            return check;
        const auto pad = std::ranges::find(pads_, device, &Pad::nativeDevice);
        if (device < 0 || pad == pads_.end())
            return InputAdmission::Stale;
        if (!std::isfinite(x) || !std::isfinite(y))
            return InputAdmission::Invalid;
        pad->directionHat = {y < -0.5F, y > 0.5F, x < -0.5F, x > 0.5F};
        for (std::size_t i = 0; i < pad->directionHat.size(); ++i)
            static_cast<void>(collector_.SetGamepadButton(pad->id,
                                                          static_cast<Input::GamepadButton>(Index(Input::GamepadButton::DPadUp) + i),
                                                          pad->directionKeys[i] || pad->directionHat[i]));
        return InputAdmission::Accepted;
    }

    /** @copydoc AndroidInputSession::Axis */
    InputAdmission AndroidInputSession::Axis(const std::int32_t device, const Input::GamepadAxis axis, const float value) noexcept {
        if (const auto check = Check(); check != InputAdmission::Accepted)
            return check;
        const auto pad = std::ranges::find(pads_, device, &Pad::nativeDevice);
        if (device < 0 || pad == pads_.end())
            return InputAdmission::Stale;
        return collector_.SetGamepadAxis(pad->id, axis, value) ? InputAdmission::Accepted : InputAdmission::Invalid;
    }

    /** @copydoc AndroidInputSession::ConfigureSensor */
    InputAdmission AndroidInputSession::ConfigureSensor(const SensorKind sensor, const bool supported, const bool enabled) noexcept {
        if (const auto check = Check(false); check != InputAdmission::Accepted)
            return check;
        if (Index(sensor) >= sensors_.size())
            return InputAdmission::Invalid;
        failed_[Index(sensor)] = false;
        supported_[Index(sensor)] = supported;
        enabled_[Index(sensor)] = enabled;
        RefreshSensors();
        return InputAdmission::Accepted;
    }

    /** @copydoc AndroidInputSession::FailSensor */
    InputAdmission AndroidInputSession::FailSensor(const SensorKind sensor) noexcept {
        if (const auto check = Check(false); check != InputAdmission::Accepted)
            return check;
        if (Index(sensor) >= sensors_.size())
            return InputAdmission::Invalid;
        failed_[Index(sensor)] = true;
        RefreshSensors();
        return InputAdmission::Accepted;
    }

    /** @copydoc AndroidInputSession::Sample */
    InputAdmission AndroidInputSession::Sample(const SensorKind sensor, const std::array<float, 3> value,
                                               const std::int64_t timestampNanoseconds, const std::uint64_t counter) noexcept {
        if (const auto check = Check(); check != InputAdmission::Accepted)
            return check;
        if (Index(sensor) >= sensors_.size())
            return InputAdmission::Invalid;
        auto &sample = sensors_[Index(sensor)];
        if (sample.availability != SensorAvailability::Available && sample.availability != SensorAvailability::AwaitingSample)
            return InputAdmission::Unavailable;
        const SensorSample next{SensorAvailability::Available, value, counter, timestampNanoseconds};
        if (!collector_.SetSensorSample(sensor, next))
            return InputAdmission::Invalid;
        sample = next;
        return InputAdmission::Accepted;
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
