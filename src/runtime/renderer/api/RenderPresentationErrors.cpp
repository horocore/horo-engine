#include "Horo/Runtime/Render/RenderPresentationOutput.h"

namespace Horo::Render::RenderPresentationErrors {
    namespace {
        const ErrorDomainId Domain{"horo.render.presentation"};
    }

    const ErrorCodeDescriptor
        InvalidOutput{.domain = Domain,
                      .code = ErrorCode{"render.presentation.invalid_output"},
                      .defaultSeverity = ErrorSeverity::Error,
                      .summary = "Presentation output intent or evidence is invalid.",
                      .remediationHint = "Supply finite supported encoding values and current nonzero display/configuration revisions.",
                      .retryable = false,
                      .userActionable = false};
    const ErrorCodeDescriptor OutputUnsupported{.domain = Domain,
                                                .code = ErrorCode{"render.presentation.output_unsupported"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "The requested presentation encoding is unsupported.",
                                                .remediationHint =
                                                    "Select an explicitly supported surface encoding; no fallback was selected.",
                                                .retryable = false,
                                                .userActionable = true};
    const ErrorCodeDescriptor
        OutputUnavailable{.domain = Domain,
                          .code = ErrorCode{"render.presentation.output_unavailable"},
                          .defaultSeverity = ErrorSeverity::Error,
                          .summary = "The attached presentation output is unavailable.",
                          .remediationHint =
                              "Keep the output suspended until current host display evidence and native realization are available.",
                          .retryable = true,
                          .userActionable = true};
    const ErrorCodeDescriptor HdrUnavailable{.domain = Domain,
                                             .code = ErrorCode{"render.presentation.hdr_unavailable"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "The current host output cannot realize the required HDR encoding.",
                                             .remediationHint =
                                                 "Refresh actual display/HDR evidence or submit an explicit different output request.",
                                             .retryable = true,
                                             .userActionable = true};
    const ErrorCodeDescriptor
        OutputMismatch{.domain = Domain,
                       .code = ErrorCode{"render.presentation.output_mismatch"},
                       .defaultSeverity = ErrorSeverity::Error,
                       .summary = "Native output or graph encoding differs from the admitted output contract.",
                       .remediationHint =
                           "Prepare graph pixels and native presentation against the same exact output encoding and generation.",
                       .retryable = false,
                       .userActionable = false};
    const ErrorCodeDescriptor
        AcquisitionPending{.domain = Domain,
                           .code = ErrorCode{"render.presentation.acquisition_pending"},
                           .defaultSeverity = ErrorSeverity::Error,
                           .summary = "Native drawable acquisition is still pending.",
                           .remediationHint = "Service host events and poll again without blocking or advancing a presentation frame.",
                           .retryable = true,
                           .userActionable = false};
    const ErrorCodeDescriptor
        DrawableUnavailable{.domain = Domain,
                            .code = ErrorCode{"render.presentation.drawable_unavailable"},
                            .defaultSeverity = ErrorSeverity::Error,
                            .summary = "The native output did not provide a usable drawable.",
                            .remediationHint = "Suspend or recover the output using current host facts; no partial frame was admitted.",
                            .retryable = true,
                            .userActionable = false};
    const ErrorCodeDescriptor
        ThreadAffinityViolation{.domain = Domain,
                                .code = ErrorCode{"render.presentation.wrong_thread"},
                                .defaultSeverity = ErrorSeverity::Error,
                                .summary = "Presentation was accessed outside its declared owner thread.",
                                .remediationHint =
                                    "Publish window/display facts and perform surface operations on their original host thread.",
                                .retryable = false,
                                .userActionable = false};
}  // namespace Horo::Render::RenderPresentationErrors
