#include "OpenGLRenderBackendErrors.h"

namespace Horo::Render::OpenGLBackendErrors {
    namespace {
        const ErrorDomainId Domain{"horo.render.opengl"};
    }  // namespace

    const ErrorCodeDescriptor AlreadyInitialized{.domain = Domain,
                                                 .code = ErrorCode{"render.backend.already_initialized"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "OpenGL backend is already initialized.",
                                                 .remediationHint = "Shut down the backend before initializing it again.",
                                                 .retryable = false,
                                                 .userActionable = false};

    const ErrorCodeDescriptor FrameActive{.domain = Domain,
                                          .code = ErrorCode{"render.backend.frame_active"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "An OpenGL frame is active.",
                                          .remediationHint = "Complete or abort the frame before this operation.",
                                          .retryable = false,
                                          .userActionable = false};

    const ErrorCodeDescriptor FrameAlreadyActive{.domain = Domain,
                                                 .code = ErrorCode{"render.backend.frame_already_active"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "An OpenGL frame is already active.",
                                                 .remediationHint = "Complete or abort the frame before beginning another.",
                                                 .retryable = false,
                                                 .userActionable = false};

    const ErrorCodeDescriptor FrameTokenExhausted{.domain = Domain,
                                                  .code = ErrorCode{"render.backend.frame_token_exhausted"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "OpenGL frame token space was exhausted.",
                                                  .remediationHint = "Restart the backend after queued work is retired.",
                                                  .retryable = false,
                                                  .userActionable = false};

    const ErrorCodeDescriptor FrameTokenMismatch{.domain = Domain,
                                                 .code = ErrorCode{"render.backend.frame_token_mismatch"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "OpenGL frame token does not match the active frame.",
                                                 .remediationHint = "Use the token returned by the current BeginFrame call.",
                                                 .retryable = false,
                                                 .userActionable = false};

    const ErrorCodeDescriptor InvalidConfig{.domain = Domain,
                                            .code = ErrorCode{"render.backend.invalid_config"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "OpenGL configuration is invalid.",
                                            .remediationHint = "Use a supported frames-in-flight and presentation configuration.",
                                            .retryable = false,
                                            .userActionable = false};

    const ErrorCodeDescriptor InvalidExecutionPlan{.domain = Domain,
                                                   .code = ErrorCode{"render.backend.invalid_execution_plan"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "OpenGL execution plan is invalid.",
                                                   .remediationHint = "Submit a valid ordered pass plan.",
                                                   .retryable = false,
                                                   .userActionable = false};

    const ErrorCodeDescriptor InvalidExtent{.domain = Domain,
                                            .code = ErrorCode{"render.backend.invalid_extent"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "OpenGL render extent is invalid.",
                                            .remediationHint = "Use a non-zero supported render extent.",
                                            .retryable = false,
                                            .userActionable = false};

    const ErrorCodeDescriptor InvalidFrameDescriptor{.domain = Domain,
                                                     .code = ErrorCode{"render.backend.invalid_frame_descriptor"},
                                                     .defaultSeverity = ErrorSeverity::Error,
                                                     .summary = "OpenGL frame descriptor is invalid.",
                                                     .remediationHint = "Provide a valid frame descriptor.",
                                                     .retryable = false,
                                                     .userActionable = false};

    const ErrorCodeDescriptor NoActiveFrame{.domain = Domain,
                                            .code = ErrorCode{"render.backend.no_active_frame"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "No OpenGL frame is active.",
                                            .remediationHint = "Begin a frame before this operation.",
                                            .retryable = false,
                                            .userActionable = false};

    const ErrorCodeDescriptor NotInitialized{.domain = Domain,
                                             .code = ErrorCode{"render.backend.not_initialized"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "OpenGL backend is not initialized.",
                                             .remediationHint = "Initialize the backend before use.",
                                             .retryable = false,
                                             .userActionable = false};

    const ErrorCodeDescriptor InvalidRegistration{.domain = Domain,
                                                  .code = ErrorCode{"render.opengl.invalid_registration"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "OpenGL backend registration is invalid.",
                                                  .remediationHint = "Provide a valid OpenGL runtime and presentation provider.",
                                                  .retryable = false,
                                                  .userActionable = false};

    const ErrorCodeDescriptor UnsupportedApiFamily{.domain = Domain,
                                                   .code = ErrorCode{"render.opengl.unsupported_api_family"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "The realized context is not desktop OpenGL.",
                                                   .remediationHint = "Select a desktop OpenGL driver; OpenGL ES is not supported.",
                                                   .retryable = false,
                                                   .userActionable = true};

    const ErrorCodeDescriptor UnsupportedVersion{.domain = Domain,
                                                 .code = ErrorCode{"render.opengl.unsupported_version"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "The realized OpenGL context version is below the required version.",
                                                 .remediationHint = "Install a driver that provides desktop OpenGL 4.1 or newer.",
                                                 .retryable = false,
                                                 .userActionable = true};

    const ErrorCodeDescriptor UnsupportedProfile{.domain = Domain,
                                                 .code = ErrorCode{"render.opengl.unsupported_profile"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "The realized OpenGL context is not Core profile.",
                                                 .remediationHint = "Configure the host to create an OpenGL Core profile context.",
                                                 .retryable = false,
                                                 .userActionable = true};

    const ErrorCodeDescriptor MissingRequiredEntryPoints{.domain = Domain,
                                                         .code = ErrorCode{"render.opengl.missing_required_entry_points"},
                                                         .defaultSeverity = ErrorSeverity::Error,
                                                         .summary = "Required OpenGL command entry points are unavailable.",
                                                         .remediationHint = "Update or repair the OpenGL driver installation.",
                                                         .retryable = false,
                                                         .userActionable = true};

    const ErrorCodeDescriptor InvalidCapabilities{.domain = Domain,
                                                  .code = ErrorCode{"render.opengl.invalid_capabilities"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "The OpenGL driver reported invalid baseline limits.",
                                                  .remediationHint = "Update the graphics driver and rerun renderer diagnostics.",
                                                  .retryable = false,
                                                  .userActionable = true};

    const ErrorCodeDescriptor WrongThread{.domain = Domain,
                                          .code = ErrorCode{"render.opengl.wrong_thread"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "OpenGL context work was requested from a non-owner thread.",
                                          .remediationHint = "Dispatch renderer lifecycle and frame work to the context owner thread.",
                                          .retryable = true,
                                          .userActionable = false};

    const ErrorCodeDescriptor PresentationInUse{.domain = Domain,
                                                .code = ErrorCode{"render.opengl.presentation_in_use"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "OpenGL presentation is already in use.",
                                                .remediationHint = "Release the active presentation lease before creating another.",
                                                .retryable = true,
                                                .userActionable = false};

    const ErrorCodeDescriptor UnsupportedPassKind{.domain = Domain,
                                                  .code = ErrorCode{"render.opengl.unsupported_pass_kind"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "OpenGL render pass kind is unsupported.",
                                                  .remediationHint = "Submit only pass kinds supported by OpenGL.",
                                                  .retryable = false,
                                                  .userActionable = false};

    const ErrorCodeDescriptor UnsupportedResourceOperation{.domain = Domain,
                                                           .code = ErrorCode{"render.opengl.unsupported_resource_operation"},
                                                           .defaultSeverity = ErrorSeverity::Error,
                                                           .summary = "OpenGL generic resource realization is not available.",
                                                           .remediationHint =
                                                               "Use a backend version that implements generic renderer resources.",
                                                           .retryable = false,
                                                           .userActionable = false};
}  // namespace Horo::Render::OpenGLBackendErrors
