#include "Horo/WorldStreaming/WorldStreamingErrors.h"

namespace Horo::WorldStreaming::WorldStreamingErrors {
    const ErrorCodeDescriptor OriginShiftPolicyInvalid{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.origin_shift_policy.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "An origin-shift policy, evaluation context or request is malformed.",
        .remediationHint = "Provide valid typed policy, frame and request identities plus bounded positive thresholds.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor OriginShiftPolicyUnsupported{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.origin_shift_policy.unsupported"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "An origin-shift policy version, host mode, requester or lifecycle value is unsupported.",
        .remediationHint = "Use the current contract version and one of the declared typed origin-shift values.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor OriginShiftPolicyStale{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.origin_shift_policy.stale"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "An origin-shift evaluation no longer names the active policy or origin-frame owner.",
        .remediationHint = "Capture the current policy publication and active origin frame before retrying.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor OriginShiftPolicyUnauthorized{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.origin_shift_policy.unauthorized"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The requester cannot initiate an origin shift in the configured host mode.",
        .remediationHint = "Route the request through gameplay, editor preview or network authority allowed by host composition.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor OriginShiftPolicyLifecycleUnavailable{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.origin_shift_policy.lifecycle_unavailable"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "Origin-shift evaluation is cancelling or closed.",
        .remediationHint = "Evaluate only while the owning host policy is active.",
        .retryable = false,
        .userActionable = false,
    };
}  // namespace Horo::WorldStreaming::WorldStreamingErrors
