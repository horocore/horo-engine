#include "InputErrors.h"

namespace Horo::Input::Errors {
    namespace {
        const ErrorDomainId Domain{"horo.input"};
    }

    const ErrorCodeDescriptor CollectionInvalid{
        .domain = Domain,
        .code = ErrorCode{"input.collection.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Input collection candidate or canonical action registration is invalid.",
        .remediationHint = "Use the current owner-thread collection and a validated canonical registration.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor CollectionStale{
        .domain = Domain,
        .code = ErrorCode{"input.collection.stale"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Input collection owner, source, assignment or frame generation is stale.",
        .remediationHint = "Use the current owner-thread collection and a validated canonical registration.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor CollectionCapacity{
        .domain = Domain,
        .code = ErrorCode{"input.collection.capacity"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Input collection fixed source or action capacity is exhausted.",
        .remediationHint = "Use the current owner-thread collection and a validated canonical registration.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor CollectionWrongThread{
        .domain = Domain,
        .code = ErrorCode{"input.collection.wrong_thread"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Input collection must run on its router owner thread.",
        .remediationHint = "Use the current owner-thread collection and a validated canonical registration.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ProfileInvalidSchema{
        .domain = Domain,
        .code = ErrorCode{"input.profile.invalid_schema"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Input profile schema is invalid or unsupported.",
        .remediationHint = "Use a supported input profile schema.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ProfileMalformed{
        .domain = Domain,
        .code = ErrorCode{"input.profile.malformed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Input profile content is malformed.",
        .remediationHint = "Repair or regenerate the input profile.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ProfileReadFailed{
        .domain = Domain,
        .code = ErrorCode{"input.profile.read_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Input profile could not be read.",
        .remediationHint = "Verify that the profile exists and is readable.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ProfileDirectoryCreationFailed{
        .domain = Domain,
        .code = ErrorCode{"input.profile.mkdir_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Input profile directory could not be created.",
        .remediationHint = "Verify the destination path and permissions.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ProfileWriteFailed{
        .domain = Domain,
        .code = ErrorCode{"input.profile.write_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Input profile could not be written.",
        .remediationHint = "Verify available storage and destination permissions.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ProfilePromotionFailed{
        .domain = Domain,
        .code = ErrorCode{"input.profile.promote_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Input profile replacement could not be committed.",
        .remediationHint = "Retry after checking destination permissions.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor CaptureInactiveContext{
        .domain = Domain,
        .code = ErrorCode{"input.capture.inactive_context"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Input capture requires an active context.",
        .remediationHint = "Activate the context before requesting capture.",
        .retryable = false,
        .userActionable = false,
    };
    const ErrorCodeDescriptor CaptureBusy{
        .domain = Domain,
        .code = ErrorCode{"input.capture.busy"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Pointer input is already captured.",
        .remediationHint = "Release the current capture before requesting another.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor ActionMapValidationFailed{
        .domain = Domain,
        .code = ErrorCode{"input.action_map.validation_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Input action map validation failed.",
        .remediationHint = "Resolve the reported binding diagnostics.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor ProfileValidationFailed{
        .domain = Domain,
        .code = ErrorCode{"input.profile.validation_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Input profile validation failed.",
        .remediationHint = "Resolve the reported binding diagnostics.",
        .retryable = false,
        .userActionable = true,
    };
}  // namespace Horo::Input::Errors
