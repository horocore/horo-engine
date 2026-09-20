#include "NullRenderBackendErrors.h"

namespace Horo::Render::NullBackendErrors {
    namespace {
        const ErrorDomainId Domain{"horo.render"};
    }  // namespace

    const ErrorCodeDescriptor AlreadyInitialized{.domain = Domain,
                                                 .code = ErrorCode{"render.backend.already_initialized"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "Renderer backend is already initialized.",
                                                 .remediationHint = "Shut down the backend before initializing it again.",
                                                 .retryable = false,
                                                 .userActionable = false};

    const ErrorCodeDescriptor FrameActive{.domain = Domain,
                                          .code = ErrorCode{"render.backend.frame_active"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "A renderer frame is active.",
                                          .remediationHint = "Complete or abort the frame before this operation.",
                                          .retryable = false,
                                          .userActionable = false};

    const ErrorCodeDescriptor FrameAlreadyActive{.domain = Domain,
                                                 .code = ErrorCode{"render.backend.frame_already_active"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "A renderer frame is already active.",
                                                 .remediationHint = "Complete or abort the frame before beginning another.",
                                                 .retryable = false,
                                                 .userActionable = false};

    const ErrorCodeDescriptor FrameTokenExhausted{.domain = Domain,
                                                  .code = ErrorCode{"render.backend.frame_token_exhausted"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "Renderer frame token space was exhausted.",
                                                  .remediationHint = "Restart the backend after all queued GPU work is retired.",
                                                  .retryable = false,
                                                  .userActionable = false};

    const ErrorCodeDescriptor FrameTokenMismatch{.domain = Domain,
                                                 .code = ErrorCode{"render.backend.frame_token_mismatch"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "Renderer frame token does not match the active frame.",
                                                 .remediationHint = "Use the token returned by the current BeginFrame call.",
                                                 .retryable = false,
                                                 .userActionable = false};

    const ErrorCodeDescriptor InvalidConfig{.domain = Domain,
                                            .code = ErrorCode{"render.backend.invalid_config"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "Renderer configuration is invalid.",
                                            .remediationHint = "Use a supported frames-in-flight and presentation configuration.",
                                            .retryable = false,
                                            .userActionable = false};

    const ErrorCodeDescriptor InvalidExecutionPlan{.domain = Domain,
                                                   .code = ErrorCode{"render.backend.invalid_execution_plan"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "Renderer execution plan is invalid.",
                                                   .remediationHint = "Submit a valid ordered pass plan.",
                                                   .retryable = false,
                                                   .userActionable = false};

    const ErrorCodeDescriptor InvalidExtent{.domain = Domain,
                                            .code = ErrorCode{"render.backend.invalid_extent"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "Render extent is invalid.",
                                            .remediationHint = "Use a non-zero supported render extent.",
                                            .retryable = false,
                                            .userActionable = false};

    const ErrorCodeDescriptor InvalidFrameDescriptor{.domain = Domain,
                                                     .code = ErrorCode{"render.backend.invalid_frame_descriptor"},
                                                     .defaultSeverity = ErrorSeverity::Error,
                                                     .summary = "Renderer frame descriptor is invalid.",
                                                     .remediationHint = "Provide a valid frame descriptor.",
                                                     .retryable = false,
                                                     .userActionable = false};

    const ErrorCodeDescriptor NoActiveFrame{.domain = Domain,
                                            .code = ErrorCode{"render.backend.no_active_frame"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "No renderer frame is active.",
                                            .remediationHint = "Begin a frame before this operation.",
                                            .retryable = false,
                                            .userActionable = false};

    const ErrorCodeDescriptor NotInitialized{.domain = Domain,
                                             .code = ErrorCode{"render.backend.not_initialized"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "Renderer backend is not initialized.",
                                             .remediationHint = "Initialize the backend before use.",
                                             .retryable = false,
                                             .userActionable = false};

    const ErrorCodeDescriptor UnsupportedPassKind{.domain = Domain,
                                                  .code = ErrorCode{"render.backend.unsupported_pass_kind"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "Render pass kind is unsupported.",
                                                  .remediationHint = "Submit only pass kinds supported by the backend.",
                                                  .retryable = false,
                                                  .userActionable = false};

    const ErrorCodeDescriptor PresentationUnsupported{.domain = Domain,
                                                      .code = ErrorCode{"render.null.presentation_unsupported"},
                                                      .defaultSeverity = ErrorSeverity::Error,
                                                      .summary = "Null renderer does not support presentation.",
                                                      .remediationHint = "Use a presentation-capable backend or disable presentation.",
                                                      .retryable = false,
                                                      .userActionable = false};

    const ErrorCodeDescriptor ResourceInstanceExhausted{.domain = Domain,
                                                        .code = ErrorCode{"render.null.resource_instance_exhausted"},
                                                        .defaultSeverity = ErrorSeverity::Error,
                                                        .summary = "Null renderer resource instance space is exhausted.",
                                                        .remediationHint = "Restart the backend after all resource work is retired.",
                                                        .retryable = false,
                                                        .userActionable = false};

    const ErrorCodeDescriptor UnsupportedResourceOperation{.domain = Domain,
                                                           .code = ErrorCode{"render.null.unsupported_resource_operation"},
                                                           .defaultSeverity = ErrorSeverity::Error,
                                                           .summary = "Null renderer does not support the requested resource contract.",
                                                           .remediationHint =
                                                               "Reduce the resource size or use a supported descriptor combination.",
                                                           .retryable = false,
                                                           .userActionable = false};
}  // namespace Horo::Render::NullBackendErrors
