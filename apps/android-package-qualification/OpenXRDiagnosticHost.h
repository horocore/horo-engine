#pragma once

/** @file OpenXRDiagnosticHost.h
 * @brief Explicit Activity-owned cold OpenXR diagnostic for the signed qualification APK.
 */

#include <cstdint>
#include <memory>

struct android_app;

namespace Horo::XR::OpenXRInternal {
    class AndroidOpenXRColdDiscovery;
}

namespace Horo::AndroidQualification {
    /** @brief Queues one diagnostic per Activity and consumes it after pending lifecycle commands. */
    class OpenXRDiagnosticHost final {
    public:
        /** @brief Begins one Activity diagnostic with no native ownership. */
        OpenXRDiagnosticHost() noexcept;
        /** @brief Releases private storage after the host has polled retained native ownership. */
        ~OpenXRDiagnosticHost();
        OpenXRDiagnosticHost(const OpenXRDiagnosticHost &) = delete;
        OpenXRDiagnosticHost &operator=(const OpenXRDiagnosticHost &) = delete;

        /** @brief Observes a GameActivity lifecycle command on the application's native lane.
         * @param command Exact native callback command received by the app owner.
         */
        void Command(std::int32_t command) noexcept;

        /** @brief Runs the queued diagnostic only while the same Activity is resumed and live.
         * @param app GameActivity running this host and its JNI lifetime.
         */
        void Pump(android_app &app) noexcept;

        /** @brief Retries only the retained instance's native destruction on the Activity lane.
         * @return True after successful native retirement or when there is no ownership debt.
         * @details Retirement can progress after pause, stop or destroy. This never republishes a diagnostic outcome.
         */
        [[nodiscard]] bool PollRetirement() noexcept;

    private:
        /** @brief Records one diagnostic outcome and retains its exact native owner on retirement failure. */
        void Probe(android_app &app);

        enum class State {
            Waiting,
            Queued,
            Attempted
        };
        State state_{State::Waiting};
        std::unique_ptr<XR::OpenXRInternal::AndroidOpenXRColdDiscovery> discovery_;
    };
}  // namespace Horo::AndroidQualification
