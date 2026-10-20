#include "ProjectErrors.h"

#include "Horo/Application/ProjectErrorDomain.h"

namespace Horo::Application::ProjectErrors {
    namespace {
        const ErrorDomainId Domain{"horo.application.project"};
    }  // namespace

    const ErrorCodeDescriptor VersionInvalid{.domain = Domain,
                                             .code = ErrorCode{"project.version.invalid"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "Horo project version is invalid.",
                                             .remediationHint = "Use canonical SemVer without build metadata.",
                                             .retryable = false,
                                             .userActionable = true};

    const ErrorCodeDescriptor HashInvalid{.domain = Domain,
                                          .code = ErrorCode{"project.contract_hash.invalid"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "Project contract hash is invalid.",
                                          .remediationHint = "Use a canonical lowercase SHA-256 identity.",
                                          .retryable = false,
                                          .userActionable = true};

    const ErrorCodeDescriptor RegistryInvalid{.domain = Domain,
                                              .code = ErrorCode{"project.compatibility_registry.invalid"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "Release compatibility catalog is invalid.",
                                              .remediationHint = "Regenerate the release compatibility catalog.",
                                              .retryable = false,
                                              .userActionable = true};

    const ErrorCodeDescriptor RegistryDuplicateRelease{.domain = Domain,
                                                       .code = ErrorCode{"project.compatibility_registry.duplicate_release"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "Release compatibility catalog has a duplicate release.",
                                                       .remediationHint = "Keep exactly one compatibility decision for each exact release.",
                                                       .retryable = false,
                                                       .userActionable = true};

    const ErrorCodeDescriptor RegistryBaselineMismatch{.domain = Domain,
                                                       .code = ErrorCode{"project.compatibility_registry.baseline_mismatch"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "Release compatibility baseline is inconsistent.",
                                                       .remediationHint =
                                                           "Reference an existing baseline decision with the same persistent contract.",
                                                       .retryable = false,
                                                       .userActionable = true};

    const ErrorCodeDescriptor
        RegistryPatchContractDrift{.domain = Domain,
                                   .code = ErrorCode{"project.compatibility_registry.patch_contract_drift"},
                                   .defaultSeverity = ErrorSeverity::Error,
                                   .summary = "A patch release changed its persistent contract.",
                                   .remediationHint = "Move the contract change to a new release line and provide a migration definition.",
                                   .retryable = false,
                                   .userActionable = true};

    const ErrorCodeDescriptor ProofRejected{.domain = Domain,
                                            .code = ErrorCode{"project.version.compatibility_proof_untrusted"},
                                            .defaultSeverity = ErrorSeverity::Error,
                                            .summary = "Project compatibility proof is not trusted.",
                                            .remediationHint =
                                                "Open the project with a Horo release that can verify its compatibility proof.",
                                            .retryable = false,
                                            .userActionable = true};

    const ErrorCodeDescriptor MetadataNotFound{.domain = Domain,
                                               .code = ErrorCode{"project.metadata.not_found"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "Project metadata was not found.",
                                               .remediationHint = "Select a directory containing .horo/project.json.",
                                               .retryable = false,
                                               .userActionable = true};

    const ErrorCodeDescriptor MetadataReadFailed{.domain = Domain,
                                                 .code = ErrorCode{"project.metadata.read_failed"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "Project metadata could not be read.",
                                                 .remediationHint = "Verify file permissions and retry.",
                                                 .retryable = true,
                                                 .userActionable = true};

    const ErrorCodeDescriptor MetadataSizeInvalid{.domain = Domain,
                                                  .code = ErrorCode{"project.metadata.size_invalid"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "Project metadata size is invalid.",
                                                  .remediationHint = "Use a non-empty project file no larger than 64 KiB.",
                                                  .retryable = false,
                                                  .userActionable = true};

    const ErrorCodeDescriptor MetadataInvalidJson{.domain = Domain,
                                                  .code = ErrorCode{"project.metadata.invalid_json"},
                                                  .defaultSeverity = ErrorSeverity::Error,
                                                  .summary = "Project metadata is not valid JSON.",
                                                  .remediationHint = "Repair or recreate project metadata.",
                                                  .retryable = false,
                                                  .userActionable = true};

    const ErrorCodeDescriptor MetadataDuplicateKey{.domain = Domain,
                                                   .code = ErrorCode{"project.metadata.duplicate_key"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "Project metadata contains a duplicate key.",
                                                   .remediationHint = "Remove duplicate JSON object keys.",
                                                   .retryable = false,
                                                   .userActionable = true};

    const ErrorCodeDescriptor MetadataLimitExceeded{.domain = Domain,
                                                    .code = ErrorCode{"project.metadata.limit_exceeded"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "Project metadata exceeds parser limits.",
                                                    .remediationHint = "Reduce metadata nesting, keys, or field sizes.",
                                                    .retryable = false,
                                                    .userActionable = true};

    const ErrorCodeDescriptor MetadataInvalidSchema{.domain = Domain,
                                                    .code = ErrorCode{"project.metadata.invalid_schema"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "Project metadata schema is unsupported.",
                                                    .remediationHint = "Open the project with a compatible Horo release.",
                                                    .retryable = false,
                                                    .userActionable = true};

    const ErrorCodeDescriptor MetadataInvalidValue{.domain = Domain,
                                                   .code = ErrorCode{"project.metadata.invalid_value"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "Project metadata contains an invalid value.",
                                                   .remediationHint = "Correct the reported project metadata field.",
                                                   .retryable = false,
                                                   .userActionable = true};

    const ErrorCodeDescriptor MigrationCatalogInvalid{.domain = Domain,
                                                      .code = ErrorCode{"project.migration.catalog_invalid"},
                                                      .defaultSeverity = ErrorSeverity::Error,
                                                      .summary = "Project migration catalog is invalid.",
                                                      .remediationHint = "Regenerate the migration catalog from canonical definitions.",
                                                      .retryable = false,
                                                      .userActionable = true};

    const ErrorCodeDescriptor MigrationDuplicateIdentity{.domain = Domain,
                                                         .code = ErrorCode{"project.migration.duplicate_identity"},
                                                         .defaultSeverity = ErrorSeverity::Error,
                                                         .summary = "A migration identity is duplicated.",
                                                         .remediationHint =
                                                             "Keep one immutable definition for each stable migration identity.",
                                                         .retryable = false,
                                                         .userActionable = true};

    const ErrorCodeDescriptor MigrationDuplicateEdge{.domain = Domain,
                                                     .code = ErrorCode{"project.migration.duplicate_edge"},
                                                     .defaultSeverity = ErrorSeverity::Error,
                                                     .summary = "A migration edge is duplicated.",
                                                     .remediationHint =
                                                         "Keep one canonical definition per kind and exact source/target pair.",
                                                     .retryable = false,
                                                     .userActionable = true};

    const ErrorCodeDescriptor MigrationBackwardEdge{.domain = Domain,
                                                    .code = ErrorCode{"project.migration.backward_edge"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "A migration edge is not forward-only.",
                                                    .remediationHint = "Use an exact older source and newer target baseline.",
                                                    .retryable = false,
                                                    .userActionable = true};

    const ErrorCodeDescriptor MigrationCycle{.domain = Domain,
                                             .code = ErrorCode{"project.migration.cycle"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "The migration graph contains a cycle.",
                                             .remediationHint = "Remove the cyclic definition; reverse migrations are unsupported.",
                                             .retryable = false,
                                             .userActionable = true};

    const ErrorCodeDescriptor MigrationAmbiguous{.domain = Domain,
                                                 .code = ErrorCode{"project.migration.ambiguous"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "The migration plan is ambiguous.",
                                                 .remediationHint = "Declare one checkpoint or retain one unique sequential chain.",
                                                 .retryable = false,
                                                 .userActionable = true};

    const ErrorCodeDescriptor MigrationPathMissing{.domain = Domain,
                                                   .code = ErrorCode{"project.migration.path_missing"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "No supported migration path exists.",
                                                   .remediationHint =
                                                       "Open the project with a Horo release whose support horizon includes it.",
                                                   .retryable = false,
                                                   .userActionable = true};

    const ErrorCodeDescriptor MigrationContractMismatch{.domain = Domain,
                                                        .code = ErrorCode{"project.migration.contract_mismatch"},
                                                        .defaultSeverity = ErrorSeverity::Error,
                                                        .summary = "Migration contract hashes do not form a valid chain.",
                                                        .remediationHint = "Regenerate definitions from the frozen persistent contracts.",
                                                        .retryable = false,
                                                        .userActionable = true};

    const ErrorCodeDescriptor MigrationProviderMissing{.domain = Domain,
                                                       .code = ErrorCode{"project.migration.provider_missing"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "A required migration provider is unavailable.",
                                                       .remediationHint =
                                                           "Restore the trusted package or plugin provider before opening the project.",
                                                       .retryable = false,
                                                       .userActionable = true};

    const ErrorCodeDescriptor MigrationPipelineInvalid{.domain = Domain,
                                                       .code = ErrorCode{"project.migration.pipeline_invalid"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "A migration pipeline is invalid.",
                                                       .remediationHint = "Fix stage identities, write sets, or validation barriers.",
                                                       .retryable = false,
                                                       .userActionable = true};

    const ErrorCodeDescriptor MigrationInventoryInvalid{.domain = Domain,
                                                        .code = ErrorCode{"project.migration.inventory_invalid"},
                                                        .defaultSeverity = ErrorSeverity::Error,
                                                        .summary = "Project migration inventory is unsafe or ambiguous.",
                                                        .remediationHint =
                                                            "Remove escaping symlinks, duplicate paths, or portable case collisions.",
                                                        .retryable = false,
                                                        .userActionable = true};

    const ErrorCodeDescriptor MigrationInventoryLimit{.domain = Domain,
                                                      .code = ErrorCode{"project.migration.inventory_limit"},
                                                      .defaultSeverity = ErrorSeverity::Error,
                                                      .summary = "Project migration inventory exceeds configured limits.",
                                                      .remediationHint = "Increase reviewed limits or reduce the authored project input.",
                                                      .retryable = false,
                                                      .userActionable = true};

    const ErrorCodeDescriptor MigrationDocumentStale{.domain = Domain,
                                                     .code = ErrorCode{"project.migration.document_stale"},
                                                     .defaultSeverity = ErrorSeverity::Error,
                                                     .summary = "A migration document handle is stale.",
                                                     .remediationHint = "Rebuild the migration inventory and retry.",
                                                     .retryable = false,
                                                     .userActionable = true};

    const ErrorCodeDescriptor MigrationWriteConflict{.domain = Domain,
                                                     .code = ErrorCode{"project.migration.write_conflict"},
                                                     .defaultSeverity = ErrorSeverity::Error,
                                                     .summary = "Parallel migration results conflict.",
                                                     .remediationHint = "Use disjoint document writes or a serialized Then stage.",
                                                     .retryable = false,
                                                     .userActionable = true};

    const ErrorCodeDescriptor MigrationStageFailed{.domain = Domain,
                                                   .code = ErrorCode{"project.migration.stage_failed"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "A project migration stage failed.",
                                                   .remediationHint = "Inspect the definition, stage, and document diagnostic.",
                                                   .retryable = false,
                                                   .userActionable = true};

    const ErrorCodeDescriptor MigrationCancelled{.domain = Domain,
                                                 .code = ErrorCode{"project.migration.cancelled"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "Project migration was cancelled.",
                                                 .remediationHint = "Retry the project-open operation when ready.",
                                                 .retryable = true,
                                                 .userActionable = true};
    const ErrorCodeDescriptor MigrationLocked{.domain = Domain,
                                              .code = ErrorCode{"project.migration.locked"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "The project is locked by another mutation.",
                                              .remediationHint = "Wait for the active project operation and retry.",
                                              .retryable = true,
                                              .userActionable = true};
    const ErrorCodeDescriptor MigrationInputChanged{.domain = Domain,
                                                    .code = ErrorCode{"project.migration.input_changed"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "Project input changed during migration preparation.",
                                                    .remediationHint = "Retry migration from a fresh project snapshot.",
                                                    .retryable = true,
                                                    .userActionable = true};
    const ErrorCodeDescriptor MigrationCapacityInsufficient{.domain = Domain,
                                                            .code = ErrorCode{"project.migration.capacity_insufficient"},
                                                            .defaultSeverity = ErrorSeverity::Error,
                                                            .summary = "There is not enough same-filesystem capacity for a safe migration.",
                                                            .remediationHint = "Free storage and retry.",
                                                            .retryable = true,
                                                            .userActionable = true};
    const ErrorCodeDescriptor MigrationPublishFailed{.domain = Domain,
                                                     .code = ErrorCode{"project.migration.publish_failed"},
                                                     .defaultSeverity = ErrorSeverity::Error,
                                                     .summary = "Migration publication failed.",
                                                     .remediationHint = "Reopen the project to run automatic recovery.",
                                                     .retryable = true,
                                                     .userActionable = true};
    const ErrorCodeDescriptor MigrationRecoveryFailed{.domain = Domain,
                                                      .code = ErrorCode{"project.migration.recovery_failed"},
                                                      .defaultSeverity = ErrorSeverity::Error,
                                                      .summary = "Migration recovery could not prove a safe action.",
                                                      .remediationHint =
                                                          "Keep the project closed and inspect the recovery diagnostic bundle.",
                                                      .retryable = false,
                                                      .userActionable = true};
}  // namespace Horo::Application::ProjectErrors

namespace Horo::Application {
    /** @copydoc ProjectErrorDomain */
    ModuleErrorDomainDescriptor ProjectErrorDomain() {
        return {.id = ProjectErrors::VersionInvalid.domain,
                .descriptors = {&ProjectErrors::VersionInvalid,
                                &ProjectErrors::HashInvalid,
                                &ProjectErrors::RegistryInvalid,
                                &ProjectErrors::RegistryDuplicateRelease,
                                &ProjectErrors::RegistryBaselineMismatch,
                                &ProjectErrors::RegistryPatchContractDrift,
                                &ProjectErrors::ProofRejected,
                                &ProjectErrors::MetadataNotFound,
                                &ProjectErrors::MetadataReadFailed,
                                &ProjectErrors::MetadataSizeInvalid,
                                &ProjectErrors::MetadataInvalidJson,
                                &ProjectErrors::MetadataDuplicateKey,
                                &ProjectErrors::MetadataLimitExceeded,
                                &ProjectErrors::MetadataInvalidSchema,
                                &ProjectErrors::MetadataInvalidValue,
                                &ProjectErrors::MigrationCatalogInvalid,
                                &ProjectErrors::MigrationDuplicateIdentity,
                                &ProjectErrors::MigrationDuplicateEdge,
                                &ProjectErrors::MigrationBackwardEdge,
                                &ProjectErrors::MigrationCycle,
                                &ProjectErrors::MigrationAmbiguous,
                                &ProjectErrors::MigrationPathMissing,
                                &ProjectErrors::MigrationContractMismatch,
                                &ProjectErrors::MigrationProviderMissing,
                                &ProjectErrors::MigrationPipelineInvalid,
                                &ProjectErrors::MigrationInventoryInvalid,
                                &ProjectErrors::MigrationInventoryLimit,
                                &ProjectErrors::MigrationDocumentStale,
                                &ProjectErrors::MigrationWriteConflict,
                                &ProjectErrors::MigrationStageFailed,
                                &ProjectErrors::MigrationCancelled,
                                &ProjectErrors::MigrationLocked,
                                &ProjectErrors::MigrationInputChanged,
                                &ProjectErrors::MigrationCapacityInsufficient,
                                &ProjectErrors::MigrationPublishFailed,
                                &ProjectErrors::MigrationRecoveryFailed}};
    }
}  // namespace Horo::Application
