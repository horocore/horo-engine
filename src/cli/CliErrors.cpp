#include "Horo/Cli/CliErrors.h"

#include <array>

namespace Horo::Cli::CliErrors {
    namespace {
        const ErrorDomainId CliDomain{"horo.cli"};
    }

    const ErrorCodeDescriptor DescriptorInvalid{.domain = CliDomain,
                                                .code = ErrorCode{"cli.descriptor_invalid"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "CLI command descriptor is invalid.",
                                                .remediationHint = "Correct the inert command metadata before host activation.",
                                                .retryable = false,
                                                .userActionable = false};

    const ErrorCodeDescriptor RegistryCapacityExceeded{.domain = CliDomain,
                                                       .code = ErrorCode{"cli.registry_capacity_exceeded"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "CLI command registry capacity was exceeded.",
                                                       .remediationHint = "Reduce contributed metadata or revise the explicit host limits.",
                                                       .retryable = false,
                                                       .userActionable = false};

    const ErrorCodeDescriptor CommandPathDuplicate{.domain = CliDomain,
                                                   .code = ErrorCode{"cli.command_path_duplicate"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "CLI command path is already registered.",
                                                   .remediationHint = "Assign every command one unique hierarchical path.",
                                                   .retryable = false,
                                                   .userActionable = false};

    const ErrorCodeDescriptor OptionNameDuplicate{.domain = CliDomain,
                                                  .code = ErrorCode{"cli.option_name_duplicate"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "CLI option name is duplicated within a command.",
                                                  .remediationHint = "Use unique long names and short aliases within the command.",
                                                  .retryable = false,
                                                  .userActionable = false};

    const ErrorCodeDescriptor OptionSchemaIncompatible{.domain = CliDomain,
                                                       .code = ErrorCode{"cli.option_schema_incompatible"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "CLI option schema contains incompatible metadata.",
                                                       .remediationHint = "Align the option kind, defaults, and enumeration values.",
                                                       .retryable = false,
                                                       .userActionable = false};

    const ErrorCodeDescriptor OutputSchemaIncompatible{.domain = CliDomain,
                                                       .code = ErrorCode{"cli.output_schema_incompatible"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "CLI output schema contains incompatible metadata.",
                                                       .remediationHint =
                                                           "Declare a versioned schema with human and machine output formats.",
                                                       .retryable = false,
                                                       .userActionable = false};

    const ErrorCodeDescriptor CapabilityUnauthorized{.domain = CliDomain,
                                                     .code = ErrorCode{"cli.capability_unauthorized"},
                                                     .defaultSeverity = ErrorSeverity::Error,
                                                     .summary = "CLI command requires an unauthorized capability.",
                                                     .remediationHint = "Approve the capability at composition or omit the contribution.",
                                                     .retryable = false,
                                                     .userActionable = false};

    const ErrorCodeDescriptor HostUnsupported{.domain = CliDomain,
                                              .code = ErrorCode{"cli.host_unsupported"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "CLI command does not support the active host.",
                                              .remediationHint = "Select descriptors declared for this executable composition.",
                                              .retryable = false,
                                              .userActionable = false};

    const ErrorCodeDescriptor ContractVersionIncompatible{.domain = CliDomain,
                                                          .code = ErrorCode{"cli.contract_version_incompatible"},
                                                          .defaultSeverity = ErrorSeverity::Error,
                                                          .summary = "CLI command descriptor contract is incompatible with the host.",
                                                          .remediationHint = "Use the host contract major and no newer minor version.",
                                                          .retryable = false,
                                                          .userActionable = false};

    const ErrorCodeDescriptor ParserPolicyInvalid{.domain = CliDomain,
                                                  .code = ErrorCode{"cli.parser_policy_invalid"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "CLI parser policy is invalid.",
                                                  .remediationHint = "Provide non-zero parser limits and explicit host input seams.",
                                                  .retryable = false,
                                                  .userActionable = false};

    const ErrorCodeDescriptor CommandUnknown{.domain = CliDomain,
                                             .code = ErrorCode{"cli.command_unknown"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "CLI command is unknown.",
                                             .remediationHint = "Select a command from generated CLI help.",
                                             .retryable = false,
                                             .userActionable = true};

    const ErrorCodeDescriptor ParseFailed{.domain = CliDomain,
                                          .code = ErrorCode{"cli.parse_failed"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "CLI arguments failed validation.",
                                          .remediationHint = "Correct every structured usage diagnostic and retry.",
                                          .retryable = false,
                                          .userActionable = true};

    const ErrorCodeDescriptor InputModeUnsupported{.domain = CliDomain,
                                                   .code = ErrorCode{"cli.input_mode_unsupported"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "CLI stdin mode is not supported by the command.",
                                                   .remediationHint = "Select only the input grammar declared by generated help.",
                                                   .retryable = false,
                                                   .userActionable = true};

    const ErrorCodeDescriptor InputCapacityExceeded{.domain = CliDomain,
                                                    .code = ErrorCode{"cli.input_capacity_exceeded"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "CLI input exceeds its configured bound.",
                                                    .remediationHint = "Reduce argv or stdin input to the documented limit.",
                                                    .retryable = false,
                                                    .userActionable = true};

    const ErrorCodeDescriptor InteractiveInputUnavailable{.domain = CliDomain,
                                                          .code = ErrorCode{"cli.interactive_input_unavailable"},
                                                          .defaultSeverity = ErrorSeverity::Error,
                                                          .summary = "Required interactive input is unavailable.",
                                                          .remediationHint = "Provide the command's deterministic option alternative.",
                                                          .retryable = false,
                                                          .userActionable = true};

    const ErrorCodeDescriptor
        DispatchRegistrationInvalid{.domain = CliDomain,
                                    .code = ErrorCode{"cli.dispatch_registration_invalid"},
                                    .defaultSeverity = ErrorSeverity::Error,
                                    .summary = "CLI dispatch registration is invalid.",
                                    .remediationHint = "Bind one matching adapter and the exact declared capabilities before activation.",
                                    .retryable = false,
                                    .userActionable = false};

    const ErrorCodeDescriptor CommandUnavailable{.domain = CliDomain,
                                                 .code = ErrorCode{"cli.command_unavailable"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "CLI command has no active application adapter.",
                                                 .remediationHint = "Install or activate the module that owns this command.",
                                                 .retryable = false,
                                                 .userActionable = true};

    const ErrorCodeDescriptor SideEffectUnauthorized{.domain = CliDomain,
                                                     .code = ErrorCode{"cli.side_effect_unauthorized"},
                                                     .defaultSeverity = ErrorSeverity::Error,
                                                     .summary = "CLI command side effects are not authorized.",
                                                     .remediationHint =
                                                         "Use an invocation policy that explicitly admits these side effects.",
                                                     .retryable = false,
                                                     .userActionable = true};

    const ErrorCodeDescriptor ExecutionContextInvalid{.domain = CliDomain,
                                                      .code = ErrorCode{"cli.execution_context_invalid"},
                                                      .defaultSeverity = ErrorSeverity::Error,
                                                      .summary = "CLI execution context is invalid.",
                                                      .remediationHint =
                                                          "Provide valid invocation identity, correlation, timeout, and bounds.",
                                                      .retryable = false,
                                                      .userActionable = false};

    const ErrorCodeDescriptor ExecutionCancelled{.domain = CliDomain,
                                                 .code = ErrorCode{"cli.execution_cancelled"},
                                                 .defaultSeverity = ErrorSeverity::Warning,
                                                 .summary = "CLI command execution was cancelled.",
                                                 .remediationHint = "Retry the command when the operation should continue.",
                                                 .retryable = true,
                                                 .userActionable = true};

    const ErrorCodeDescriptor ExecutionTimedOut{.domain = CliDomain,
                                                .code = ErrorCode{"cli.execution_timed_out"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "CLI command execution exceeded its deadline.",
                                                .remediationHint = "Use an admitted timeout or reduce the requested work.",
                                                .retryable = true,
                                                .userActionable = true};

    const ErrorCodeDescriptor ExecutionCapacityExceeded{.domain = CliDomain,
                                                        .code = ErrorCode{"cli.execution_capacity_exceeded"},
                                                        .defaultSeverity = ErrorSeverity::Error,
                                                        .summary = "CLI command execution exceeded a bounded resource limit.",
                                                        .remediationHint = "Reduce progress or result output to the documented limit.",
                                                        .retryable = false,
                                                        .userActionable = true};
    const ErrorCodeDescriptor HostFailure{.domain = CliDomain,
                                          .code = ErrorCode{"cli.host_failure"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "CLI host failed before completing the invocation.",
                                          .remediationHint = "Inspect local diagnostics and retry the invocation.",
                                          .retryable = false,
                                          .userActionable = false};

    namespace {
        /** @brief Builds inert parser metadata with the original shared severity and remediation. */
        ErrorCodeDescriptor ParserDiagnostic(const char *identity, const char *summary) {
            return {.domain = CliDomain,
                    .code = ErrorCode{identity},
                    .defaultSeverity = ErrorSeverity::Error,
                    .summary = summary,
                    .remediationHint = "Check the declared command grammar."};
        }

        /** @brief Original parser diagnostic identities registered for canonical cross-host translation. */
        const std::array<ErrorCodeDescriptor, 11> ParserDiagnosticDescriptors{{
            ParserDiagnostic("cli.configuration_value_incompatible", "Configured value is incompatible with the command option."),
            ParserDiagnostic("cli.option_required", "Required option is missing."),
            ParserDiagnostic("cli.option_source_conflict", "Command option sources conflict."),
            ParserDiagnostic("cli.option_unknown", "Unknown option."),
            ParserDiagnostic("cli.option_value_invalid", "Option value is invalid."),
            ParserDiagnostic("cli.option_value_missing", "Option requires a value."),
            ParserDiagnostic("cli.path_normalization_failed", "Path input could not be normalized safely."),
            ParserDiagnostic("cli.path_normalizer_unavailable", "Path input requires the host path-normalization adapter."),
            ParserDiagnostic("cli.positional_required", "Required positional input is missing."),
            ParserDiagnostic("cli.positional_unexpected", "Unexpected positional input."),
            ParserDiagnostic("cli.stdin_malformed", "Standard input is malformed."),
        }};
    }  // namespace

    /** @copydoc ErrorDomain */
    ModuleErrorDomainDescriptor ErrorDomain() {
        ModuleErrorDomainDescriptor domain{.id = CliDomain,
                                           .descriptors = {&DescriptorInvalid,
                                                           &RegistryCapacityExceeded,
                                                           &CommandPathDuplicate,
                                                           &OptionNameDuplicate,
                                                           &OptionSchemaIncompatible,
                                                           &OutputSchemaIncompatible,
                                                           &CapabilityUnauthorized,
                                                           &HostUnsupported,
                                                           &ContractVersionIncompatible,
                                                           &ParserPolicyInvalid,
                                                           &CommandUnknown,
                                                           &ParseFailed,
                                                           &InputModeUnsupported,
                                                           &InputCapacityExceeded,
                                                           &InteractiveInputUnavailable,
                                                           &DispatchRegistrationInvalid,
                                                           &CommandUnavailable,
                                                           &SideEffectUnauthorized,
                                                           &ExecutionContextInvalid,
                                                           &ExecutionCancelled,
                                                           &ExecutionTimedOut,
                                                           &ExecutionCapacityExceeded,
                                                           &HostFailure}};
        for (const auto &descriptor : ParserDiagnosticDescriptors)
            domain.descriptors.push_back(&descriptor);
        return domain;
    }
}  // namespace Horo::Cli::CliErrors
