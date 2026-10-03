#include "Horo/WorldStreaming/WorldStreamingErrors.h"

namespace Horo::WorldStreaming::WorldStreamingErrors {
    /** @copydoc SchedulerConcurrencyUnsupported */
    const ErrorCodeDescriptor SchedulerConcurrencyUnsupported{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.scheduler.concurrency_unsupported"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The requested target profile or operation stage is unsupported or disabled.",
        .remediationHint = "Supply a declared profile and an explicitly enabled operation stage; do not silently fall back.",
        .retryable = false,
        .userActionable = true,
    };

    /** @copydoc SchedulerConcurrencyStale */
    const ErrorCodeDescriptor SchedulerConcurrencyStale{
        .domain = ErrorDomainId{"horo.world_streaming"},
        .code = ErrorCode{"world_streaming.scheduler.concurrency_stale"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The concurrency policy profile or revision does not match the current scheduler publication.",
        .remediationHint = "Capture the current policy revision; replace only the same profile with a strictly newer publication.",
        .retryable = false,
        .userActionable = false,
    };
}  // namespace Horo::WorldStreaming::WorldStreamingErrors
