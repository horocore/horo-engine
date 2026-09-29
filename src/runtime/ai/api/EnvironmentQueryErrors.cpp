#include "Horo/AI/AIErrors.h"

namespace Horo::AI::AIErrors {
    namespace {
        const ErrorDomainId AiDomain{"horo.ai"};
    }

    const ErrorCodeDescriptor EnvironmentQuerySchemaInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.environment_query.schema_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "An environment-query asset or descriptor has an invalid typed representation.",
        .remediationHint = "Correct non-zero identities, versions, stage order, and typed property contracts before admission.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor EnvironmentQueryLimitExceeded{
        .domain = AiDomain,
        .code = ErrorCode{"ai.environment_query.limit_exceeded"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Environment-query metadata exceeds a hard validation bound.",
        .remediationHint = "Reduce stage, descriptor, property, context, or canonical payload counts before admission.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor EnvironmentQueryIdentityConflict{
        .domain = AiDomain,
        .code = ErrorCode{"ai.environment_query.identity_conflict"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A stable environment-query identity is duplicated within its domain.",
        .remediationHint = "Issue unique stage, descriptor, and property IDs independent of labels or serialized order.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor EnvironmentQueryDescriptorUnavailable{
        .domain = AiDomain,
        .code = ErrorCode{"ai.environment_query.descriptor_unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A required environment-query item, context, generator, or test descriptor is unavailable.",
        .remediationHint = "Install the owning contribution and recapture descriptors before compiling the query.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor EnvironmentQueryVersionIncompatible{
        .domain = AiDomain,
        .code = ErrorCode{"ai.environment_query.version_incompatible"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "An environment-query descriptor is outside the authored schema-version interval.",
        .remediationHint = "Migrate the authoring schema or install a compatible descriptor version.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor EnvironmentQueryStageUnsupported{
        .domain = AiDomain,
        .code = ErrorCode{"ai.environment_query.stage_unsupported"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "An unknown environment-query stage is preserved but cannot be executed.",
        .remediationHint = "Install a compatible stage provider or remove the unsupported stage before compilation.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor EnvironmentQueryContextMissing{
        .domain = AiDomain,
        .code = ErrorCode{"ai.environment_query.context_missing"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A required environment-query context cannot be captured.",
        .remediationHint = "Supply the target or explicitly compose the owning context provider before submission.",
        .retryable = true,
        .userActionable = true,
    };
    const ErrorCodeDescriptor EnvironmentQueryContextStale{
        .domain = AiDomain,
        .code = ErrorCode{"ai.environment_query.context_stale"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "An environment-query context has a stale scene, entity, or execution revision.",
        .remediationHint = "Acquire a current scene view and resubmit with current generation-checked entities.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor EnvironmentQueryContextCapabilityUnavailable{
        .domain = AiDomain,
        .code = ErrorCode{"ai.environment_query.context_capability_unavailable"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A required environment-query context provider capability is unavailable.",
        .remediationHint = "Select a composition that supplies the declared capability.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor EnvironmentQueryContextInvalid{
        .domain = AiDomain,
        .code = ErrorCode{"ai.environment_query.context_invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "An environment-query context provider returned invalid typed data.",
        .remediationHint = "Return only bounded owned canonical values and current scene entity references.",
        .retryable = false,
        .userActionable = true,
    };
}  // namespace Horo::AI::AIErrors
