#include "MetalRenderBackendErrors.h"

namespace Horo::Render::MetalBackendErrors {
    namespace {
        const ErrorDomainId Domain{"horo.render.metal"};
    }  // namespace

    const ErrorCodeDescriptor AlreadyInitialized{.domain = Domain,
                                                 .code = ErrorCode{"render.backend.already_initialized"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "Metal backend is already initialized.",
                                                 .remediationHint = "Shut down the backend before initializing it again.",
                                                 .retryable = false,
                                                 .userActionable = false};

    const ErrorCodeDescriptor AdapterNotFound{.domain = Domain,
                                              .code = ErrorCode{"render.metal.adapter_not_found"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "The selected Metal adapter was not found.",
                                              .remediationHint = "Refresh adapter discovery and select an available device.",
                                              .retryable = true,
                                              .userActionable = true};

    const ErrorCodeDescriptor AdapterUnavailable{.domain = Domain,
                                                 .code = ErrorCode{"render.metal.adapter_unavailable"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "The selected Metal adapter is unavailable.",
                                                 .remediationHint = "Refresh adapter discovery or inspect system GPU availability.",
                                                 .retryable = true,
                                                 .userActionable = true};

    const ErrorCodeDescriptor CommandQueueCreationFailed{.domain = Domain,
                                                         .code = ErrorCode{"render.metal.command_queue_creation_failed"},
                                                         .defaultSeverity = ErrorSeverity::Error,
                                                         .summary = "Metal command queue creation failed.",
                                                         .remediationHint = "Inspect Metal diagnostics and retry device initialization.",
                                                         .retryable = true,
                                                         .userActionable = false};

    const ErrorCodeDescriptor FrameActive{.domain = Domain,
                                          .code = ErrorCode{"render.backend.frame_active"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "A Metal frame is active.",
                                          .remediationHint = "Complete or abort the frame before this operation.",
                                          .retryable = false,
                                          .userActionable = false};

    const ErrorCodeDescriptor FrameAlreadyActive{.domain = Domain,
                                                 .code = ErrorCode{"render.backend.frame_already_active"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "A Metal frame is already active.",
                                                 .remediationHint = "Complete or abort the frame before beginning another.",
                                                 .retryable = false,
                                                 .userActionable = false};

    const ErrorCodeDescriptor FrameTokenExhausted{.domain = Domain,
                                                  .code = ErrorCode{"render.backend.frame_token_exhausted"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "Metal frame token space was exhausted.",
                                                  .remediationHint = "Restart the backend after queued work is retired.",
                                                  .retryable = false,
                                                  .userActionable = false};

    const ErrorCodeDescriptor FrameTokenMismatch{.domain = Domain,
                                                 .code = ErrorCode{"render.backend.frame_token_mismatch"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "Metal frame token does not match the active frame.",
                                                 .remediationHint = "Use the token returned by the current BeginFrame call.",
                                                 .retryable = false,
                                                 .userActionable = false};

    const ErrorCodeDescriptor InvalidConfig{.domain = Domain,
                                            .code = ErrorCode{"render.backend.invalid_config"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "Metal configuration is invalid.",
                                            .remediationHint = "Use a supported frames-in-flight and presentation configuration.",
                                            .retryable = false,
                                            .userActionable = false};

    const ErrorCodeDescriptor InvalidDeviceFacts{.domain = Domain,
                                                 .code = ErrorCode{"render.metal.invalid_device_facts"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "Metal device facts are incomplete.",
                                                 .remediationHint = "Inspect native device queries and driver diagnostics.",
                                                 .retryable = false,
                                                 .userActionable = false};

    const ErrorCodeDescriptor InvalidExecutionPlan{.domain = Domain,
                                                   .code = ErrorCode{"render.backend.invalid_execution_plan"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "Metal execution plan is invalid.",
                                                   .remediationHint = "Submit a valid ordered pass plan.",
                                                   .retryable = false,
                                                   .userActionable = false};

    const ErrorCodeDescriptor InvalidExtent{.domain = Domain,
                                            .code = ErrorCode{"render.backend.invalid_extent"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "Metal render extent is invalid.",
                                            .remediationHint = "Use a non-zero supported render extent.",
                                            .retryable = false,
                                            .userActionable = false};

    const ErrorCodeDescriptor InvalidFrameDescriptor{.domain = Domain,
                                                     .code = ErrorCode{"render.backend.invalid_frame_descriptor"},
                                                     .defaultSeverity = ErrorSeverity::Error,
                                                     .summary = "Metal frame descriptor is invalid.",
                                                     .remediationHint = "Provide a valid frame descriptor.",
                                                     .retryable = false,
                                                     .userActionable = false};

    const ErrorCodeDescriptor NoActiveFrame{.domain = Domain,
                                            .code = ErrorCode{"render.backend.no_active_frame"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "No Metal frame is active.",
                                            .remediationHint = "Begin a frame before this operation.",
                                            .retryable = false,
                                            .userActionable = false};

    const ErrorCodeDescriptor NotInitialized{.domain = Domain,
                                             .code = ErrorCode{"render.backend.not_initialized"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "Metal backend is not initialized.",
                                             .remediationHint = "Initialize the backend before use.",
                                             .retryable = false,
                                             .userActionable = false};

    const ErrorCodeDescriptor PresentationInUse{.domain = Domain,
                                                .code = ErrorCode{"render.metal.presentation_in_use"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "Metal presentation is already in use.",
                                                .remediationHint = "Release the active presentation lease before creating another.",
                                                .retryable = true,
                                                .userActionable = false};

    const ErrorCodeDescriptor PresentationUnsupported{.domain = Domain,
                                                      .code = ErrorCode{"render.metal.presentation_unsupported"},
                                                      .defaultSeverity = ErrorSeverity::Error,
                                                      .summary = "Metal presentation is unsupported by the selected adapter.",
                                                      .remediationHint = "Select an adapter that can present to the host display.",
                                                      .retryable = false,
                                                      .userActionable = true};

    const ErrorCodeDescriptor RequiredFormatUnsupported{.domain = Domain,
                                                        .code = ErrorCode{"render.metal.required_format_unsupported"},
                                                        .defaultSeverity = ErrorSeverity::Error,
                                                        .summary = "A required Metal format combination is unsupported.",
                                                        .remediationHint =
                                                            "Select a qualified adapter or inspect device capability diagnostics.",
                                                        .retryable = false,
                                                        .userActionable = true};

    const ErrorCodeDescriptor ResourceCreationFailed{.domain = Domain,
                                                     .code = ErrorCode{"render.metal.resource_creation_failed"},
                                                     .defaultSeverity = ErrorSeverity::Error,
                                                     .summary = "Metal resource creation failed.",
                                                     .remediationHint = "Reduce resource demand or inspect the Metal device diagnostics.",
                                                     .retryable = true,
                                                     .userActionable = false};

    const ErrorCodeDescriptor ResourceIdentityInvalid{.domain = Domain,
                                                      .code = ErrorCode{"render.metal.resource_identity_invalid"},
                                                      .defaultSeverity = ErrorSeverity::Error,
                                                      .summary = "Metal backend resource identity is invalid.",
                                                      .remediationHint =
                                                          "Use the exact ready resource generations supplied by the frontend.",
                                                      .retryable = false,
                                                      .userActionable = false};

    const ErrorCodeDescriptor UnsupportedFramesInFlight{.domain = Domain,
                                                        .code = ErrorCode{"render.metal.unsupported_frames_in_flight"},
                                                        .defaultSeverity = ErrorSeverity::Error,
                                                        .summary = "Metal frames-in-flight configuration is unsupported.",
                                                        .remediationHint = "Use a frames-in-flight count supported by Metal.",
                                                        .retryable = false,
                                                        .userActionable = false};

    const ErrorCodeDescriptor UnsupportedDeviceFamily{.domain = Domain,
                                                      .code = ErrorCode{"render.metal.unsupported_device_family"},
                                                      .defaultSeverity = ErrorSeverity::Error,
                                                      .summary = "The Metal GPU family is unsupported.",
                                                      .remediationHint =
                                                          "Use an Apple7-or-newer Apple GPU or a Mac2-capable Intel Mac GPU.",
                                                      .retryable = false,
                                                      .userActionable = true};

    const ErrorCodeDescriptor UnsupportedHost{.domain = Domain,
                                              .code = ErrorCode{"render.metal.unsupported_host"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "The host does not satisfy the Metal platform baseline.",
                                              .remediationHint = "Use native macOS 14.0 or later on a supported architecture.",
                                              .retryable = false,
                                              .userActionable = true};

    const ErrorCodeDescriptor UnsupportedPassKind{.domain = Domain,
                                                  .code = ErrorCode{"render.metal.unsupported_pass_kind"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "Metal render pass kind is unsupported.",
                                                  .remediationHint = "Submit only pass kinds supported by Metal.",
                                                  .retryable = false,
                                                  .userActionable = false};

    const ErrorCodeDescriptor UnsupportedResourceOperation{.domain = Domain,
                                                           .code = ErrorCode{"render.metal.unsupported_resource_operation"},
                                                           .defaultSeverity = ErrorSeverity::Error,
                                                           .summary = "Metal generic resource realization is not available.",
                                                           .remediationHint =
                                                               "Use a backend version that implements generic renderer resources.",
                                                           .retryable = false,
                                                           .userActionable = false};

    const ErrorCodeDescriptor StaleAdapterSnapshot{.domain = Domain,
                                                   .code = ErrorCode{"render.metal.stale_adapter_snapshot"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "The Metal adapter discovery snapshot is stale.",
                                                   .remediationHint = "Refresh adapter discovery before initializing the backend.",
                                                   .retryable = true,
                                                   .userActionable = false};
}  // namespace Horo::Render::MetalBackendErrors
