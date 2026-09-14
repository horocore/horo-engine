#include "Horo/WorldStreaming/WorldStreamingErrors.h"

namespace Horo::WorldStreaming::WorldStreamingErrors {
    namespace {
        [[nodiscard]] ErrorCodeDescriptor TraceError(const char *code, const ErrorSeverity severity, const char *summary,
                                                     const char *remediation, const bool actionable = false) {
            return {.domain = ErrorDomainId{"horo.world_streaming"},
                    .code = ErrorCode{code},
                    .defaultSeverity = severity,
                    .summary = summary,
                    .remediationHint = remediation,
                    .retryable = false,
                    .userActionable = actionable};
        }
    }  // namespace

    const ErrorCodeDescriptor TraceInvalid =
        TraceError("world_streaming.trace.invalid", ErrorSeverity::Error,
                   "A World Streaming trace binding, stage, subject, parent, or terminal request is malformed.",
                   "Supply complete typed identities and exact root parentage for the requested stage.", true);
    const ErrorCodeDescriptor TraceUnsupported =
        TraceError("world_streaming.trace.unsupported", ErrorSeverity::Error,
                   "A World Streaming trace stage or terminal status is unsupported.",
                   "Use only stages and terminal statuses declared by this trace contract version.", true);
    const ErrorCodeDescriptor TraceStale = TraceError("world_streaming.trace.stale", ErrorSeverity::Warning,
                                                      "A World Streaming trace command no longer matches the active binding revision.",
                                                      "Capture the current binding revision at the authority boundary before retrying.");
    const ErrorCodeDescriptor TraceIdentityConflict =
        TraceError("world_streaming.trace.identity_conflict", ErrorSeverity::Error,
                   "A World Streaming trace repeats a span identity or names an unavailable parent.",
                   "Issue a unique span identity and parent it to the root or an earlier admitted stage.", true);
    const ErrorCodeDescriptor TraceCapacityExceeded = TraceError("world_streaming.trace.capacity_exceeded", ErrorSeverity::Warning,
                                                                 "A World Streaming trace reached its mandatory lifetime span ceiling.",
                                                                 "Start a successor bounded trace instead of growing the active binding.");
    const ErrorCodeDescriptor TraceLifecycleUnavailable =
        TraceError("world_streaming.trace.lifecycle_unavailable", ErrorSeverity::Warning,
                   "A World Streaming trace operation is unavailable in the current lifecycle.",
                   "Complete active stages before replacement or create a new trace after shutdown.");
    const ErrorCodeDescriptor TraceStorageUnavailable =
        TraceError("world_streaming.trace.storage_unavailable", ErrorSeverity::Error,
                   "Storage required for a World Streaming trace binding is unavailable.",
                   "Reduce the admitted span ceiling or restore process memory before retrying.");
    const ErrorCodeDescriptor TraceThreadAffinityViolation =
        TraceError("world_streaming.trace.thread_affinity_violation", ErrorSeverity::Error,
                   "A World Streaming trace mutation ran outside its declaring authority owner thread.",
                   "Route begin, completion, replacement and shutdown through the bound authority thread.");
}  // namespace Horo::WorldStreaming::WorldStreamingErrors
