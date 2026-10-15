#include "AndroidGameActivityInput.h"

#include <algorithm>
#include <android/input.h>
#include <bit>
#include <cstdint>
#include <time.h>

namespace Horo::Platform::Android {
    /** @brief Compose native sensor discovery and private Java permission callbacks on the application owner. */
    GameActivityInput::GameActivityInput(android_app &app, Input::RawInputCollector &collector, const std::array<bool, 3> admittedSensors)
        : app_(app), session_(collector) {
        if (!BindJava())
            return;
        const auto type = env_->GetObjectClass(app_.activity->javaGameActivity);
        const auto nameMethod = env_->GetMethodID(type, "getPackageName", "()Ljava/lang/String;");
        const auto package = nameMethod && !env_->ExceptionCheck()
                                 ? static_cast<jstring>(env_->CallObjectMethod(app_.activity->javaGameActivity, nameMethod))
                                 : nullptr;
        if (package && !env_->ExceptionCheck()) {
            const char *name = env_->GetStringUTFChars(package, nullptr);
            if (name) {
                manager_ = ASensorManager_getInstanceForPackage(name);
                env_->ReleaseStringUTFChars(package, name);
            }
        }
        env_->DeleteLocalRef(package);
        env_->DeleteLocalRef(type);
        if (env_->ExceptionCheck())
            env_->ExceptionClear();
        if (manager_)
            queue_ = ASensorManager_createEventQueue(manager_, app_.looper, LOOPER_ID_USER + 1, nullptr, nullptr);
        constexpr std::array types{ASENSOR_TYPE_ACCELEROMETER, ASENSOR_TYPE_GYROSCOPE, ASENSOR_TYPE_STEP_COUNTER};
        for (std::size_t i = 0; i < types.size(); ++i) {
            sensors_[i] = manager_ ? ASensorManager_getDefaultSensor(manager_, types[i]) : nullptr;
            static_cast<void>(session_.ConfigureSensor(static_cast<SensorKind>(i), sensors_[i] != nullptr, admittedSensors[i]));
            if (admittedSensors[i] && (!manager_ || (sensors_[i] && !queue_)))
                static_cast<void>(session_.FailSensor(static_cast<SensorKind>(i)));
        }
        for (const auto axis : {AMOTION_EVENT_AXIS_Z, AMOTION_EVENT_AXIS_RZ, AMOTION_EVENT_AXIS_LTRIGGER, AMOTION_EVENT_AXIS_RTRIGGER,
                                AMOTION_EVENT_AXIS_HAT_X, AMOTION_EVENT_AXIS_HAT_Y})
            GameActivityPointerAxes_enableAxis(axis);
        android_app_set_motion_event_filter(&app_, [](const GameActivityMotionEvent *event) {
            return (event->source & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN ||
                   (event->source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK;
        });
        status_ = InputAdmission::Accepted;
    }

    /** @brief Stop and detach Java producers before releasing native callback storage or sensors. */
    GameActivityInput::~GameActivityInput() {
        if (env_ && detach_) {
            env_->CallVoidMethod(app_.activity->javaGameActivity, detach_, reinterpret_cast<jlong>(this));
            if (env_->ExceptionCheck())
                env_->ExceptionClear();
        }
        session_.Shutdown();
        if (queue_) {
            for (const auto *sensor : sensors_)
                if (sensor)
                    ASensorEventQueue_disableSensor(queue_, sensor);
            ASensorManager_destroyEventQueue(manager_, queue_);
        }
        if (attached_)
            app_.activity->vm->DetachCurrentThread();
    }

    /** @brief Register a per-Activity callback without a process-global native pointer. */
    bool GameActivityInput::BindJava() noexcept {
        auto *vm = app_.activity->vm;
        void *environment{};
        const auto state = vm->GetEnv(&environment, JNI_VERSION_1_6);
        if (state == JNI_EDETACHED) {
            if (vm->AttachCurrentThread(&env_, nullptr) != JNI_OK)
                return false;
            attached_ = true;
        } else if (state == JNI_OK)
            env_ = static_cast<JNIEnv *>(environment);
        if (!env_)
            return false;
        jclass type = env_->GetObjectClass(app_.activity->javaGameActivity);
        if (!type)
            return false;
        const JNINativeMethod callback{const_cast<char *>("horoInputEvent"), const_cast<char *>("(JII)V"),
                                       reinterpret_cast<void *>(&Callback)};
        const auto bind = env_->GetMethodID(type, "horoBindInput", "(J)V");
        detach_ = env_->GetMethodID(type, "horoDetachInput", "(J)V");
        request_ = env_->GetMethodID(type, "horoRequestActivityRecognition", "()V");
        const bool registered =
            !env_->ExceptionCheck() && bind && detach_ && request_ && env_->RegisterNatives(type, &callback, 1) == JNI_OK;
        env_->DeleteLocalRef(type);
        if (registered)
            env_->CallVoidMethod(app_.activity->javaGameActivity, bind, reinterpret_cast<jlong>(this));
        const bool failed = env_->ExceptionCheck();
        if (failed)
            env_->ExceptionClear();
        return registered && !failed;
    }

    /** @brief Copy synchronized Java producer observations into a finite SPSC queue. */
    void GameActivityInput::Callback(JNIEnv *, jobject, const jlong bridge, const jint kind, const jint value) noexcept {
        if (!bridge)
            return;
        auto &self = *reinterpret_cast<GameActivityInput *>(bridge);
        const auto written = self.written_.load(std::memory_order_relaxed);
        if (written - self.read_.load(std::memory_order_acquire) >= self.messages_.size()) {
            self.overflow_.store(true, std::memory_order_release);
            return;
        }
        self.messages_[written % self.messages_.size()] = {kind, value};
        self.written_.store(written + 1, std::memory_order_release);
    }

    /** @brief Consume permission and removal changes before any new native input/sensor sample. */
    void GameActivityInput::DrainMessages() noexcept {
        if (overflow_.load(std::memory_order_acquire)) {
            status_ = InputAdmission::Capacity;
            session_.Shutdown();
            return;
        }
        auto read = read_.load(std::memory_order_relaxed);
        const auto written = written_.load(std::memory_order_acquire);
        while (read != written) {
            const auto event = messages_[read % messages_.size()];
            if (event.kind == 1) {
                const auto permission = static_cast<PermissionState>(event.value);
                const bool lostGrant = session_.Permission() == PermissionState::Granted && permission != PermissionState::Granted;
                static_cast<void>(session_.SetPermission(permission));
                if (lostGrant) {
                    touches_ = {};
                    timespec now{};
                    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
                        minimumInputTimestamp_ = now.tv_sec * 1'000'000'000LL + now.tv_nsec;
                    if (clock_gettime(CLOCK_BOOTTIME, &now) == 0)
                        minimumSensorTimestamp_.fill(now.tv_sec * 1'000'000'000LL + now.tv_nsec);
                }
                UpdateSensors();
            } else if (event.kind == 2) {
                static_cast<void>(session_.DisconnectGamepad(event.value));
                static_cast<void>(session_.SetFocus(false, false));
                static_cast<void>(session_.SetFocus(resumed_, focused_));
                touches_ = {};
                timespec now{};
                if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
                    minimumInputTimestamp_ = now.tv_sec * 1'000'000'000LL + now.tv_nsec;
                if (clock_gettime(CLOCK_BOOTTIME, &now) == 0)
                    minimumSensorTimestamp_.fill(now.tv_sec * 1'000'000'000LL + now.tv_nsec);
            }
            ++read;
        }
        read_.store(read, std::memory_order_release);
    }

    /** @copydoc GameActivityInput::RequestStepPermission */
    bool GameActivityInput::RequestStepPermission() noexcept {
        if (status_ != InputAdmission::Accepted || !request_)
            return false;
        static_cast<void>(session_.ConfigureSensor(SensorKind::StepCounter, sensors_[2] != nullptr, true));
        env_->CallVoidMethod(app_.activity->javaGameActivity, request_);
        if (!env_->ExceptionCheck())
            return true;
        env_->ExceptionClear();
        static_cast<void>(session_.SetPermission(PermissionState::Denied));
        return false;
    }

    /** @copydoc GameActivityInput::Command */
    void GameActivityInput::Command(const std::int32_t command) noexcept {
        const bool previouslyActive = resumed_ && focused_;
        switch (command) {
            case APP_CMD_RESUME:
                resumed_ = true;
                break;
            case APP_CMD_PAUSE:
            case APP_CMD_STOP:
                resumed_ = false;
                break;
            case APP_CMD_GAINED_FOCUS:
                focused_ = true;
                break;
            case APP_CMD_LOST_FOCUS:
            case APP_CMD_TERM_WINDOW:
                focused_ = false;
                break;
            case APP_CMD_DESTROY:
                session_.Shutdown();
                status_ = InputAdmission::Stopped;
                break;
            default:
                return;
        }
        if (!previouslyActive && resumed_ && focused_) {
            timespec now{};
            if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
                minimumInputTimestamp_ = now.tv_sec * 1'000'000'000LL + now.tv_nsec;
        }
        static_cast<void>(session_.SetFocus(resumed_, focused_));
        if (!resumed_ || !focused_)
            touches_ = {};
        UpdateSensors();
    }

    /** @copydoc GameActivityInput::Pump */
    void GameActivityInput::Pump() noexcept {
        DrainMessages();
        auto *input = android_app_swap_input_buffers(&app_);
        if (input) {
            if (input->keyEventsCount > 512 || input->motionEventsCount > 512 - input->keyEventsCount) {
                status_ = InputAdmission::Capacity;
                session_.Shutdown();
            } else if (status_ == InputAdmission::Accepted) {
                for (std::uint64_t i = 0; i < input->keyEventsCount; ++i)
                    KeyEvent(input->keyEvents[i]);
                for (std::uint64_t i = 0; i < input->motionEventsCount; ++i)
                    MotionEvent(input->motionEvents[i]);
            }
            android_app_clear_key_events(input);
            android_app_clear_motion_events(input);
        }
        UpdateSensors();
        DrainSensors();
    }
}  // namespace Horo::Platform::Android
