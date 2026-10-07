#include "Horo/WorldStreaming/WorldStreamingErrors.h"

namespace Horo::WorldStreaming::WorldStreamingErrors {
    /** @copydoc EvictionPolicyInvalid */
    const ErrorCodeDescriptor EvictionPolicyInvalid{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.eviction.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Eviction policy, authority or candidate facts are malformed.",
        .remediationHint = "Capture a valid bounded snapshot from the current active authority.",
        .retryable = false,
        .userActionable = false,
    };
    /** @copydoc EvictionPolicyUnsupported */
    const ErrorCodeDescriptor EvictionPolicyUnsupported{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.eviction.unsupported"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Eviction contract version or enum is unsupported.",
        .remediationHint = "Capture a valid bounded snapshot from the current active authority.",
        .retryable = false,
        .userActionable = false,
    };
    /** @copydoc EvictionPolicyStale */
    const ErrorCodeDescriptor EvictionPolicyStale{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.eviction.stale"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Eviction facts name a replaced owner, policy or authority snapshot.",
        .remediationHint = "Capture a valid bounded snapshot from the current active authority.",
        .retryable = false,
        .userActionable = false,
    };
    /** @copydoc EvictionPolicyCapacityExceeded */
    const ErrorCodeDescriptor EvictionPolicyCapacityExceeded{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.eviction.capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Eviction snapshot exceeds policy or output capacity.",
        .remediationHint = "Capture a valid bounded snapshot from the current active authority.",
        .retryable = false,
        .userActionable = false,
    };
    /** @copydoc EvictionPolicyIdentityConflict */
    const ErrorCodeDescriptor EvictionPolicyIdentityConflict{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.eviction.identity_conflict"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Eviction snapshot repeats a canonical cell identity.",
        .remediationHint = "Capture a valid bounded snapshot from the current active authority.",
        .retryable = false,
        .userActionable = false,
    };
    /** @copydoc EvictionPolicyLifecycleUnavailable */
    const ErrorCodeDescriptor EvictionPolicyLifecycleUnavailable{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.eviction.lifecycle_unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Eviction selection admission is cancelling or closed.",
        .remediationHint = "Capture a valid bounded snapshot from the current active authority.",
        .retryable = false,
        .userActionable = false,
    };
}  // namespace Horo::WorldStreaming::WorldStreamingErrors
