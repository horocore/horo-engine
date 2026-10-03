#include "Horo/AI/AIErrors.h"

namespace Horo::AI::AIErrors {
    const ErrorCodeDescriptor CanonicalStateInvalid{
        .domain = ErrorDomainId{"horo.ai"},
        .code = ErrorCode{"ai.canonical.invalid"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Canonical AI state is malformed or exceeds its bounded layout.",
        .remediationHint = "Capture the complete typed AI population with every schema key exactly once.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor CanonicalSchemaUnsupported{
        .domain = ErrorDomainId{"horo.ai"},
        .code = ErrorCode{"ai.canonical.schema_unsupported"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Canonical AI state requires an unavailable or ambiguous schema migration.",
        .remediationHint = "Compose one explicit supported forward migration with the admitted source schema.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor CanonicalMigrationFailed{
        .domain = ErrorDomainId{"horo.ai"},
        .code = ErrorCode{"ai.canonical.migration_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Canonical AI schema migration cannot preserve a validated value.",
        .remediationHint = "Repair the explicit key mapping or target schema; retain the prior valid state.",
        .retryable = false,
        .userActionable = true,
    };
    const ErrorCodeDescriptor CanonicalRestoreStale{
        .domain = ErrorDomainId{"horo.ai"},
        .code = ErrorCode{"ai.canonical.restore_stale"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "The AI restore destination changed before publication.",
        .remediationHint = "Discard staging and reacquire the current Scene, agent and schema generations.",
        .retryable = true,
        .userActionable = false,
    };
    const ErrorCodeDescriptor CanonicalRestoreCancelled{
        .domain = ErrorDomainId{"horo.ai"},
        .code = ErrorCode{"ai.canonical.restore_cancelled"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "Canonical AI restore was cancelled before publication.",
        .remediationHint = "Keep the active state and prepare a new restore only when requested.",
        .retryable = true,
        .userActionable = false,
    };
}  // namespace Horo::AI::AIErrors
