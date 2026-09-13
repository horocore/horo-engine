#include "Horo/Runtime/Render/RenderReadbackErrors.h"

namespace Horo::Render::RenderReadbackErrors {
    namespace {
        const ErrorDomainId Domain{"render.readback"};
    }

    const ErrorCodeDescriptor InvalidConfiguration{.domain = Domain,
                                                   .code = ErrorCode{"render.readback.invalid_configuration"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "Renderer readback configuration is invalid.",
                                                   .remediationHint = "Provide a live renderer identity and finite compatible limits."};
    const ErrorCodeDescriptor InvalidDescriptor{.domain = Domain,
                                                .code = ErrorCode{"render.readback.invalid_descriptor"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "Renderer readback descriptor is invalid.",
                                                .remediationHint = "Use a live source, bounded range, alignment, and positive timeout."};
    const ErrorCodeDescriptor CapacityExceeded{.domain = Domain,
                                               .code = ErrorCode{"render.readback.capacity_exceeded"},
                                               .defaultSeverity = ErrorSeverity::Warning,
                                               .summary = "Renderer readback capacity is exhausted.",
                                               .remediationHint = "Consume or retire prior readbacks before retrying.",
                                               .retryable = true};
    const ErrorCodeDescriptor InvalidRequest{.domain = Domain,
                                             .code = ErrorCode{"render.readback.invalid_request"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "Renderer readback identity is not live in this queue.",
                                             .remediationHint = "Use a request issued by the active renderer readback queue."};
    const ErrorCodeDescriptor InvalidTransition{.domain = Domain,
                                                .code = ErrorCode{"render.readback.invalid_transition"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "Renderer readback lifecycle transition is invalid.",
                                                .remediationHint = "Follow pending, submitted, completion, and acknowledgement order."};
    const ErrorCodeDescriptor ResultPending{.domain = Domain,
                                            .code = ErrorCode{"render.readback.result_pending"},
                                            .defaultSeverity = ErrorSeverity::Info,
                                            .summary = "Renderer readback result is not ready.",
                                            .remediationHint = "Poll later without blocking the renderer owner thread.",
                                            .retryable = true};
    const ErrorCodeDescriptor Cancelled{.domain = Domain,
                                        .code = ErrorCode{"render.readback.cancelled"},
                                        .defaultSeverity = ErrorSeverity::Info,
                                        .summary = "Renderer readback was cancelled.",
                                        .remediationHint = "Discard the acknowledged terminal request."};
    const ErrorCodeDescriptor TimedOut{.domain = Domain,
                                       .code = ErrorCode{"render.readback.timed_out"},
                                       .defaultSeverity = ErrorSeverity::Warning,
                                       .summary = "Renderer readback exceeded its caller-owned deadline.",
                                       .remediationHint = "Retire submitted backend work and discard the terminal request."};
    const ErrorCodeDescriptor MappingSizeMismatch{.domain = Domain,
                                                  .code = ErrorCode{"render.readback.mapping_size_mismatch"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "Renderer readback mapping size does not match its request.",
                                                  .remediationHint = "Map exactly the admitted byte range before publication."};
    const ErrorCodeDescriptor Stopped{.domain = Domain,
                                      .code = ErrorCode{"render.readback.stopped"},
                                      .defaultSeverity = ErrorSeverity::Warning,
                                      .summary = "Renderer readback admission is stopped.",
                                      .remediationHint = "Do not submit new readbacks during shutdown."};
}  // namespace Horo::Render::RenderReadbackErrors
