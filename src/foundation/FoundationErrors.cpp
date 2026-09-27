#include "FoundationErrors.h"

namespace Horo {
    namespace {
        const ErrorDomainId ConfigurationDomain{"horo.configuration"};
        const ErrorDomainId HashingDomain{"horo.foundation.hashing"};
        const ErrorDomainId JobDomain{"horo.foundation.jobs"};
        const ErrorDomainId MathDomain{"horo.foundation.math"};
        const ErrorDomainId ModuleDescriptorDomain{"horo.foundation.modules"};
        const ErrorDomainId ErrorCodeRegistryDomain{"horo.foundation.errors"};
        const ErrorDomainId ValidationDomain{"horo.foundation.validation"};
        const ErrorDomainId ObservabilityDomain{"horo.foundation.observability"};

        [[nodiscard]] ErrorCodeDescriptor ValidationDescriptor(const std::string_view code, const std::string_view summary,
                                                               const std::string_view remediationHint) {
            return {.domain = ValidationDomain,
                    .code = ErrorCode{std::string{code}},
                    .defaultSeverity = ErrorSeverity::Error,
                    .summary = summary,
                    .remediationHint = remediationHint};
        }
    }  // namespace

    namespace ConfigurationErrors {
        const ErrorCodeDescriptor SchemaInvalid{.domain = ConfigurationDomain,
                                                .code = ErrorCode{"configuration.schema_invalid"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "Configuration schema is invalid.",
                                                .remediationHint = "Correct the schema descriptor before registration.",
                                                .retryable = false,
                                                .userActionable = false};

        const ErrorCodeDescriptor SchemaSealed{.domain = ConfigurationDomain,
                                               .code = ErrorCode{"configuration.schema_sealed"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "Configuration schema is already sealed.",
                                               .remediationHint = "Register settings before sealing the schema.",
                                               .retryable = false,
                                               .userActionable = false};

        const ErrorCodeDescriptor DraftStale{.domain = ConfigurationDomain,
                                             .code = ErrorCode{"configuration.draft_stale"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "Configuration draft is stale.",
                                             .remediationHint = "Refresh the draft from the active configuration and retry.",
                                             .retryable = true,
                                             .userActionable = false};

        const ErrorCodeDescriptor ValueInvalid{.domain = ConfigurationDomain,
                                               .code = ErrorCode{"configuration.value_invalid"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "Configuration value is invalid.",
                                               .remediationHint = "Provide a value matching the registered setting type.",
                                               .retryable = false,
                                               .userActionable = true};

        const ErrorCodeDescriptor JsonParseError{.domain = ConfigurationDomain,
                                                 .code = ErrorCode{"configuration.json_parse_error"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "Configuration JSON could not be parsed.",
                                                 .remediationHint = "Repair or regenerate the configuration file.",
                                                 .retryable = false,
                                                 .userActionable = true};

        const ErrorCodeDescriptor FileNotFound{.domain = ConfigurationDomain,
                                               .code = ErrorCode{"configuration.file_not_found"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "Configuration file was not found.",
                                               .remediationHint = "Verify the configuration path.",
                                               .retryable = false,
                                               .userActionable = true};

        const ErrorCodeDescriptor FileWriteError{.domain = ConfigurationDomain,
                                                 .code = ErrorCode{"configuration.file_write_error"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "Configuration file could not be written.",
                                                 .remediationHint = "Verify destination permissions and available storage.",
                                                 .retryable = true,
                                                 .userActionable = true};

        const ErrorCodeDescriptor ResolutionFailed{.domain = ConfigurationDomain,
                                                   .code = ErrorCode{"configuration.resolution_failed"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "Configuration sources could not be resolved.",
                                                   .remediationHint = "Correct every reported source finding and retry resolution.",
                                                   .retryable = false,
                                                   .userActionable = true};

        const ErrorCodeDescriptor InputTooLarge{.domain = ConfigurationDomain,
                                                .code = ErrorCode{"configuration.input_too_large"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "Configuration input exceeds its resource limit.",
                                                .remediationHint = "Reduce the configuration input to the documented limits.",
                                                .retryable = false,
                                                .userActionable = true};

        const ErrorCodeDescriptor PersistenceUnavailable{.domain = ConfigurationDomain,
                                                         .code = ErrorCode{"configuration.persistence_unavailable"},
                                                         .defaultSeverity = ErrorSeverity::Error,
                                                         .summary = "Durable configuration persistence is not composed.",
                                                         .remediationHint = "Use the platform configuration file store.",
                                                         .retryable = false,
                                                         .userActionable = false};
    }  // namespace ConfigurationErrors

    namespace JobErrors {
        const ErrorCodeDescriptor Cancelled{.domain = JobDomain,
                                            .code = ErrorCode{"job.cancelled"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "Job was cancelled.",
                                            .remediationHint = "Retry only if the owning operation is still active.",
                                            .retryable = true,
                                            .userActionable = false};

        const ErrorCodeDescriptor Failed{.domain = JobDomain,
                                         .code = ErrorCode{"job.failed"},
                                         .defaultSeverity = ErrorSeverity::Error,
                                         .summary = "Job execution failed.",
                                         .remediationHint = "Inspect the job error and diagnostics before retrying.",
                                         .retryable = false,
                                         .userActionable = false};

        const ErrorCodeDescriptor InvalidHandle{.domain = JobDomain,
                                                .code = ErrorCode{"job.invalid_handle"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "Job handle is invalid.",
                                                .remediationHint = "Use a handle returned by the active job system.",
                                                .retryable = false,
                                                .userActionable = false};

        const ErrorCodeDescriptor NotFound{.domain = JobDomain,
                                           .code = ErrorCode{"job.not_found"},
                                           .defaultSeverity = ErrorSeverity::Error,
                                           .summary = "Job was not found.",
                                           .remediationHint = "Verify that the job belongs to the active job system.",
                                           .retryable = false,
                                           .userActionable = false};

        const ErrorCodeDescriptor QueueFull{.domain = JobDomain,
                                            .code = ErrorCode{"job.queue_full"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "Job queue is full.",
                                            .remediationHint = "Retry after queued work completes.",
                                            .retryable = true,
                                            .userActionable = false};

        const ErrorCodeDescriptor Shutdown{.domain = JobDomain,
                                           .code = ErrorCode{"job.shutdown"},
                                           .defaultSeverity = ErrorSeverity::Error,
                                           .summary = "Job system is shutting down.",
                                           .remediationHint = "Do not submit new work after shutdown begins.",
                                           .retryable = false,
                                           .userActionable = false};

        const ErrorCodeDescriptor TaskGroupClosed{.domain = JobDomain,
                                                  .code = ErrorCode{"job.task_group_closed"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "Task group is closed.",
                                                  .remediationHint = "Spawn child work before cancelling or joining the task group.",
                                                  .retryable = false,
                                                  .userActionable = false};

        const ErrorCodeDescriptor WaitForbidden{.domain = JobDomain,
                                                .code = ErrorCode{"job.wait_forbidden"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "The calling thread is not permitted to wait for this job.",
                                                .remediationHint =
                                                    "Use an allowed executor or select an explicit bounded owner-thread policy.",
                                                .retryable = false,
                                                .userActionable = false};

        const ErrorCodeDescriptor WaitTimedOut{.domain = JobDomain,
                                               .code = ErrorCode{"job.wait_timed_out"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "The bounded job wait reached its deadline.",
                                               .remediationHint =
                                                   "Cancel the owning operation or retry only while its captured state remains alive.",
                                               .retryable = true,
                                               .userActionable = false};

        const ErrorCodeDescriptor WaitCapacityDeadlock{.domain = JobDomain,
                                                       .code = ErrorCode{"job.wait_capacity_deadlock"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "The requested wait would create a scheduler capacity deadlock.",
                                                       .remediationHint =
                                                           "Restructure the dependency so a job never waits on its active execution chain.",
                                                       .retryable = false,
                                                       .userActionable = false};

        const ErrorCodeDescriptor InvalidProgress{.domain = JobDomain,
                                                  .code = ErrorCode{"job.progress_invalid"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "Job progress is invalid.",
                                                  .remediationHint = "Use a bounded phase and normalized finite progress value.",
                                                  .retryable = false,
                                                  .userActionable = false};

        const ErrorCodeDescriptor ProgressRegressed{.domain = JobDomain,
                                                    .code = ErrorCode{"job.progress_regressed"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "Job progress regressed within its phase.",
                                                    .remediationHint = "Advance monotonically or begin a new phase.",
                                                    .retryable = false,
                                                    .userActionable = false};

        const ErrorCodeDescriptor TerminalImmutable{.domain = JobDomain,
                                                    .code = ErrorCode{"job.terminal_immutable"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "Terminal job state is immutable.",
                                                    .remediationHint = "Publish progress before returning the terminal result.",
                                                    .retryable = false,
                                                    .userActionable = false};
    }  // namespace JobErrors

    namespace HashingErrors {
        const ErrorCodeDescriptor InvalidSha256Text{.domain = HashingDomain,
                                                    .code = ErrorCode{"foundation.sha256.invalid_text"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "SHA-256 text is not canonical.",
                                                    .remediationHint =
                                                        "Provide the lowercase sha256 prefix and exactly 64 lowercase hexadecimal digits.",
                                                    .retryable = false,
                                                    .userActionable = true};
    }  // namespace HashingErrors

    namespace ModuleDescriptorErrors {
        const ErrorCodeDescriptor InvalidSettingsContribution{.domain = ModuleDescriptorDomain,
                                                              .code = ErrorCode{"foundation.module.invalid_settings_contribution"},
                                                              .defaultSeverity = ErrorSeverity::Error,
                                                              .summary = "Module settings contribution is invalid."};
        const ErrorCodeDescriptor DuplicateSetting{.domain = ModuleDescriptorDomain,
                                                   .code = ErrorCode{"foundation.module.duplicate_setting"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "Configuration setting key is duplicated."};
        const ErrorCodeDescriptor SettingOwnerConflict{.domain = ModuleDescriptorDomain,
                                                       .code = ErrorCode{"foundation.module.setting_owner_conflict"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "Configuration setting owners overlap."};
        const ErrorCodeDescriptor DuplicateEnvironmentBinding{.domain = ModuleDescriptorDomain,
                                                              .code = ErrorCode{"foundation.module.duplicate_environment_binding"},
                                                              .defaultSeverity = ErrorSeverity::Error,
                                                              .summary = "Configuration environment binding is duplicated."};
        const ErrorCodeDescriptor InvalidDescriptor{.domain = ModuleDescriptorDomain,
                                                    .code = ErrorCode{"foundation.module.invalid_descriptor"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "Module descriptor is invalid.",
                                                    .remediationHint = "Correct the inert descriptor before composition.",
                                                    .retryable = false,
                                                    .userActionable = false};
        const ErrorCodeDescriptor DuplicateModule{.domain = ModuleDescriptorDomain,
                                                  .code = ErrorCode{"foundation.module.duplicate_identity"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "Module identity is duplicated.",
                                                  .remediationHint = "Assign each selected module one stable unique identity.",
                                                  .retryable = false,
                                                  .userActionable = false};
        const ErrorCodeDescriptor MissingDependency{.domain = ModuleDescriptorDomain,
                                                    .code = ErrorCode{"foundation.module.missing_dependency"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "Required module dependency is missing.",
                                                    .remediationHint = "Select the required provider before composition.",
                                                    .retryable = false,
                                                    .userActionable = false};
        const ErrorCodeDescriptor IncompatibleDependency{.domain = ModuleDescriptorDomain,
                                                         .code = ErrorCode{"foundation.module.incompatible_dependency"},
                                                         .defaultSeverity = ErrorSeverity::Error,
                                                         .summary = "Module dependency contract is incompatible.",
                                                         .remediationHint = "Select a provider meeting the minimum contract version.",
                                                         .retryable = false,
                                                         .userActionable = false};
        const ErrorCodeDescriptor MissingCapability{.domain = ModuleDescriptorDomain,
                                                    .code = ErrorCode{"foundation.module.missing_capability"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "Required module capability is missing.",
                                                    .remediationHint = "Select a module that provides the declared capability.",
                                                    .retryable = false,
                                                    .userActionable = false};
        const ErrorCodeDescriptor DependencyCycle{.domain = ModuleDescriptorDomain,
                                                  .code = ErrorCode{"foundation.module.dependency_cycle"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "Module dependency graph contains a cycle.",
                                                  .remediationHint = "Remove the cyclic dependency or capability requirement.",
                                                  .retryable = false,
                                                  .userActionable = false};
    }  // namespace ModuleDescriptorErrors

    namespace ErrorCodeRegistryErrors {
        const ErrorCodeDescriptor InvalidDescriptor{.domain = ErrorCodeRegistryDomain,
                                                    .code = ErrorCode{"foundation.error_registry.invalid_descriptor"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "Error code descriptor is invalid.",
                                                    .remediationHint = "Contribute non-null stable descriptors before activation."};
        const ErrorCodeDescriptor InvalidNamespace{.domain = ErrorCodeRegistryDomain,
                                                   .code = ErrorCode{"foundation.error_registry.invalid_namespace"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "Error namespace is invalid.",
                                                   .remediationHint = "Use a canonical domain owned by the contributing module."};
        const ErrorCodeDescriptor DomainOwnershipConflict{.domain = ErrorCodeRegistryDomain,
                                                          .code = ErrorCode{"foundation.error_registry.domain_ownership_conflict"},
                                                          .defaultSeverity = ErrorSeverity::Error,
                                                          .summary = "Error domain ownership conflicts with another module.",
                                                          .remediationHint =
                                                              "Assign each namespace and its descendants to one module owner."};
        const ErrorCodeDescriptor DuplicateCode{.domain = ErrorCodeRegistryDomain,
                                                .code = ErrorCode{"foundation.error_registry.duplicate_code"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "Error domain and code pair is duplicated.",
                                                .remediationHint = "Declare each stable error identity exactly once."};
        const ErrorCodeDescriptor InvalidDeprecation{.domain = ErrorCodeRegistryDomain,
                                                     .code = ErrorCode{"foundation.error_registry.invalid_deprecation"},
                                                     .defaultSeverity = ErrorSeverity::Error,
                                                     .summary = "Error code deprecation replacement is invalid.",
                                                     .remediationHint = "Reference a distinct registered code in the same domain."};
    }  // namespace ErrorCodeRegistryErrors

    namespace ValidationErrors {
        const ErrorCodeDescriptor InvalidLimits =
            ValidationDescriptor("foundation.validation.invalid_limits", "Validation result limits are invalid.",
                                 "Choose a non-zero capacity within the documented hard limit.");
        const ErrorCodeDescriptor UnknownDiagnostic =
            ValidationDescriptor("foundation.validation.unknown_diagnostic", "Validation finding identity is not registered.",
                                 "Declare and activate the module-owned error descriptor first.");
        const ErrorCodeDescriptor InvalidSource =
            ValidationDescriptor("foundation.validation.invalid_source", "Validation finding source context is invalid.",
                                 "Provide a stable source and a column only with a line.");
        const ErrorCodeDescriptor CapacityExceeded =
            ValidationDescriptor("foundation.validation.capacity_exceeded", "Validation finding capacity was exceeded.",
                                 "Split the input or admit a larger bounded validation pass.");
        const ErrorCodeDescriptor PassClosed =
            ValidationDescriptor("foundation.validation.pass_closed", "Validation pass is already closed.",
                                 "Create a new pass for additional findings.");
    }  // namespace ValidationErrors

    namespace ObservabilityErrors {
        const ErrorCodeDescriptor InvalidBundleRequest{.domain = ObservabilityDomain,
                                                       .code = ErrorCode{"observability.bundle.invalid_request"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "Diagnostic bundle request is invalid.",
                                                       .remediationHint = "Use absolute output and allowlisted regular files.",
                                                       .retryable = false,
                                                       .userActionable = true};
        const ErrorCodeDescriptor BundleReadFailed{.domain = ObservabilityDomain,
                                                   .code = ErrorCode{"observability.bundle.read_failed"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "Diagnostic bundle input could not be read.",
                                                   .remediationHint = "Verify source file permissions.",
                                                   .retryable = true,
                                                   .userActionable = true};
        const ErrorCodeDescriptor BundleWriteFailed{.domain = ObservabilityDomain,
                                                    .code = ErrorCode{"observability.bundle.write_failed"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "Diagnostic bundle could not be written.",
                                                    .remediationHint = "Verify destination permissions and available storage.",
                                                    .retryable = true,
                                                    .userActionable = true};
        const ErrorCodeDescriptor BundleSizeExceeded{.domain = ObservabilityDomain,
                                                     .code = ErrorCode{"observability.bundle.size_exceeded"},
                                                     .defaultSeverity = ErrorSeverity::Error,
                                                     .summary = "Diagnostic bundle input exceeded its size limit.",
                                                     .remediationHint = "Reduce the allowlist or increase the explicit bound.",
                                                     .retryable = false,
                                                     .userActionable = true};
    }  // namespace ObservabilityErrors

    namespace Math::Errors {
        const ErrorCodeDescriptor InvalidAffineMatrix{.domain = MathDomain,
                                                      .code = ErrorCode{"math.invalid_affine_matrix"},
                                                      .defaultSeverity = ErrorSeverity::Error,
                                                      .summary = "Matrix is not a valid affine transform.",
                                                      .remediationHint = "Provide a finite affine matrix.",
                                                      .retryable = false,
                                                      .userActionable = false};

        const ErrorCodeDescriptor InvalidBounds{.domain = MathDomain,
                                                .code = ErrorCode{"math.invalid_bounds"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "Bounds are invalid.",
                                                .remediationHint = "Provide finite ordered bounds.",
                                                .retryable = false,
                                                .userActionable = false};

        const ErrorCodeDescriptor InvalidHomogeneousPoint{.domain = MathDomain,
                                                          .code = ErrorCode{"math.invalid_homogeneous_point"},
                                                          .defaultSeverity = ErrorSeverity::Error,
                                                          .summary = "Homogeneous point is invalid.",
                                                          .remediationHint = "Use a finite point with a valid homogeneous divisor.",
                                                          .retryable = false,
                                                          .userActionable = false};

        const ErrorCodeDescriptor InvalidPlane{.domain = MathDomain,
                                               .code = ErrorCode{"math.invalid_plane"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "Plane is invalid.",
                                               .remediationHint = "Provide a finite plane with a non-zero normal.",
                                               .retryable = false,
                                               .userActionable = false};

        const ErrorCodeDescriptor InvalidProjection{.domain = MathDomain,
                                                    .code = ErrorCode{"math.invalid_projection"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "Projection parameters are invalid.",
                                                    .remediationHint = "Provide finite projection parameters with valid ranges.",
                                                    .retryable = false,
                                                    .userActionable = false};

        const ErrorCodeDescriptor InvalidRay{.domain = MathDomain,
                                             .code = ErrorCode{"math.invalid_ray"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "Ray is invalid.",
                                             .remediationHint = "Provide a finite normalized direction and ordered distances.",
                                             .retryable = false,
                                             .userActionable = false};

        const ErrorCodeDescriptor InvalidView{.domain = MathDomain,
                                              .code = ErrorCode{"math.invalid_view"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "View parameters are invalid.",
                                              .remediationHint = "Provide distinct finite eye and target vectors with a valid up vector.",
                                              .retryable = false,
                                              .userActionable = false};

        const ErrorCodeDescriptor NonFiniteInput{.domain = MathDomain,
                                                 .code = ErrorCode{"math.non_finite_input"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "Math input contains a non-finite value.",
                                                 .remediationHint = "Remove NaN or infinity values before the operation.",
                                                 .retryable = false,
                                                 .userActionable = false};

        const ErrorCodeDescriptor SingularMatrix{.domain = MathDomain,
                                                 .code = ErrorCode{"math.singular_matrix"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "Matrix is singular.",
                                                 .remediationHint = "Provide an invertible matrix.",
                                                 .retryable = false,
                                                 .userActionable = false};

        const ErrorCodeDescriptor ZeroLength{.domain = MathDomain,
                                             .code = ErrorCode{"math.zero_length"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "Vector length is zero.",
                                             .remediationHint = "Provide a vector with non-zero length.",
                                             .retryable = false,
                                             .userActionable = false};
    }  // namespace Math::Errors

    namespace {
        const ErrorDomainId PathDomain{"horo.foundation.paths"};
    }  // namespace

    namespace PathErrors {
        const ErrorCodeDescriptor DirectoryCreateFailed{.domain = PathDomain,
                                                        .code = ErrorCode{"paths.directory_create_failed"},
                                                        .defaultSeverity = ErrorSeverity::Error,
                                                        .summary = "Failed to create directory.",
                                                        .remediationHint = "Verify parent directory permissions and available storage.",
                                                        .retryable = true,
                                                        .userActionable = true};

        const ErrorCodeDescriptor PathEscape{.domain = PathDomain,
                                             .code = ErrorCode{"paths.escape"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "Path escapes the allowed root.",
                                             .remediationHint = "Use a path that stays within the project directory.",
                                             .retryable = false,
                                             .userActionable = true};

        const ErrorCodeDescriptor InvalidPath{.domain = PathDomain,
                                              .code = ErrorCode{"paths.invalid"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "Path is invalid or malformed.",
                                              .remediationHint = "Provide a valid path without illegal characters or empty segments.",
                                              .retryable = false,
                                              .userActionable = true};
    }  // namespace PathErrors
}  // namespace Horo
