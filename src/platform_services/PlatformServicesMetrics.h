#pragma once

#include <cstdint>

namespace Horo::PlatformServices {
    enum class PlatformServiceKind : std::uint8_t;

    namespace Detail {
        enum class PlatformRequestMetricOutcome : std::uint8_t {
            Accepted,
            Rejected,
            Succeeded,
            Failed,
            Cancelled,
            TimedOut,
            Shutdown
        };

        enum class PlatformRequestQueueMetricOutcome : std::uint8_t {
            Accepted,
            CapacityRejected,
            ShutdownRejected,
            InvalidConfiguration
        };

        enum class PlatformCapabilityMetricOutcome : std::uint8_t {
            Available,
            Unavailable,
            NullProvider,
            PolicyDenied,
            FrontendClosed,
            InvalidService
        };

        enum class PlatformSessionMetricOutcome : std::uint8_t {
            Allowed,
            StaleSession,
            StaleAccessPolicy,
            ConsentRequired,
            AccessDenied,
            AccessRestricted,
            AccessRevoked,
            AccessUnavailable,
            Inactive
        };

        enum class PlatformShutdownMetricOutcome : std::uint8_t {
            Succeeded,
            Failed
        };

        void RegisterPlatformServicesMetricDescriptors();
        void RecordPlatformRequestMetric(PlatformRequestMetricOutcome outcome, std::uint64_t delta = 1) noexcept;
        void RecordPlatformRequestQueueMetric(PlatformRequestQueueMetricOutcome outcome) noexcept;
        void RecordPlatformRetryScheduledMetric() noexcept;
        void RecordPlatformThrottledMetric() noexcept;
        void RecordPlatformCapabilityMetric(PlatformServiceKind service, PlatformCapabilityMetricOutcome outcome) noexcept;
        void RecordPlatformSessionMetric(PlatformServiceKind service, PlatformSessionMetricOutcome outcome) noexcept;
        void RecordPlatformShutdownMetric(PlatformShutdownMetricOutcome outcome) noexcept;
    }  // namespace Detail
}  // namespace Horo::PlatformServices
