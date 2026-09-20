#include "Horo/Extensions/ExtensionErrors.h"

namespace Horo::Extensions::ExtensionErrors {
    const ErrorDomainId Domain{"horo.extensions"};

    const ErrorCodeDescriptor InvalidManifest{
        .domain = Domain,
        .code = ErrorCode{"invalid_manifest"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The extension manifest is invalid or malformed.",
        .remediationHint = "Check the extension.json file for syntax errors or missing required fields.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor LoadFailed{
        .domain = Domain,
        .code = ErrorCode{"load_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Failed to load the extension dynamic library.",
        .remediationHint = "Ensure the extension is compiled for the current platform and all dependencies are present.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor MissingEntryPoint{
        .domain = Domain,
        .code = ErrorCode{"missing_entry_point"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The extension is missing the required horo_extension_load entry point.",
        .remediationHint = "Verify that the extension exports horo_extension_load using HORO_EXTENSION_EXPORT.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor ContributionRejected{
        .domain = Domain,
        .code = ErrorCode{"contribution_rejected"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The extension contribution was rejected.",
        .remediationHint = "Check contribution identities, ABI versions, descriptors, and conflicts.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor EditorSurfaceDescriptorInvalid{
        .domain = Domain,
        .code = ErrorCode{"editor_surface_descriptor_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The editor-surface descriptor is malformed or inconsistent.",
        .remediationHint = "Use a canonical identity, compatible placement, bounded policy, and active provider generation.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor InvocationFailed{
        .domain = Domain,
        .code = ErrorCode{"invocation_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "An extension callback failed.",
        .remediationHint = "Inspect the extension diagnostics and verify its input and version compatibility.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor ModuleResolutionFailed{
        .domain = Domain,
        .code = ErrorCode{"module_resolution_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The extension package module graph could not be resolved.",
        .remediationHint = "Check module roles, local dependencies, service imports, export versions, and cycles.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor LifecycleTransitionInvalid{
        .domain = Domain,
        .code = ErrorCode{"lifecycle_transition_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The extension lifecycle transition is invalid for the current state or authority.",
        .remediationHint = "Refresh lifecycle state and submit a legal transition through its owning service.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor LifecycleRevisionStale{
        .domain = Domain,
        .code = ErrorCode{"lifecycle_revision_stale"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The extension lifecycle state changed before the requested transition could commit.",
        .remediationHint = "Refresh the current lifecycle revision before retrying the transition.",
        .retryable = true,
        .userActionable = false,
    };

    const ErrorCodeDescriptor LifecycleCapacityExceeded{
        .domain = Domain,
        .code = ErrorCode{"lifecycle_capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The bounded extension lifecycle state or audit capacity was exhausted.",
        .remediationHint = "Retire archived audit state or begin a fresh package lifecycle generation.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor CapabilityAdmissionInvalid{
        .domain = Domain,
        .code = ErrorCode{"capability_admission_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The extension capability admission input is invalid.",
        .remediationHint = "Provide canonical bounded identities and a consistent host policy snapshot.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor PermissionDenied{
        .domain = Domain,
        .code = ErrorCode{"permission_denied"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The extension permission request was denied.",
        .remediationHint = "Review the extension request and approve only the required permission through host policy.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor CapabilityUnavailable{
        .domain = Domain,
        .code = ErrorCode{"capability_unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The requested application capability is unavailable.",
        .remediationHint = "Use a host composition that exposes the declared capability.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor CapabilityRevoked{
        .domain = Domain,
        .code = ErrorCode{"capability_revoked"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The extension capability handle is no longer active.",
        .remediationHint = "Stop callback dispatch and acquire a handle from the current activation generation.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor CapabilityRegistryInvalid{
        .domain = Domain,
        .code = ErrorCode{"capability_registry_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The application capability registry input is invalid.",
        .remediationHint = "Provide canonical provider identity, version, generation, and version range values.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor CapabilityRegistryDuplicate{
        .domain = Domain,
        .code = ErrorCode{"capability_registry_duplicate"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The application capability provider is already registered.",
        .remediationHint = "Register only one provider for each exact capability contract version.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor CapabilityVersionIncompatible{
        .domain = Domain,
        .code = ErrorCode{"capability_version_incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "No compatible application capability provider version is available.",
        .remediationHint = "Use a supported contract version or install a compatible provider.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor CapabilityRegistryCapacityExceeded{
        .domain = Domain,
        .code = ErrorCode{"capability_registry_capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The application capability registry capacity was exceeded.",
        .remediationHint = "Reduce the explicitly composed provider set.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor CapabilityRegistryShutdown{
        .domain = Domain,
        .code = ErrorCode{"capability_registry_shutdown"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The application capability registry is shutting down.",
        .remediationHint = "Do not register or resolve providers after host shutdown begins.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor BackendServiceInvalid{Domain,
                                                    ErrorCode{"backend_service_invalid"},
                                                    ErrorSeverity::Error,
                                                    "The backend-service descriptor or request is invalid.",
                                                    "Provide canonical identities and a complete typed provider contract.",
                                                    false,
                                                    false};
    const ErrorCodeDescriptor BackendServiceDuplicate{Domain,
                                                      ErrorCode{"backend_service_duplicate"},
                                                      ErrorSeverity::Error,
                                                      "The backend service is already registered.",
                                                      "Publish only one live provider for each service identity.",
                                                      false,
                                                      false};
    const ErrorCodeDescriptor BackendServiceUnavailable{Domain,
                                                        ErrorCode{"backend_service_unavailable"},
                                                        ErrorSeverity::Error,
                                                        "The requested backend service is unavailable.",
                                                        "Resolve a service from the current provider generation.",
                                                        false,
                                                        true};
    const ErrorCodeDescriptor
        BackendServiceContractMismatch{Domain,
                                       ErrorCode{"backend_service_contract_mismatch"},
                                       ErrorSeverity::Error,
                                       "The backend service does not match the admitted capability contract.",
                                       "Use the exact service contract, provider, version, and activation generation.",
                                       false,
                                       false};
    const ErrorCodeDescriptor BackendServiceTypeMismatch{Domain,
                                                         ErrorCode{"backend_service_type_mismatch"},
                                                         ErrorSeverity::Error,
                                                         "The backend service adapter type does not match the caller contract.",
                                                         "Use the typed contract declared by the service export.",
                                                         false,
                                                         false};
    const ErrorCodeDescriptor BackendServiceThreadViolation{Domain,
                                                            ErrorCode{"backend_service_thread_violation"},
                                                            ErrorSeverity::Error,
                                                            "The backend service was called from a forbidden thread.",
                                                            "Dispatch the operation through its declared host thread policy.",
                                                            true,
                                                            false};
    const ErrorCodeDescriptor BackendServiceInvocationFailed{Domain,
                                                             ErrorCode{"backend_service_invocation_failed"},
                                                             ErrorSeverity::Error,
                                                             "The backend service provider failed.",
                                                             "Inspect the attributed provider error cause.",
                                                             false,
                                                             false};
    const ErrorCodeDescriptor BackendServiceCancelled{Domain,
                                                      ErrorCode{"backend_service_cancelled"},
                                                      ErrorSeverity::Warning,
                                                      "The backend service operation was cancelled.",
                                                      "Retry only while the caller and provider generation remain active.",
                                                      true,
                                                      false};
    const ErrorCodeDescriptor BackendServiceCapacityExceeded{Domain,
                                                             ErrorCode{"backend_service_capacity_exceeded"},
                                                             ErrorSeverity::Error,
                                                             "The backend-service registry capacity was exceeded.",
                                                             "Reduce the explicitly composed backend-service set.",
                                                             false,
                                                             false};
    const ErrorCodeDescriptor BackendServiceShutdown{Domain,
                                                     ErrorCode{"backend_service_shutdown"},
                                                     ErrorSeverity::Error,
                                                     "The backend-service registry is shutting down.",
                                                     "Do not register or resolve services after host shutdown begins.",
                                                     false,
                                                     false};

    const ErrorCodeDescriptor ProjectValidatorRegistryInvalid{
        .domain = Domain,
        .code = ErrorCode{"project_validator_registry_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The project-validator registry input is invalid.",
        .remediationHint = "Provide canonical provider identities, bounded project-relative inputs, and declared findings.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor ProjectValidatorRegistryDuplicate{
        .domain = Domain,
        .code = ErrorCode{"project_validator_registry_duplicate"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The project-validator identity is already registered.",
        .remediationHint = "Publish only one provider generation for each validator identity.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor ProjectValidatorRegistryCapacityExceeded{
        .domain = Domain,
        .code = ErrorCode{"project_validator_registry_capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The project-validator provider capacity was exceeded.",
        .remediationHint = "Reduce the explicitly composed validator provider set.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor ProjectValidatorRegistryShutdown{
        .domain = Domain,
        .code = ErrorCode{"project_validator_registry_shutdown"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The project-validator registry is shutting down.",
        .remediationHint = "Do not register or begin validation after host shutdown starts.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor ProjectValidatorInvocationFailed{
        .domain = Domain,
        .code = ErrorCode{"project_validator_invocation_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "An attributed project-validator callback failed.",
        .remediationHint = "Inspect the provider identity and its declared validation error.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor ProjectValidationCancelled{
        .domain = Domain,
        .code = ErrorCode{"project_validation_cancelled"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "Project validation was cancelled.",
        .remediationHint = "Retry validation when the project operation is active.",
        .retryable = true,
        .userActionable = false,
    };

    const ErrorCodeDescriptor PipelineStepRegistryInvalid{
        .domain = Domain,
        .code = ErrorCode{"pipeline_step_registry_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The pipeline-step registry input is invalid.",
        .remediationHint = "Provide canonical bounded step, provider, dependency, and artifact declarations.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor ToolchainProviderRegistryInvalid{
        .domain = Domain,
        .code = ErrorCode{"toolchain_provider_registry_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The toolchain provider registry input is invalid.",
        .remediationHint = "Provide canonical bounded provider, tool, authority, and invocation values.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor PipelineStepRegistryDuplicate{
        .domain = Domain,
        .code = ErrorCode{"pipeline_step_registry_duplicate"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The pipeline step or output producer is already registered.",
        .remediationHint = "Publish one provider per step identity and one producer per generated artifact.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor ToolchainProviderRegistryDuplicate{
        .domain = Domain,
        .code = ErrorCode{"toolchain_provider_registry_duplicate"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The toolchain provider contribution is already registered.",
        .remediationHint = "Publish only one active provider generation for each contribution identity.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor PipelineStepRegistryCapacityExceeded{
        .domain = Domain,
        .code = ErrorCode{"pipeline_step_registry_capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The pipeline-step registry capacity was exceeded.",
        .remediationHint = "Reduce the explicitly composed pipeline-step provider set.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor ToolchainProviderRegistryCapacityExceeded{
        .domain = Domain,
        .code = ErrorCode{"toolchain_provider_registry_capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The toolchain provider registry capacity was exceeded.",
        .remediationHint = "Reduce the explicitly composed provider set.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor PipelineStepRegistryShutdown{
        .domain = Domain,
        .code = ErrorCode{"pipeline_step_registry_shutdown"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The pipeline-step registry is shutting down.",
        .remediationHint = "Do not register or begin pipeline runs after host shutdown starts.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor ToolchainProviderRegistryShutdown{
        .domain = Domain,
        .code = ErrorCode{"toolchain_provider_registry_shutdown"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The toolchain provider registry is shutting down.",
        .remediationHint = "Do not register or invoke providers after host shutdown starts.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor PipelineGraphInvalid{
        .domain = Domain,
        .code = ErrorCode{"pipeline_graph_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The pipeline-step dependency or artifact graph is invalid.",
        .remediationHint = "Provide existing phase-compatible dependencies and available declared inputs.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor PipelineGraphCycle{
        .domain = Domain,
        .code = ErrorCode{"pipeline_graph_cycle"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The pipeline-step dependency graph contains a cycle.",
        .remediationHint = "Remove at least one cyclic dependency before running the pipeline.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor PipelineOutputInvalid{
        .domain = Domain,
        .code = ErrorCode{"pipeline_output_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A pipeline step produced invalid output.",
        .remediationHint = "Produce each declared output exactly once through the bounded host sink.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor PipelineStepInvocationFailed{
        .domain = Domain,
        .code = ErrorCode{"pipeline_step_invocation_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "An attributed pipeline-step callback failed.",
        .remediationHint = "Inspect the provider identity and its preserved typed cause.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor PipelineRunCancelled{
        .domain = Domain,
        .code = ErrorCode{"pipeline_run_cancelled"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The pipeline run was cancelled.",
        .remediationHint = "Retry the pipeline when its owning operation remains active.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ToolchainProviderUnavailable{
        .domain = Domain,
        .code = ErrorCode{"toolchain_provider_unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The requested toolchain provider generation is unavailable.",
        .remediationHint = "Resolve a currently registered provider authority before invoking a tool.",
        .retryable = true,
        .userActionable = false,
    };

    const ErrorCodeDescriptor ToolchainPolicyRejected{
        .domain = Domain,
        .code = ErrorCode{"toolchain_policy_rejected"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Host policy rejected the requested toolchain invocation.",
        .remediationHint = "Use an approved logical tool and arguments for the active host policy.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor ToolchainInvocationFailed{
        .domain = Domain,
        .code = ErrorCode{"toolchain_invocation_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The approved tool failed at the platform process boundary.",
        .remediationHint = "Inspect the attributed provider, tool, output, and preserved platform error.",
        .retryable = false,
        .userActionable = false,
    };

    const ErrorCodeDescriptor HeadlessHostConfigurationInvalid{
        .domain = Domain,
        .code = ErrorCode{"headless_host_configuration_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The headless extension host configuration is invalid.",
        .remediationHint = "Provide valid bounded host policies and service authorities.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor HeadlessHostStateInvalid{
        .domain = Domain,
        .code = ErrorCode{"headless_host_state_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The headless extension host operation is invalid in its current state.",
        .remediationHint = "Register providers before startup and submit work only while the host is ready.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor HeadlessHostDiscoveryFailed{
        .domain = Domain,
        .code = ErrorCode{"headless_host_discovery_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Headless extension discovery failed.",
        .remediationHint = "Inspect root policy and declared package locations.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor HeadlessHostActivationFailed{
        .domain = Domain,
        .code = ErrorCode{"headless_host_activation_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Headless extension activation failed.",
        .remediationHint = "Inspect package trust, compatibility, ABI, and registration diagnostics.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor HeadlessImporterUnavailable{
        .domain = Domain,
        .code = ErrorCode{"headless_importer_unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The requested headless asset importer is unavailable.",
        .remediationHint = "Select a contribution published by the active headless composition.",
        .retryable = false,
        .userActionable = true,
    };

    const ErrorCodeDescriptor BackendOperationRegistryInvalid{
        .domain = Domain,
        .code = ErrorCode{"backend_operation_registry_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The backend-operation provider, request, or transition is invalid.",
        .remediationHint = "Provide canonical provider identities and bounded operation payloads.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor BackendOperationRegistryDuplicate{
        .domain = Domain,
        .code = ErrorCode{"backend_operation_registry_duplicate"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The backend-operation provider generation is already registered.",
        .remediationHint = "Publish one operation provider for each exact module, provider, and generation identity.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor BackendOperationRegistryCapacityExceeded{
        .domain = Domain,
        .code = ErrorCode{"backend_operation_registry_capacity_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The bounded backend-operation capacity was exceeded.",
        .remediationHint = "Retire an existing provider or operation before admitting more work.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor BackendOperationRegistryShutdown{
        .domain = Domain,
        .code = ErrorCode{"backend_operation_registry_shutdown"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The backend-operation registry is shutting down.",
        .remediationHint = "Do not register providers or begin operations after host shutdown starts.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor BackendOperationProviderUnavailable{
        .domain = Domain,
        .code = ErrorCode{"backend_operation_provider_unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The requested backend-operation provider generation is unavailable.",
        .remediationHint = "Resolve an active provider registration before beginning the operation.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor BackendOperationPayloadInvalid{
        .domain = Domain,
        .code = ErrorCode{"backend_operation_payload_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A backend-operation diagnostic or result payload is invalid or exceeds its bound.",
        .remediationHint = "Use declared typed identities and keep payloads within the provider limits.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor BackendOperationAbandoned{
        .domain = Domain,
        .code = ErrorCode{"backend_operation_abandoned"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "A backend operation producer was abandoned before completion.",
        .remediationHint = "Keep the move-only producer alive until the provider publishes a terminal result.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor BackendOperationCancelled{
        .domain = Domain,
        .code = ErrorCode{"backend_operation_cancelled"},
        .defaultSeverity = ErrorSeverity::Warning,
        .summary = "The backend operation was cancelled.",
        .remediationHint = "Retry the operation after its caller, provider, and host lifecycle are active.",
        .retryable = true,
        .userActionable = false,
    };
}  // namespace Horo::Extensions::ExtensionErrors
