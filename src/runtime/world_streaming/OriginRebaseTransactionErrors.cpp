#include "Horo/WorldStreaming/WorldStreamingErrors.h"

namespace Horo::WorldStreaming::WorldStreamingErrors {
    const ErrorCodeDescriptor OriginRebaseInvalid{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.origin_rebase.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "An origin-rebase transaction, decision, participant set or receipt is malformed.",
        .remediationHint = "Provide valid typed identities, exact frame fences and one complete participant set.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor OriginRebaseUnsupported{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.origin_rebase.unsupported"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "An origin-rebase lifecycle, commit point or transaction value is unsupported.",
        .remediationHint = "Use declared values and publish only at the post-simulation host safe point.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor OriginRebaseStale{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.origin_rebase.stale"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "An origin-rebase participant, receipt or active-frame fence is stale.",
        .remediationHint = "Discard prepared state and gather participants against the current origin publication.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor OriginRebaseIncomplete{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.origin_rebase.incomplete"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The complete required origin-rebase participant set was not supplied.",
        .remediationHint = "Provide exactly one current implementation for every host-required participant.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor OriginRebaseCapacityExceeded{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.origin_rebase.capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The required origin-rebase participant set exceeds its mandatory ceiling.",
        .remediationHint = "Reduce the host participant set or raise the explicitly supported ceiling.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor OriginRebaseLifecycleUnavailable{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.origin_rebase.lifecycle_unavailable"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "Origin-rebase preparation or publication is unavailable in the current lifecycle.",
        .remediationHint = "Prepare and publish through one active non-terminal host transaction.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor OriginRebaseSafePointUnavailable{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.origin_rebase.safe_point_unavailable"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "Origin-rebase publication was requested outside the post-simulation host safe point.",
        .remediationHint = "Retain the prepared transaction and retry at the owning host's post-simulation safe point.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor OriginRebaseStorageUnavailable{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.origin_rebase.storage_unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Storage required to gather a complete prepared origin-rebase transaction is unavailable.",
        .remediationHint = "Release transient runtime work and retry the shift from a fresh active-frame observation.",
        .retryable = true,
        .userActionable = false,
    };
}  // namespace Horo::WorldStreaming::WorldStreamingErrors
