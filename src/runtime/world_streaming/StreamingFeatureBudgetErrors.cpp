#include "Horo/WorldStreaming/WorldStreamingErrors.h"

namespace Horo::WorldStreaming::WorldStreamingErrors {
    namespace {
        /** @brief Describes a stable aggregate feature-reservation failure in the existing error domain. */
        [[nodiscard]] ErrorCodeDescriptor Describe(const char *code, const char *summary, const char *remediation) {
            return {.domain = ErrorDomainId{"horo.world_streaming"},
                    .code = ErrorCode{code},
                    .defaultSeverity = ErrorSeverity::Error,
                    .summary = summary,
                    .remediationHint = remediation,
                    .retryable = false,
                    .userActionable = true};
        }
    }  // namespace

    const ErrorCodeDescriptor FeatureBudgetInvalid =
        Describe("world_streaming.feature_budget.invalid",
                 "A feature budget request has incomplete or malformed configuration, identity or peak amounts.",
                 "Supply complete known feature/resource vectors and the exact mounted operation.");
    const ErrorCodeDescriptor FeatureBudgetStale =
        Describe("world_streaming.feature_budget.stale",
                 "A feature budget command names a foreign or superseded owner, policy, revision or reservation.",
                 "Route the exact retained token through its original authority and current context.");
    const ErrorCodeDescriptor FeatureBudgetCapacityExceeded =
        Describe("world_streaming.feature_budget.capacity_exceeded",
                 "A feature budget request exceeds a global or feature slice, or mandatory metadata ceiling.",
                 "Retire accepted work or reserve a supported additional peak before allocation.");
    const ErrorCodeDescriptor FeatureBudgetUnsupported =
        Describe("world_streaming.feature_budget.unsupported", "A feature budget request uses an unknown feature or contract version.",
                 "Use the supported feature partition and contract version.");
    const ErrorCodeDescriptor FeatureBudgetLifecycleUnavailable =
        Describe("world_streaming.feature_budget.lifecycle_unavailable",
                 "Feature admission or replacement is closed, or accepted resources have not retired.",
                 "Drain exact operations, consumer leases and cache retirement acknowledgements first.");
}  // namespace Horo::WorldStreaming::WorldStreamingErrors
