#pragma once
/** @file AndroidInput.h
 * @brief Owner-thread Android input projection and permission-scoped sensor samples, without native handles.
 */
#include "Horo/Runtime/Input.h"

#include <array>
#include <cstdint>
#include <thread>

namespace Horo::Platform::Android {
    /** @brief Typed collection admission; unavailable input never changes the collector. */
    enum class InputAdmission : std::uint8_t {
        Accepted,
        Invalid,
        Unavailable,
        Stale,
        Capacity,
        WrongThread,
        Stopped
    };
    /** @brief Runtime Activity Recognition permission state, including settings-return revocation. */
    enum class PermissionState : std::uint8_t {
        NotRequested,
        Requesting,
        Granted,
        Denied,
        DeniedPermanently,
        Revoked
    };
    using SensorKind = Input::SensorKind;
    using SensorAvailability = Input::SensorAvailability;
    using SensorSample = Input::SensorSample;

    /** @brief Fixed-state Android adapter over the canonical RawInputCollector. No backend/native object escapes.
     * @details Host serializes all calls after collector BeginFrame and before Commit. Connect/disconnect are
     * device-lifecycle operations; routine projection allocates no storage. Shutdown and destruction occur on the owner thread before
     * collector destruction.
     */
    class AndroidInputSession final {
    public:
        /** @brief Bind one host-owned collector. @param collector Owner-thread collector outliving this session. */
        explicit AndroidInputSession(Input::RawInputCollector &collector) noexcept;
        ~AndroidInputSession();
        AndroidInputSession(const AndroidInputSession &) = delete;
        AndroidInputSession &operator=(const AndroidInputSession &) = delete;
        /** @brief Apply activity/window focus; loss immediately neutralizes controls and sensor evidence. @param resumed Activity is
         * resumed. @param focused Its window owns focus. @return Typed admission. */
        [[nodiscard]] InputAdmission SetFocus(bool resumed, bool focused) noexcept;
        /** @brief Apply current Activity Recognition grant; loss neutralizes controls and prior sensor evidence before publication.
         * @param state Current platform-observed grant state.
         * @return Typed admission. */
        [[nodiscard]] InputAdmission SetPermission(PermissionState state) noexcept;
        /** @brief Project a normalized physical key. @param key Canonical physical key. @param down Held state. @return Typed admission. */
        [[nodiscard]] InputAdmission Key(Input::Key key, bool down) noexcept;
        /** @brief Project physical modifier evidence only while focus is admitted. @param modifiers Copied physical modifier bits. @return
         * Admission. */
        [[nodiscard]] InputAdmission Modifiers(Input::ModifierState modifiers) noexcept;
        /** @brief Project one host-incarnated touch contact. @param id Host-issued contact incarnation. @param x Finite local X. @param y
         * Finite local Y. @param down Held state. @return Typed collection outcome. */
        [[nodiscard]] InputAdmission Touch(Input::TouchContactId id, float x, float y, bool down) noexcept;
        /** @brief Cancel current touches without synthesizing an activating release. */
        void CancelTouches() noexcept;
        /** @brief Admit one canonical gamepad at a device lifecycle safe point, up to four live devices. @param nativeDevice Host-local
         * physical source identity. @return Admission. */
        [[nodiscard]] InputAdmission ConnectGamepad(std::int32_t nativeDevice);
        /** @brief Retire one gamepad incarnation; stale IDs never affect a replacement. @param nativeDevice Exact physical source to
         * retire. @return Admission. */
        [[nodiscard]] InputAdmission DisconnectGamepad(std::int32_t nativeDevice);
        /** @brief Project one canonical gamepad button. @param nativeDevice Admitted physical source. @param button Canonical button.
         * @param down Held state. @return Admission. */
        [[nodiscard]] InputAdmission Button(std::int32_t nativeDevice, Input::GamepadButton button, bool down) noexcept;
        /** @brief Merge a physical direction hat with independently held D-pad keys. @param nativeDevice Admitted source.
         * @param x Finite horizontal hat value. @param y Finite vertical hat value. @return Admission. */
        [[nodiscard]] InputAdmission Hat(std::int32_t nativeDevice, float x, float y) noexcept;
        /** @brief Project one canonical finite normalized axis. @param nativeDevice Admitted physical source. @param axis Canonical axis.
         * @param value Finite normalized value, clamped to [-1,1]. @return Admission. */
        [[nodiscard]] InputAdmission Axis(std::int32_t nativeDevice, Input::GamepadAxis axis, float value) noexcept;
        /** @brief Declare actual native sensor support and explicit host admission. @param sensor Canonical sensor. @param supported Actual
         * native support. @param enabled Explicit host policy admission. @return Admission. */
        [[nodiscard]] InputAdmission ConfigureSensor(SensorKind sensor, bool supported, bool enabled) noexcept;
        /** @brief Close sensor publication after a typed native enable/read failure. @param sensor Canonical sensor whose native operation
         * failed. @return Admission. */
        [[nodiscard]] InputAdmission FailSensor(SensorKind sensor) noexcept;
        /** @brief Publish one fresh finite sample; gated by focus, policy and permission. @param sensor Canonical source. @param value
         * Finite SI motion components. @param timestampNanoseconds Positive monotonic source time. @param counter Exact step count for
         * StepCounter. @return Admission. */
        [[nodiscard]] InputAdmission Sample(SensorKind sensor, std::array<float, 3> value, std::int64_t timestampNanoseconds,
                                            std::uint64_t counter = 0) noexcept;
        /** @brief Read owned current sensor evidence. @param sensor Canonical source. @return Value-only snapshot; no native/source
         * pointer. */
        [[nodiscard]] SensorSample Sensor(SensorKind sensor) const noexcept;
        /** @brief Return current permission state. @return Copied state. */
        [[nodiscard]] PermissionState Permission() const noexcept;
        /** @brief Stop publication and neutralize all input before native bridge/collector retirement. */
        void Shutdown() noexcept;

    private:
        struct Pad {
            std::int32_t nativeDevice{-1};
            Input::GamepadDeviceId id{};
            std::array<bool, 4> directionKeys{};
            std::array<bool, 4> directionHat{};
        };

        [[nodiscard]] InputAdmission Check(bool requireFocus = true) const noexcept;
        void RefreshSensors() noexcept;
        Input::RawInputCollector &collector_;
        std::thread::id owner_{std::this_thread::get_id()};
        std::array<Pad, 4> pads_{};
        std::array<SensorSample, 3> sensors_{};
        std::array<bool, 3> supported_{};
        std::array<bool, 3> enabled_{};
        std::array<bool, 3> failed_{};
        PermissionState permission_{PermissionState::NotRequested};
        bool focused_{};
        bool stopped_{};
    };
}  // namespace Horo::Platform::Android
