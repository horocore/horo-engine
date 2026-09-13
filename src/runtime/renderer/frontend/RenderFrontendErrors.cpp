#include "RenderFrontendErrors.h"

namespace Horo::Render::FrontendErrors {
    namespace {
        // Preserve the existing serialized domain until an explicit compatibility migration is approved.
        const ErrorDomainId Domain{"render.frontend"};
    }  // namespace

    const ErrorCodeDescriptor AmbiguousPassWorkload{.domain = Domain,
                                                    .code = ErrorCode{"render.frontend.ambiguous_pass_workload"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "Render pass workload is ambiguous.",
                                                    .remediationHint = "Provide exactly one supported workload for the pass.",
                                                    .retryable = false,
                                                    .userActionable = false};

    const ErrorCodeDescriptor ExecutorChangeDuringFrame{.domain = Domain,
                                                        .code = ErrorCode{"render.frontend.executor_change_during_frame"},
                                                        .defaultSeverity = ErrorSeverity::Error,
                                                        .summary = "Render executor cannot change during an active frame.",
                                                        .remediationHint = "Attach or detach executors between frames.",
                                                        .retryable = false,
                                                        .userActionable = false};

    const ErrorCodeDescriptor FrameAlreadyActive{.domain = Domain,
                                                 .code = ErrorCode{"render.frontend.frame_already_active"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "A render frame is already active.",
                                                 .remediationHint = "Complete or abort the active frame before beginning another.",
                                                 .retryable = false,
                                                 .userActionable = false};

    const ErrorCodeDescriptor FrameAlreadyExecuted{.domain = Domain,
                                                   .code = ErrorCode{"render.frontend.frame_already_executed"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "The active render frame was already executed.",
                                                   .remediationHint = "End the frame or begin a new frame before executing again.",
                                                   .retryable = false,
                                                   .userActionable = false};

    const ErrorCodeDescriptor FrameException{.domain = Domain,
                                             .code = ErrorCode{"render.frontend.frame_exception"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "Render frame execution raised an exception.",
                                             .remediationHint = "Inspect backend diagnostics and abort the failed frame.",
                                             .retryable = true,
                                             .userActionable = false};

    const ErrorCodeDescriptor FrameNotActive{.domain = Domain,
                                             .code = ErrorCode{"render.frontend.frame_not_active"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "No render frame is active.",
                                             .remediationHint = "Begin a frame before issuing this operation.",
                                             .retryable = false,
                                             .userActionable = false};

    const ErrorCodeDescriptor FrameNotExecuted{.domain = Domain,
                                               .code = ErrorCode{"render.frontend.frame_not_executed"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "The active render frame has not executed.",
                                               .remediationHint = "Execute the frame before ending it.",
                                               .retryable = false,
                                               .userActionable = false};

    const ErrorCodeDescriptor InitializeException{.domain = Domain,
                                                  .code = ErrorCode{"render.frontend.initialize_exception"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "Renderer initialization raised an exception.",
                                                  .remediationHint = "Inspect backend initialization diagnostics.",
                                                  .retryable = true,
                                                  .userActionable = false};

    const ErrorCodeDescriptor InvalidFrameToken{.domain = Domain,
                                                .code = ErrorCode{"render.frontend.invalid_frame_token"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "Render frame token is invalid or stale.",
                                                .remediationHint = "Use the token returned for the current active frame.",
                                                .retryable = false,
                                                .userActionable = false};

    const ErrorCodeDescriptor InvalidBufferDescriptor{.domain = Domain,
                                                      .code = ErrorCode{"render.frontend.resource.invalid_buffer_descriptor"},
                                                      .defaultSeverity = ErrorSeverity::Error,
                                                      .summary = "Render buffer descriptor is invalid.",
                                                      .remediationHint = "Provide a non-zero size and valid typed buffer policy.",
                                                      .retryable = false,
                                                      .userActionable = false};

    const ErrorCodeDescriptor InvalidMeshDescriptor{.domain = Domain,
                                                    .code = ErrorCode{"render.frontend.resource.invalid_mesh_descriptor"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "Render mesh descriptor is invalid.",
                                                    .remediationHint = "Provide compatible ready vertex and index buffer generations.",
                                                    .retryable = false,
                                                    .userActionable = false};

    const ErrorCodeDescriptor InvalidRenderTargetDescriptor{.domain = Domain,
                                                            .code = ErrorCode{"render.frontend.resource.invalid_render_target_descriptor"},
                                                            .defaultSeverity = ErrorSeverity::Error,
                                                            .summary = "Render target descriptor is invalid.",
                                                            .remediationHint =
                                                                "Provide compatible ready color and depth texture-view generations.",
                                                            .retryable = false,
                                                            .userActionable = false};

    const ErrorCodeDescriptor InvalidMemoryConfig{.domain = Domain,
                                                  .code = ErrorCode{"render.frontend.memory.invalid_config"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "Render memory admission configuration is invalid.",
                                                  .remediationHint = "Use a finite non-zero budget and a valid default scope.",
                                                  .retryable = false,
                                                  .userActionable = false};

    const ErrorCodeDescriptor InvalidResourceUploadLimits{.domain = Domain,
                                                          .code = ErrorCode{"render.frontend.resource.invalid_upload_limits"},
                                                          .defaultSeverity = ErrorSeverity::Error,
                                                          .summary = "Render resource upload limits are invalid.",
                                                          .remediationHint = "Use finite non-zero queue and drain limits.",
                                                          .retryable = false,
                                                          .userActionable = false};

    const ErrorCodeDescriptor InvalidStaticMeshPass{.domain = Domain,
                                                    .code = ErrorCode{"render.frontend.invalid_static_mesh_pass"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "Static mesh pass descriptor is invalid.",
                                                    .remediationHint = "Provide a valid target, extent, camera, and scene workload.",
                                                    .retryable = false,
                                                    .userActionable = false};

    const ErrorCodeDescriptor InvalidTargetExtent{.domain = Domain,
                                                  .code = ErrorCode{"render.frontend.invalid_target_extent"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "Render target extent is invalid.",
                                                  .remediationHint = "Use a non-zero extent supported by the active backend.",
                                                  .retryable = false,
                                                  .userActionable = false};

    const ErrorCodeDescriptor InvalidTextureDescriptor{.domain = Domain,
                                                       .code = ErrorCode{"render.frontend.resource.invalid_texture_descriptor"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "Render texture descriptor is invalid.",
                                                       .remediationHint = "Provide a supported format, extent, and typed usage policy.",
                                                       .retryable = false,
                                                       .userActionable = false};

    const ErrorCodeDescriptor InvalidTextureViewDescriptor{.domain = Domain,
                                                           .code = ErrorCode{"render.frontend.resource.invalid_texture_view_descriptor"},
                                                           .defaultSeverity = ErrorSeverity::Error,
                                                           .summary = "Render texture-view descriptor is invalid.",
                                                           .remediationHint =
                                                               "Provide a compatible ready texture generation, format, range, and aspect.",
                                                           .retryable = false,
                                                           .userActionable = false};

    const ErrorCodeDescriptor ResizeDuringFrame{.domain = Domain,
                                                .code = ErrorCode{"render.frontend.resize_during_frame"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "Render target cannot resize during an active frame.",
                                                .remediationHint = "Resize the target between frames.",
                                                .retryable = false,
                                                .userActionable = false};

    const ErrorCodeDescriptor ResizeException{.domain = Domain,
                                              .code = ErrorCode{"render.frontend.resize_exception"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "Render target resize raised an exception.",
                                              .remediationHint = "Inspect backend resize diagnostics.",
                                              .retryable = true,
                                              .userActionable = false};

    const ErrorCodeDescriptor ResourceAlreadyRetiring{.domain = Domain,
                                                      .code = ErrorCode{"render.frontend.resource.already_retiring"},
                                                      .defaultSeverity = ErrorSeverity::Error,
                                                      .summary = "Render resource is already retiring.",
                                                      .remediationHint = "Stop issuing new work for the released generation.",
                                                      .retryable = false,
                                                      .userActionable = false};

    const ErrorCodeDescriptor ResourceBackendException{.domain = Domain,
                                                       .code = ErrorCode{"render.frontend.resource.backend_exception"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "Render resource backend operation raised an exception.",
                                                       .remediationHint = "Inspect backend diagnostics and retry with a new generation.",
                                                       .retryable = true,
                                                       .userActionable = false};

    const ErrorCodeDescriptor ResourceBufferUploadSizeMismatch{.domain = Domain,
                                                               .code = ErrorCode{"render.frontend.resource.buffer_upload_size_mismatch"},
                                                               .defaultSeverity = ErrorSeverity::Error,
                                                               .summary = "Render buffer upload size does not match its descriptor.",
                                                               .remediationHint = "Provide exactly the declared number of initial bytes.",
                                                               .retryable = false,
                                                               .userActionable = false};

    const ErrorCodeDescriptor ResourceChangeDuringFrame{.domain = Domain,
                                                        .code = ErrorCode{"render.frontend.resource.change_during_frame"},
                                                        .defaultSeverity = ErrorSeverity::Error,
                                                        .summary = "Render resources cannot change during an active frame.",
                                                        .remediationHint = "Process, replace, or release resources at a frame boundary.",
                                                        .retryable = true,
                                                        .userActionable = false};

    const ErrorCodeDescriptor ResourceBackendInstanceInvalid{.domain = Domain,
                                                             .code = ErrorCode{"render.frontend.resource.backend_instance_invalid"},
                                                             .defaultSeverity = ErrorSeverity::Error,
                                                             .summary = "Render resource backend instance is invalid.",
                                                             .remediationHint = "Publish only a successfully realized backend resource.",
                                                             .retryable = false,
                                                             .userActionable = false};

    const ErrorCodeDescriptor ResourceCapacityExhausted{.domain = Domain,
                                                        .code = ErrorCode{"render.frontend.resource.capacity_exhausted"},
                                                        .defaultSeverity = ErrorSeverity::Error,
                                                        .summary = "Render resource capacity is exhausted.",
                                                        .remediationHint = "Release unused resources or increase the configured capacity.",
                                                        .retryable = true,
                                                        .userActionable = false};

    const ErrorCodeDescriptor ResourceDependencyNotReady{.domain = Domain,
                                                         .code = ErrorCode{"render.frontend.resource.dependency_not_ready"},
                                                         .defaultSeverity = ErrorSeverity::Error,
                                                         .summary = "Render resource dependency is not ready.",
                                                         .remediationHint = "Use ready dependency generations owned by this frontend.",
                                                         .retryable = true,
                                                         .userActionable = false};

    const ErrorCodeDescriptor ResourceHandleMalformed{.domain = Domain,
                                                      .code = ErrorCode{"render.frontend.resource.handle_malformed"},
                                                      .defaultSeverity = ErrorSeverity::Error,
                                                      .summary = "Render resource handle is malformed.",
                                                      .remediationHint = "Use the complete handle returned by the creating frontend.",
                                                      .retryable = false,
                                                      .userActionable = false};

    const ErrorCodeDescriptor ResourceNotPending{.domain = Domain,
                                                 .code = ErrorCode{"render.frontend.resource.not_pending"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "Render resource is not pending.",
                                                 .remediationHint = "Publish or fail only the matching pending generation.",
                                                 .retryable = false,
                                                 .userActionable = false};

    const ErrorCodeDescriptor ResourceNotReady{.domain = Domain,
                                               .code = ErrorCode{"render.frontend.resource.not_ready"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "Render resource is not ready.",
                                               .remediationHint = "Wait for successful publication before using the resource.",
                                               .retryable = true,
                                               .userActionable = false};

    const ErrorCodeDescriptor ResourceOperationPending{.domain = Domain,
                                                       .code = ErrorCode{"render.frontend.resource.operation_pending"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "Render resource operation is pending.",
                                                       .remediationHint = "Poll after the frontend processes the queued request.",
                                                       .retryable = true,
                                                       .userActionable = false};

    const ErrorCodeDescriptor ResourceOperationCancelled{.domain = Domain,
                                                         .code = ErrorCode{"render.frontend.resource.operation_cancelled"},
                                                         .defaultSeverity = ErrorSeverity::Warning,
                                                         .summary = "Render resource operation was cancelled.",
                                                         .remediationHint = "Submit a new generation if the resource is still required.",
                                                         .retryable = true,
                                                         .userActionable = false};

    const ErrorCodeDescriptor ResourceOperationUnknown{.domain = Domain,
                                                       .code = ErrorCode{"render.frontend.resource.operation_unknown"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "Render resource operation is unknown.",
                                                       .remediationHint = "Use an operation identity issued by this frontend registry.",
                                                       .retryable = false,
                                                       .userActionable = false};

    const ErrorCodeDescriptor ResourceOwnerExhausted{.domain = Domain,
                                                     .code = ErrorCode{"render.frontend.resource.owner_exhausted"},
                                                     .defaultSeverity = ErrorSeverity::Critical,
                                                     .summary = "Render resource owner identities are exhausted.",
                                                     .remediationHint = "Restart the process instead of reusing an owner identity.",
                                                     .retryable = false,
                                                     .userActionable = false};

    const ErrorCodeDescriptor ResourceQueueFull{.domain = Domain,
                                                .code = ErrorCode{"render.frontend.resource.queue_full"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "Render resource request queue is full.",
                                                .remediationHint = "Retry after the frontend drains pending resource work.",
                                                .retryable = true,
                                                .userActionable = false};

    const ErrorCodeDescriptor ResourceRegistryStopped{.domain = Domain,
                                                      .code = ErrorCode{"render.frontend.resource.registry_stopped"},
                                                      .defaultSeverity = ErrorSeverity::Error,
                                                      .summary = "Render resource registry is stopped.",
                                                      .remediationHint = "Create resources only while the frontend is running.",
                                                      .retryable = false,
                                                      .userActionable = false};

    const ErrorCodeDescriptor ResourceUploadCapacityExceeded{.domain = Domain,
                                                             .code = ErrorCode{"render.frontend.resource.upload_capacity_exceeded"},
                                                             .defaultSeverity = ErrorSeverity::Error,
                                                             .summary = "Render resource upload arena capacity is exhausted.",
                                                             .remediationHint =
                                                                 "Drain pending uploads before submitting more resource work.",
                                                             .retryable = true,
                                                             .userActionable = false};

    const ErrorCodeDescriptor ResourceUnsupported{.domain = Domain,
                                                  .code = ErrorCode{"render.frontend.resource.unsupported"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "Render resource class is unsupported by the active backend.",
                                                  .remediationHint =
                                                      "Use only resource classes reported by the backend capability snapshot.",
                                                  .retryable = false,
                                                  .userActionable = false};

    const ErrorCodeDescriptor ResourceSlotOutOfRange{.domain = Domain,
                                                     .code = ErrorCode{"render.frontend.resource.slot_out_of_range"},
                                                     .defaultSeverity = ErrorSeverity::Error,
                                                     .summary = "Render resource slot is out of range.",
                                                     .remediationHint = "Use a handle issued by the active frontend.",
                                                     .retryable = false,
                                                     .userActionable = false};

    const ErrorCodeDescriptor ResourceStale{.domain = Domain,
                                            .code = ErrorCode{"render.frontend.resource.stale"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "Render resource generation is stale.",
                                            .remediationHint = "Acquire the current resource generation.",
                                            .retryable = false,
                                            .userActionable = false};

    const ErrorCodeDescriptor ResourceWrongOwner{.domain = Domain,
                                                 .code = ErrorCode{"render.frontend.resource.wrong_owner"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "Render resource belongs to another frontend.",
                                                 .remediationHint = "Resolve the resource through the active frontend.",
                                                 .retryable = false,
                                                 .userActionable = false};

    const ErrorCodeDescriptor ResourceWrongType{.domain = Domain,
                                                .code = ErrorCode{"render.frontend.resource.wrong_type"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "Render resource type does not match its registry entry.",
                                                .remediationHint = "Use the typed handle returned for the requested resource class.",
                                                .retryable = false,
                                                .userActionable = false};

    const ErrorCodeDescriptor StaleRenderTarget{.domain = Domain,
                                                .code = ErrorCode{"render.frontend.stale_render_target"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "Render target handle is stale.",
                                                .remediationHint = "Acquire the current target handle before submitting work.",
                                                .retryable = false,
                                                .userActionable = false};

    const ErrorCodeDescriptor StaticMeshExecutorAlreadyAttached{.domain = Domain,
                                                                .code = ErrorCode{"render.frontend.static_mesh_executor_already_attached"},
                                                                .defaultSeverity = ErrorSeverity::Error,
                                                                .summary = "Static mesh executor is already attached.",
                                                                .remediationHint = "Detach the current executor before attaching another.",
                                                                .retryable = false,
                                                                .userActionable = false};

    const ErrorCodeDescriptor StaticMeshExecutorMissing{.domain = Domain,
                                                        .code = ErrorCode{"render.frontend.static_mesh_executor_missing"},
                                                        .defaultSeverity = ErrorSeverity::Error,
                                                        .summary = "Static mesh executor is not attached.",
                                                        .remediationHint =
                                                            "Attach a compatible executor before submitting static mesh work.",
                                                        .retryable = false,
                                                        .userActionable = false};

    const ErrorCodeDescriptor TargetReleaseDuringFrame{.domain = Domain,
                                                       .code = ErrorCode{"render.frontend.target_release_during_frame"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "Render target cannot be released during an active frame.",
                                                       .remediationHint = "Release targets only after the active frame completes.",
                                                       .retryable = false,
                                                       .userActionable = false};
}  // namespace Horo::Render::FrontendErrors
