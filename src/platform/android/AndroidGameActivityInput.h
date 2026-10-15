#pragma once
#include "Horo/Platform/AndroidInput.h"

#include <android/sensor.h>
#include <atomic>
#include <game-activity/native_app_glue/android_native_app_glue.h>

namespace Horo::Platform::Android {
    /** @brief Actual native bridge; all Android/JNI handles remain private to this composition. */
    class GameActivityInput final {
    public:
        explicit GameActivityInput(android_app &app, Input::RawInputCollector &collector, std::array<bool, 3> admittedSensors);
        ~GameActivityInput();
        GameActivityInput(const GameActivityInput &) = delete;
        GameActivityInput &operator=(const GameActivityInput &) = delete;
        /** @brief Drain bounded native buffers/sensors after looper events and before Input Commit. */
        void Pump() noexcept;
        /** @brief Forward actual app-command lifecycle on its owner thread. */
        void Command(std::int32_t command) noexcept;
        /** @brief Explicit optional permission request; never auto-prompts from collection. */
        [[nodiscard]] bool RequestStepPermission() noexcept;

        /** @brief Typed native bridge availability. */
        [[nodiscard]] InputAdmission Status() const noexcept {
            return status_;
        }

    private:
        struct Message {
            std::int32_t kind{};
            std::int32_t value{};
        };

        /** @brief Java synchronized callback is the sole queue producer; Pump is the sole consumer. */
        static void Callback(JNIEnv *, jobject, jlong bridge, jint kind, jint value) noexcept;
        void KeyEvent(const GameActivityKeyEvent &event) noexcept;
        void MotionEvent(const GameActivityMotionEvent &event) noexcept;
        void PadMotion(const GameActivityMotionEvent &event) noexcept;
        bool TouchPoint(const GameActivityMotionEvent &event, std::uint32_t index, std::uint32_t changed) noexcept;
        void DrainMessages() noexcept;
        void UpdateSensors() noexcept;
        void DrainSensors() noexcept;
        bool BindJava() noexcept;
        android_app &app_;
        AndroidInputSession session_;
        JNIEnv *env_{};
        bool attached_{};
        jmethodID detach_{};
        jmethodID request_{};
        std::array<Message, 64> messages_{};
        std::atomic<std::uint32_t> written_{};
        std::atomic<std::uint32_t> read_{};
        std::atomic_bool overflow_{};
        ASensorManager *manager_{};
        ASensorEventQueue *queue_{};
        std::array<const ASensor *, 3> sensors_{};
        std::array<bool, 3> sensorEnabled_{};
        std::array<std::int64_t, 3> minimumSensorTimestamp_{};
        std::int64_t minimumInputTimestamp_{};
        std::array<Input::TouchContactId, 32> touches_{};
        std::uint64_t nextTouchGeneration_{1};
        bool resumed_{};
        bool focused_{};
        InputAdmission status_{InputAdmission::Unavailable};
    };
}  // namespace Horo::Platform::Android
