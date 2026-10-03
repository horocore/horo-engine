#pragma once

#include "Horo/Foundation/ErrorCode.h"

namespace Horo::WorldStreaming::Internal {
    /**
     * @brief Builds inert immutable metadata in the World Streaming error domain without static initialization dependencies.
     * @param code Stable error identity. @param severity Default severity. @param summary Domain failure description.
     * @param remediationHint Actionable recovery context. @param userActionable Whether a host may present an action.
     * @return Complete non-retryable descriptor with the canonical World Streaming domain identity.
     */
    [[nodiscard]] inline ErrorCodeDescriptor Describe(const char *code, const ErrorSeverity severity, const char *summary,
                                                      const char *remediationHint, const bool userActionable) {
        return {.domain = ErrorDomainId{"horo.world_streaming"},
                .code = ErrorCode{code},
                .defaultSeverity = severity,
                .summary = summary,
                .remediationHint = remediationHint,
                .retryable = false,
                .userActionable = userActionable};
    }
}  // namespace Horo::WorldStreaming::Internal
