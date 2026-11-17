#include "Horo/AI/AIErrors.h"

namespace Horo::AI::AIErrors {
    const ErrorCodeDescriptor PerceptionProviderFailed{
        .domain = ErrorDomainId{"horo.ai"},
        .code = ErrorCode{"ai.perception.provider_failed"},
        .defaultSeverity = ErrorSeverity::Error,
        .summary = "A custom perception provider failed or returned invalid bounded output.",
        .remediationHint = "Inspect the provider and return only valid input-candidate outcomes within its declared work limits.",
        .retryable = false,
        .userActionable = false,
    };
}  // namespace Horo::AI::AIErrors
