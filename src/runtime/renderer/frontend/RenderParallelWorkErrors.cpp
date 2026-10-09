#include "RenderParallelWorkErrors.h"

namespace Horo::Render::Detail::ParallelWorkErrors {
    namespace {
        const ErrorDomainId Domain{"render.parallel_work"};
    }

    const ErrorCodeDescriptor InvalidLimits{.domain = Domain,
                                            .code = ErrorCode{"render.parallel_work.invalid_limits"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "Parallel frame capture limits are invalid.",
                                            .remediationHint = "Supply finite non-zero pass and byte limits."};
    const ErrorCodeDescriptor CapacityExceeded{.domain = Domain,
                                               .code = ErrorCode{"render.parallel_work.capacity_exceeded"},
                                               .defaultSeverity = ErrorSeverity::Warning,
                                               .summary = "Parallel frame input exceeds its admitted envelope.",
                                               .remediationHint = "Reduce the captured frame payload before retrying.",
                                               .retryable = true};
    const ErrorCodeDescriptor InvalidPass{.domain = Domain,
                                          .code = ErrorCode{"render.parallel_work.invalid_pass"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "Parallel frame pass metadata is invalid.",
                                          .remediationHint = "Supply unique non-zero pass IDs and one valid workload per pass."};
    const ErrorCodeDescriptor InvalidGeometry{.domain = Domain,
                                              .code = ErrorCode{"render.parallel_work.invalid_geometry"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "Captured scene geometry is invalid.",
                                              .remediationHint = "Supply finite geometry, valid indices, and matching mesh generations."};
    const ErrorCodeDescriptor Cancelled{.domain = Domain,
                                        .code = ErrorCode{"render.parallel_work.cancelled"},
                                        .defaultSeverity = ErrorSeverity::Info,
                                        .summary = "Parallel frame work was cancelled before owner publication.",
                                        .remediationHint = "Begin a new frame if the work is still required."};
    const ErrorCodeDescriptor CaptureFailed{.domain = Domain,
                                            .code = ErrorCode{"render.parallel_work.capture_failed"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "Parallel frame input storage could not be acquired.",
                                            .remediationHint = "Reduce the capture envelope or retry after releasing CPU memory.",
                                            .retryable = true};
}  // namespace Horo::Render::Detail::ParallelWorkErrors
