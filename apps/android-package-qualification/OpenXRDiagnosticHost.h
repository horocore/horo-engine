#pragma once

/** @file OpenXRDiagnosticHost.h
 * @brief Explicit Activity-owned cold OpenXR diagnostic for the signed qualification APK.
 */

#include <cstdint>

struct android_app;

namespace Horo::AndroidQualification {
    /** @brief Queues one diagnostic per Activity and consumes it after pending lifecycle commands. */
    class OpenXRDiagnosticHost final {
    public:
        /** @brief Observes a GameActivity lifecycle command on the application's native lane.
         * @param command Exact native callback command received by the app owner.
         */
        void Command(std::int32_t command) noexcept;

        /** @brief Runs the queued diagnostic only while the same Activity is resumed and live.
         * @param app GameActivity running this host and its JNI lifetime.
         */
        void Pump(android_app &app) noexcept;

    private:
        enum class State {
            Waiting,
            Queued,
            Attempted
        };
        State state_{State::Waiting};
    };
}  // namespace Horo::AndroidQualification
