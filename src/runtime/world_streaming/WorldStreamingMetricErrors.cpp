#include "Horo/WorldStreaming/WorldStreamingErrors.h"

namespace Horo::WorldStreaming {
    namespace WorldStreamingErrors {
        namespace {
            const ErrorDomainId MetricDomain{"horo.world_streaming"};

            /** @brief Builds an immutable metric descriptor in the World Streaming error domain. */
            [[nodiscard]] ErrorCodeDescriptor MetricDescriptor(const char *code, const ErrorSeverity severity, const char *summary,
                                                               const char *remediationHint, const bool userActionable) {
                ErrorCodeDescriptor descriptor;
                descriptor.domain = MetricDomain;
                descriptor.code = ErrorCode{code};
                descriptor.defaultSeverity = severity;
                descriptor.summary = summary;
                descriptor.remediationHint = remediationHint;
                descriptor.retryable = false;
                descriptor.userActionable = userActionable;
                return descriptor;
            }
        }  // namespace

        const ErrorCodeDescriptor MetricInvalid =
            MetricDescriptor("world_streaming.metric.invalid", ErrorSeverity::Error,
                             "A World Streaming metric binding, policy, handle set or sample is malformed.",
                             "Publish one complete finite owner-safe-point sample under a valid bounded binding.", true);
        const ErrorCodeDescriptor MetricUnsupported = MetricDescriptor("world_streaming.metric.unsupported", ErrorSeverity::Error,
                                                                       "A World Streaming metric contract value is unsupported.",
                                                                       "Use only the availability, requirement, collection level and "
                                                                       "lifecycle values declared by this contract version.",
                                                                       true);
        const ErrorCodeDescriptor MetricStale =
            MetricDescriptor("world_streaming.metric.stale", ErrorSeverity::Warning,
                             "A World Streaming metric sample or replacement no longer matches the active owner or revisions.",
                             "Capture the current owner, composition revision and binding revision together at the authority safe point.",
                             false);
        const ErrorCodeDescriptor MetricCapacityExceeded =
            MetricDescriptor("world_streaming.metric.capacity_exceeded", ErrorSeverity::Warning,
                             "A World Streaming metric sample exceeds an admitted measurement maximum.",
                             "Correct the producer snapshot or admit a larger supported bound before publishing another sample.", false);
        const ErrorCodeDescriptor MetricCapabilityUnavailable =
            MetricDescriptor("world_streaming.metric.capability_unavailable", ErrorSeverity::Warning,
                             "Required World Streaming metric collection is unavailable from host composition.",
                             "Initialize Telemetry and provide the complete pre-bound metric handle family before activating the owner.",
                             false);
        const ErrorCodeDescriptor MetricLifecycleUnavailable =
            MetricDescriptor("world_streaming.metric.lifecycle_unavailable", ErrorSeverity::Warning,
                             "World Streaming metric publication is closed by cancellation or shutdown.",
                             "Create a new binding for a new mounted owner lifetime instead of reopening the terminal binding.", false);
        const ErrorCodeDescriptor MetricThreadAffinityViolation =
            MetricDescriptor("world_streaming.metric.thread_affinity_violation", ErrorSeverity::Error,
                             "A World Streaming metric-binding operation ran outside its declaring authority owner thread.",
                             "Route publication, replacement, cancellation and close through the bound StreamingAuthorityRole.", false);
    }  // namespace WorldStreamingErrors
}  // namespace Horo::WorldStreaming
