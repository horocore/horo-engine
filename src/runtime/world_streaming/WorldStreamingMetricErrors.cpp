#include "Horo/WorldStreaming/WorldStreamingErrors.h"

namespace Horo::WorldStreaming::WorldStreamingErrors {
    namespace {
        const ErrorDomainId Domain{"horo.world_streaming"};

        /** @brief Builds an immutable metric descriptor in the World Streaming error domain. */
        [[nodiscard]] ErrorCodeDescriptor Describe(const char *code, const ErrorSeverity severity, const char *summary,
                                                   const char *remediationHint, const bool userActionable) {
            return {.domain = Domain,
                    .code = ErrorCode{code},
                    .defaultSeverity = severity,
                    .summary = summary,
                    .remediationHint = remediationHint,
                    .retryable = false,
                    .userActionable = userActionable};
        }
    }  // namespace

    const ErrorCodeDescriptor MetricInvalid =
        Describe("world_streaming.metric.invalid", ErrorSeverity::Error,
                 "A World Streaming metric binding, policy, handle set or sample is malformed.",
                 "Publish one complete finite owner-safe-point sample under a valid bounded binding.", true);
    const ErrorCodeDescriptor MetricUnsupported =
        Describe("world_streaming.metric.unsupported", ErrorSeverity::Error, "A World Streaming metric contract value is unsupported.",
                 "Use only the availability, requirement, collection level and lifecycle values declared by this contract version.", true);
    const ErrorCodeDescriptor MetricStale =
        Describe("world_streaming.metric.stale", ErrorSeverity::Warning,
                 "A World Streaming metric sample or replacement no longer matches the active owner or revisions.",
                 "Capture the current owner, composition revision and binding revision together at the authority safe point.", false);
    const ErrorCodeDescriptor MetricCapacityExceeded =
        Describe("world_streaming.metric.capacity_exceeded", ErrorSeverity::Warning,
                 "A World Streaming metric sample exceeds an admitted measurement maximum.",
                 "Correct the producer snapshot or admit a larger supported bound before publishing another sample.", false);
    const ErrorCodeDescriptor MetricCapabilityUnavailable =
        Describe("world_streaming.metric.capability_unavailable", ErrorSeverity::Warning,
                 "Required World Streaming metric collection is unavailable from host composition.",
                 "Initialize Telemetry and provide the complete pre-bound metric handle family before activating the owner.", false);
    const ErrorCodeDescriptor MetricLifecycleUnavailable =
        Describe("world_streaming.metric.lifecycle_unavailable", ErrorSeverity::Warning,
                 "World Streaming metric publication is closed by cancellation or shutdown.",
                 "Create a new binding for a new mounted owner lifetime instead of reopening the terminal binding.", false);
    const ErrorCodeDescriptor MetricThreadAffinityViolation =
        Describe("world_streaming.metric.thread_affinity_violation", ErrorSeverity::Error,
                 "A World Streaming metric-binding operation ran outside its declaring authority owner thread.",
                 "Route publication, replacement, cancellation and close through the bound StreamingAuthorityRole.", false);
}  // namespace Horo::WorldStreaming::WorldStreamingErrors
