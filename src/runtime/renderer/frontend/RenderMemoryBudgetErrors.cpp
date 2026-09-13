#include "Horo/Runtime/Render/RenderMemoryBudgetErrors.h"

namespace Horo::Render::RenderMemoryBudgetErrors {
    namespace {
        const ErrorDomainId Domain{"render.memory"};
    }

    const ErrorCodeDescriptor InvalidConfiguration{.domain = Domain,
                                                   .code = ErrorCode{"render.memory.invalid_configuration"},
                                                   .defaultSeverity = ErrorSeverity::Error,
                                                   .summary = "Renderer memory configuration is invalid.",
                                                   .remediationHint = "Provide finite non-zero compatible budget and record limits.",
                                                   .retryable = false,
                                                   .userActionable = false};
    const ErrorCodeDescriptor InvalidCostPlan{.domain = Domain,
                                              .code = ErrorCode{"render.memory.invalid_cost_plan"},
                                              .defaultSeverity = ErrorSeverity::Error,
                                              .summary = "Renderer memory requirements are invalid or unknown.",
                                              .remediationHint = "Return a complete native-free size, alignment, class, compatibility, "
                                                                 "and provenance plan.",
                                              .retryable = false,
                                              .userActionable = false};
    const ErrorCodeDescriptor InvalidRequest{.domain = Domain,
                                             .code = ErrorCode{"render.memory.invalid_request"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "Renderer memory reservation request is invalid.",
                                             .remediationHint = "Provide a live owner scope and resource-attempt identity.",
                                             .retryable = false,
                                             .userActionable = false};
    const ErrorCodeDescriptor BudgetExceeded{.domain = Domain,
                                             .code = ErrorCode{"render.memory.budget_exceeded"},
                                             .defaultSeverity = ErrorSeverity::Error,
                                             .summary = "Renderer memory hard cap cannot admit the request.",
                                             .remediationHint = "Release eligible backing or request a smaller admitted representation.",
                                             .retryable = true,
                                             .userActionable = false};
    const ErrorCodeDescriptor CapacityExceeded{.domain = Domain,
                                               .code = ErrorCode{"render.memory.capacity_exceeded"},
                                               .defaultSeverity = ErrorSeverity::Error,
                                               .summary = "A bounded renderer memory table is full.",
                                               .remediationHint = "Drain completed work before submitting another bounded request.",
                                               .retryable = true,
                                               .userActionable = false};
    const ErrorCodeDescriptor InvalidReservation{.domain = Domain,
                                                 .code = ErrorCode{"render.memory.invalid_reservation"},
                                                 .defaultSeverity = ErrorSeverity::Error,
                                                 .summary = "Renderer memory reservation is foreign, stale, or already consumed.",
                                                 .remediationHint = "Use an outstanding reservation issued by the active frontend ledger.",
                                                 .retryable = false,
                                                 .userActionable = false};
    const ErrorCodeDescriptor InvalidAllocation{.domain = Domain,
                                                .code = ErrorCode{"render.memory.invalid_allocation"},
                                                .defaultSeverity = ErrorSeverity::Error,
                                                .summary = "Renderer memory allocation is foreign, stale, or in the wrong state.",
                                                .remediationHint = "Use a live allocation issued by the active frontend ledger.",
                                                .retryable = false,
                                                .userActionable = false};
    const ErrorCodeDescriptor InvalidPool{.domain = Domain,
                                          .code = ErrorCode{"render.memory.invalid_pool"},
                                          .defaultSeverity = ErrorSeverity::Error,
                                          .summary = "Renderer memory pool is malformed, foreign, stale, or no longer present.",
                                          .remediationHint = "Use a live pool identity returned by this frontend memory ledger.",
                                          .retryable = false,
                                          .userActionable = false};
    const ErrorCodeDescriptor Stopped{.domain = Domain,
                                      .code = ErrorCode{"render.memory.stopped"},
                                      .defaultSeverity = ErrorSeverity::Error,
                                      .summary = "Renderer memory admission is stopped.",
                                      .remediationHint = "Do not submit new renderer allocations during shutdown.",
                                      .retryable = false,
                                      .userActionable = false};
    const ErrorCodeDescriptor UnsupportedAllocation{.domain = Domain,
                                                    .code = ErrorCode{"render.memory.unsupported_allocation"},
                                                    .defaultSeverity = ErrorSeverity::Error,
                                                    .summary = "Renderer memory requirements cannot use the configured allocation class.",
                                                    .remediationHint = "Use a supported block size or an explicitly dedicated cost plan.",
                                                    .retryable = false,
                                                    .userActionable = false};
}  // namespace Horo::Render::RenderMemoryBudgetErrors
