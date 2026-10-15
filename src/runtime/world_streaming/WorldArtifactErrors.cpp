#include "Horo/WorldStreaming/WorldArtifactErrors.h"

namespace Horo::WorldStreaming::WorldArtifactErrors {
    const ErrorCodeDescriptor Invalid{.domain = ErrorDomainId{"horo.worldstreaming"},
                                      .code = ErrorCode{"world.artifact.invalid"},
                                      .defaultSeverity = ErrorSeverity::Error,
                                      .summary = "The world artifact is malformed or noncanonical.",
                                      .retryable = false,
                                      .userActionable = true};
    const ErrorCodeDescriptor CapacityExceeded{.domain = ErrorDomainId{"horo.worldstreaming"},
                                               .code = ErrorCode{"world.artifact.capacity_exceeded"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "The world artifact exceeds its explicit storage bounds.",
                                               .retryable = false,
                                               .userActionable = true};
    const ErrorCodeDescriptor Unsupported{.domain = ErrorDomainId{"horo.worldstreaming"},
                                          .code = ErrorCode{"world.artifact.unsupported"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "The world artifact schema or encoding capability is unsupported.",
                                          .retryable = false,
                                          .userActionable = true};
    const ErrorCodeDescriptor Cancelled{.domain = ErrorDomainId{"horo.worldstreaming"},
                                        .code = ErrorCode{"world.artifact.cancelled"},
                                        .defaultSeverity = ErrorSeverity::Info,
                                        .summary = "World artifact preparation was cancelled before publication.",
                                        .retryable = true,
                                        .userActionable = false};
    const ErrorCodeDescriptor Stale{.domain = ErrorDomainId{"horo.worldstreaming"},
                                    .code = ErrorCode{"world.artifact.stale"},
                                    .defaultSeverity = ErrorSeverity::Error,
                                    .summary = "The world artifact sibling or source belongs to a different generation.",
                                    .retryable = true,
                                    .userActionable = true};
}  // namespace Horo::WorldStreaming::WorldArtifactErrors
