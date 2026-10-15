#include "AndroidGameActivityInput.h"

#include <time.h>

namespace Horo::Platform::Android {
    /** @brief Enable only explicitly admitted, focused and permission-authorized native sensors. */
    void GameActivityInput::UpdateSensors() noexcept {
        if (!queue_)
            return;
        for (std::size_t i = 0; i < sensors_.size(); ++i) {
            if (!sensors_[i])
                continue;
            const auto state = session_.Sensor(static_cast<SensorKind>(i)).availability;
            const bool enable = status_ == InputAdmission::Accepted &&
                                (state == SensorAvailability::Available || state == SensorAvailability::AwaitingSample);
            if (enable == sensorEnabled_[i])
                continue;
            if (enable) {
                timespec now{};
                if (clock_gettime(CLOCK_BOOTTIME, &now) != 0) {
                    static_cast<void>(session_.FailSensor(static_cast<SensorKind>(i)));
                    continue;
                }
                minimumSensorTimestamp_[i] = now.tv_sec * 1'000'000'000LL + now.tv_nsec;
            }
            const int result =
                enable ? ASensorEventQueue_enableSensor(queue_, sensors_[i]) : ASensorEventQueue_disableSensor(queue_, sensors_[i]);
            if (result < 0 || (enable && ASensorEventQueue_setEventRate(queue_, sensors_[i], 20'000) < 0)) {
                static_cast<void>(ASensorEventQueue_disableSensor(queue_, sensors_[i]));
                static_cast<void>(session_.FailSensor(static_cast<SensorKind>(i)));
                sensorEnabled_[i] = false;
            } else
                sensorEnabled_[i] = enable;
        }
    }

    /** @brief Sample at most 64 current native events, retaining no sensitive value after loss. */
    void GameActivityInput::DrainSensors() noexcept {
        if (!queue_)
            return;
        for (std::size_t n = 0; n < 64; ++n) {
            if (ASensorEventQueue_hasEvents(queue_) == 0)
                return;
            ASensorEvent event{};
            const auto count = ASensorEventQueue_getEvents(queue_, &event, 1);
            if (count == 0)
                return;
            if (count < 0) {
                for (std::size_t i = 0; i < sensors_.size(); ++i)
                    if (sensors_[i])
                        static_cast<void>(session_.FailSensor(static_cast<SensorKind>(i)));
                return;
            }
            SensorKind kind{SensorKind::Count};
            if (event.type == ASENSOR_TYPE_ACCELEROMETER)
                kind = SensorKind::Accelerometer;
            else if (event.type == ASENSOR_TYPE_GYROSCOPE)
                kind = SensorKind::Gyroscope;
            else if (event.type == ASENSOR_TYPE_STEP_COUNTER)
                kind = SensorKind::StepCounter;
            if (kind == SensorKind::Count || !sensorEnabled_[static_cast<std::size_t>(kind)])
                continue;
            if (event.timestamp <= minimumSensorTimestamp_[static_cast<std::size_t>(kind)])
                continue;
            const bool steps = kind == SensorKind::StepCounter;
            static_cast<void>(
                session_.Sample(kind, steps ? std::array<float, 3>{} : std::array<float, 3>{event.data[0], event.data[1], event.data[2]},
                                event.timestamp, steps ? event.u64.step_counter : 0));
        }
        // Bounded collection continues next frame; timestamps retain exact source evidence.
    }
}  // namespace Horo::Platform::Android
