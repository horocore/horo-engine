#include "EditorServiceErrors.h"

namespace Horo::Editor {
    namespace {
        const ErrorDomainId SettingsDomain{"horo.editor.settings"};
        const ErrorDomainId ProjectCreationDomain{"horo.editor.project_creation"};
        const ErrorDomainId ProjectMetadataDomain{"horo.editor.project_metadata"};
        const ErrorDomainId ProjectOpenDomain{"horo.editor.project_open"};
        const ErrorDomainId ModalDomain{"horo.editor.modal_host"};
    }  // namespace

    namespace SettingsErrors {
        const ErrorCodeDescriptor DraftStale{
            .domain = SettingsDomain,
            .code = ErrorCode{"editor.settings.draft_stale"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Editor settings draft is stale.",
            .remediationHint = "Refresh settings and reapply the changes.",
            .retryable = true,
            .userActionable = true,
        };
        const ErrorCodeDescriptor PersistenceFailed{
            .domain = SettingsDomain,
            .code = ErrorCode{"editor.settings.persistence_failed"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Editor settings could not be persisted.",
            .remediationHint = "Verify settings storage permissions and retry.",
            .retryable = true,
            .userActionable = true,
        };
        const ErrorCodeDescriptor ValidationFailed{
            .domain = SettingsDomain,
            .code = ErrorCode{"editor.settings.validation_failed"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Editor settings validation failed.",
            .remediationHint = "Correct the reported settings values.",
            .retryable = false,
            .userActionable = true,
        };
    }  // namespace SettingsErrors

    namespace ProjectCreationErrors {
        const ErrorCodeDescriptor UnexpectedFailure{
            .domain = ProjectCreationDomain,
            .code = ErrorCode{"project_creation.unexpected_failure"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Project creation encountered an unexpected host failure.",
            .remediationHint = "Inspect the publication state and retained diagnostic before retrying.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor DestinationOccupied{
            .domain = ProjectCreationDomain,
            .code = ErrorCode{"project_creation.destination_occupied"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Project destination is already occupied.",
            .remediationHint = "Choose an empty destination or remove the conflicting files.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidRequest{
            .domain = ProjectCreationDomain,
            .code = ErrorCode{"project_creation.invalid_request"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Project creation request is invalid.",
            .remediationHint = "Correct the project name, destination, and template values.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor JobSubmissionFailed{
            .domain = ProjectCreationDomain,
            .code = ErrorCode{"project_creation.job_submission_failed"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Project creation job could not be submitted.",
            .remediationHint = "Retry after the job system can accept work.",
            .retryable = true,
            .userActionable = true,
        };
        const ErrorCodeDescriptor NotFound{
            .domain = ProjectCreationDomain,
            .code = ErrorCode{"project_creation.not_found"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Project creation operation was not found.",
            .remediationHint = "Refresh the active operation ID.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor NotReady{
            .domain = ProjectCreationDomain,
            .code = ErrorCode{"project_creation.not_ready"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Project creation operation is not ready.",
            .remediationHint = "Wait for the operation to complete before consuming its result.",
            .retryable = true,
            .userActionable = true,
        };
    }  // namespace ProjectCreationErrors

    namespace ProjectMetadataErrors {
        const ErrorCodeDescriptor DuplicateKey{
            .domain = ProjectMetadataDomain,
            .code = ErrorCode{"project.metadata_duplicate_key"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Project metadata contains a duplicate key.",
            .remediationHint = "Remove duplicate JSON object keys.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidJson{
            .domain = ProjectMetadataDomain,
            .code = ErrorCode{"project.metadata_invalid_json"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Project metadata JSON is invalid.",
            .remediationHint = "Repair or regenerate the project metadata file.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidSchema{
            .domain = ProjectMetadataDomain,
            .code = ErrorCode{"project.metadata_invalid_schema"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Project metadata schema is invalid or unsupported.",
            .remediationHint = "Migrate the metadata to a supported schema.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor InvalidValues{
            .domain = ProjectMetadataDomain,
            .code = ErrorCode{"project.metadata_invalid_values"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Project metadata values are invalid.",
            .remediationHint = "Correct the reported project metadata fields.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor NotFound{
            .domain = ProjectMetadataDomain,
            .code = ErrorCode{"project.metadata_not_found"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Project metadata file was not found.",
            .remediationHint = "Select a valid Horo project directory.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor ReadFailed{
            .domain = ProjectMetadataDomain,
            .code = ErrorCode{"project.metadata_read_failed"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Project metadata file could not be read.",
            .remediationHint = "Verify file permissions and retry.",
            .retryable = true,
            .userActionable = true,
        };
        const ErrorCodeDescriptor SizeInvalid{
            .domain = ProjectMetadataDomain,
            .code = ErrorCode{"project.metadata_size_invalid"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Project metadata file size is invalid.",
            .remediationHint = "Use a non-empty metadata file within the supported size limit.",
            .retryable = false,
            .userActionable = true,
        };
    }  // namespace ProjectMetadataErrors

    namespace ProjectOpenErrors {
        const ErrorCodeDescriptor Busy{.domain = ProjectOpenDomain,
                                       .code = ErrorCode{"project.open.busy"},
                                       .defaultSeverity = ErrorSeverity::Error,
                                       .summary = "Another project-open operation is active.",
                                       .remediationHint = "Wait for it to finish or cancel it before retrying.",
                                       .retryable = true,
                                       .userActionable = true};
        const ErrorCodeDescriptor NotFound{.domain = ProjectOpenDomain,
                                           .code = ErrorCode{"project.open.not_found"},
                                           .defaultSeverity = ErrorSeverity::Error,
                                           .summary = "Project-open operation was not found.",
                                           .remediationHint = "Start a new project-open operation.",
                                           .retryable = false,
                                           .userActionable = false};
        const ErrorCodeDescriptor CompatibilityBlocked{.domain = ProjectOpenDomain,
                                                       .code = ErrorCode{"project.open.compatibility_blocked"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "Project compatibility prevents opening.",
                                                       .remediationHint =
                                                           "Use a compatible Horo release or restore the required migration provider.",
                                                       .retryable = false,
                                                       .userActionable = true};
        const ErrorCodeDescriptor MigrationPlanMissing{.domain = ProjectOpenDomain,
                                                       .code = ErrorCode{"project.open.migration_plan_missing"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "No deterministic migration path is available.",
                                                       .remediationHint = "Install a release that supports this project version.",
                                                       .retryable = false,
                                                       .userActionable = true};
        const ErrorCodeDescriptor MetadataUpdateFailed{.domain = ProjectOpenDomain,
                                                       .code = ErrorCode{"project.open.metadata_update_failed"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "Project version metadata could not be updated.",
                                                       .remediationHint = "Check project permissions and retry.",
                                                       .retryable = true,
                                                       .userActionable = true};
        const ErrorCodeDescriptor ScenePreflightFailed{.domain = ProjectOpenDomain,
                                                       .code = ErrorCode{"project.open.scene_preflight_failed"},
                                                       .defaultSeverity = ErrorSeverity::Error,
                                                       .summary = "The project default scene could not be validated.",
                                                       .remediationHint =
                                                           "Repair settings.defaultScene or restore a valid Horo scene file.",
                                                       .retryable = true,
                                                       .userActionable = true};
        const ErrorCodeDescriptor DerivedStateFailed{.domain = ProjectOpenDomain,
                                                     .code = ErrorCode{"project.open.derived_state_failed"},
                                                     .defaultSeverity = ErrorSeverity::Error,
                                                     .summary = "Derived project state could not be rebuilt.",
                                                     .remediationHint = "Inspect the diagnostic and retry project opening.",
                                                     .retryable = true,
                                                     .userActionable = true};
        const ErrorCodeDescriptor Cancelled{.domain = ProjectOpenDomain,
                                            .code = ErrorCode{"project.open.cancelled"},
                                            .defaultSeverity = ErrorSeverity::Warning,
                                            .summary = "Project opening was cancelled.",
                                            .remediationHint = "Retry when ready.",
                                            .retryable = true,
                                            .userActionable = true};
        const ErrorCodeDescriptor SessionStale{.domain = ProjectOpenDomain,
                                               .code = ErrorCode{"project.open.session_stale"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "The prepared project session is stale or already consumed.",
                                               .remediationHint = "Return to project loading and prepare a new session.",
                                               .retryable = true,
                                               .userActionable = true};
        const ErrorCodeDescriptor WorkerCapacityInsufficient{.domain = ProjectOpenDomain,
                                                             .code = ErrorCode{"project.open.worker_capacity_insufficient"},
                                                             .defaultSeverity = ErrorSeverity::Error,
                                                             .summary =
                                                                 "Project migration requires additional structured-concurrency capacity.",
                                                             .remediationHint = "Retry with at least two project worker threads.",
                                                             .retryable = true,
                                                             .userActionable = false};
    }  // namespace ProjectOpenErrors

    namespace ModalErrors {
        const ErrorCodeDescriptor Busy{
            .domain = ModalDomain,
            .code = ErrorCode{"editor.modal_host.busy"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Another root modal is already active.",
            .remediationHint = "Close the active root modal before opening another.",
            .retryable = true,
            .userActionable = true,
        };
        const ErrorCodeDescriptor CloseDenied{
            .domain = ModalDomain,
            .code = ErrorCode{"editor.modal_host.close_denied"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Modal denied the requested close.",
            .remediationHint = "Resolve the modal state before closing it.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor DuplicateId{
            .domain = ModalDomain,
            .code = ErrorCode{"editor.modal_host.duplicate_id"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Modal ID is already active.",
            .remediationHint = "Use a unique modal ID.",
            .retryable = false,
            .userActionable = false,
        };
        const ErrorCodeDescriptor InvalidModal{
            .domain = ModalDomain,
            .code = ErrorCode{"editor.modal_host.invalid_modal"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Modal request is invalid.",
            .remediationHint = "Provide a valid modal instance and ID.",
            .retryable = false,
            .userActionable = false,
        };
        const ErrorCodeDescriptor ModalNotTop{
            .domain = ModalDomain,
            .code = ErrorCode{"editor.modal_host.modal_not_top"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Only the top modal may close.",
            .remediationHint = "Close child modals before their parent.",
            .retryable = false,
            .userActionable = true,
        };
        const ErrorCodeDescriptor ParentNotTop{
            .domain = ModalDomain,
            .code = ErrorCode{"editor.modal_host.parent_not_top"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Only the top modal may push a child.",
            .remediationHint = "Use the current top modal as the child owner.",
            .retryable = false,
            .userActionable = false,
        };
        const ErrorCodeDescriptor StackLimitReached{
            .domain = ModalDomain,
            .code = ErrorCode{"editor.modal_host.stack_limit_reached"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "Modal stack depth limit was reached.",
            .remediationHint = "Close a modal before opening another child.",
            .retryable = true,
            .userActionable = true,
        };
    }  // namespace ModalErrors
}  // namespace Horo::Editor
