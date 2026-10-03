#include "Horo/WorldStreaming/WorldStreamingErrors.h"

namespace Horo::WorldStreaming::WorldStreamingErrors {
    const ErrorCodeDescriptor FailurePolicyInvalid{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.failure_policy.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Malformed failure-policy facts or identities.",
        .remediationHint = "Use current bounded authority facts and exact canonical terminal operations.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor FailurePolicyUnsupported{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.failure_policy.unsupported"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Unsupported failure contract version or enum.",
        .remediationHint = "Use current bounded authority facts and exact canonical terminal operations.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor FailurePolicyStale{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.failure_policy.stale"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Stale failure policy, revision, generation or clock.",
        .remediationHint = "Use current bounded authority facts and exact canonical terminal operations.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor FailurePolicyCapacityExceeded{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.failure_policy.capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Failure history exceeds its bounded record ceiling.",
        .remediationHint = "Use current bounded authority facts and exact canonical terminal operations.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor FailurePolicyLifecycleUnavailable{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.failure_policy.lifecycle_unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "New retry work is closed during cancellation or shutdown.",
        .remediationHint = "Use current bounded authority facts and exact canonical terminal operations.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor FailurePolicyTransitionInvalid{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.failure_policy.transition_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Failure cleanup or retry eligibility is incomplete.",
        .remediationHint = "Use current bounded authority facts and exact canonical terminal operations.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor FailurePolicyTimeExhausted{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.failure_policy.time_exhausted"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The next monotonic cooldown cannot be represented.",
        .remediationHint = "Use current bounded authority facts and exact canonical terminal operations.",
        .retryable = false,
        .userActionable = false,
    };
}  // namespace Horo::WorldStreaming::WorldStreamingErrors
