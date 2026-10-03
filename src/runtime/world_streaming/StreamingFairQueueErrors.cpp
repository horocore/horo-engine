#include "Horo/WorldStreaming/WorldStreamingErrors.h"

namespace Horo::WorldStreaming::WorldStreamingErrors {
    namespace {
        /** @brief Defines stable fair-queue diagnostics in the World Streaming error domain. */
        [[nodiscard]] ErrorCodeDescriptor DescribeFairQueue(const char *code, const char *summary) {
            return {.domain = ErrorDomainId{"horo.world_streaming"},
                    .code = ErrorCode{code},
                    .defaultSeverity = ErrorSeverity::Error,
                    .summary = summary,
                    .remediationHint = "Use current fenced pending work and supported bounded owner policy.",
                    .retryable = false,
                    .userActionable = false};
        }
    }  // namespace

    const ErrorCodeDescriptor FairQueueInvalid =
        DescribeFairQueue("world_streaming.fair_queue.invalid", "A queue owner, context or entry is malformed.");
    const ErrorCodeDescriptor FairQueueUnsupported =
        DescribeFairQueue("world_streaming.fair_queue.unsupported",
                          "A queue contract version, eligibility or withdrawal outcome is unsupported.");
    const ErrorCodeDescriptor FairQueueStale =
        DescribeFairQueue("world_streaming.fair_queue.stale", "A queue command or proposal no longer names current pending work.");
    const ErrorCodeDescriptor FairQueueCapacityExceeded =
        DescribeFairQueue("world_streaming.fair_queue.capacity_exceeded",
                          "The bounded pending queue or admission snapshot exceeds its capacity.");
    const ErrorCodeDescriptor FairQueueIdentityConflict =
        DescribeFairQueue("world_streaming.fair_queue.identity_conflict", "Pending work repeats an operation or cell identity.");
    const ErrorCodeDescriptor FairQueueLifecycleUnavailable =
        DescribeFairQueue("world_streaming.fair_queue.lifecycle_unavailable", "Pending admission is closed by shutdown.");
}  // namespace Horo::WorldStreaming::WorldStreamingErrors
