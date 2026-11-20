#pragma once

/** @file AgentHistoryReview.h @brief Local editor display redaction and reviewed history writes. */

#include "AgentHistorySession.h"

namespace Horo::Agent {
    /** @brief Removes recognized credential-bearing lines and rejects unsupported display text.
     * @param kind History display field being admitted.
     * @param visible Text currently visible to the editor user.
     * @return The exact idempotently redacted display value, or a denial.
     * @details Only for plain display text. Structured project context, tool data, and provider metadata
     * are outside this boundary. The caller presents the result for the user's per-exchange approval.
     */
    [[nodiscard]] AgentHistoryTextDecision RedactHistoryDisplayText(AgentHistoryTextKind kind, std::string_view visible);

    /** @brief Immutable sanitized display pair for one exact plain local chat exchange. */
    class AgentHistoryExchangeReview final {
    public:
        /** @brief Capture the exact text to show before offering persistence.
         * @param project Exact history project selection captured with the exchange.
         * @param prompt Visible prompt from a plain local text request.
         * @param response Visible terminal assistant response.
         * @return Sanitized pair or typed invalid, capacity, or unsupported-text error.
         */
        [[nodiscard]] static Result<AgentHistoryExchangeReview> Prepare(AgentHistoryProjectRevision project, std::string_view prompt,
                                                                        std::string_view response);

        /** @brief Borrow the exact sanitized prompt for presentation. */
        [[nodiscard]] std::string_view Prompt() const noexcept {
            return prompt_;
        }

        /** @brief Borrow the exact sanitized answer for presentation. */
        [[nodiscard]] std::string_view Response() const noexcept {
            return response_;
        }

        /** @brief Queue the exact preview after the user approves saving it.
         * @param history Exact editor-owned worker session.
         * @param title Host-supplied display title. Use localized copy with no provider text substitutions.
         * @param now Host epoch seconds.
         * @return Exact queued operation or a stale, busy, capacity, or unavailable denial.
         */
        [[nodiscard]] Result<AgentHistoryOperationId> SaveApproved(AgentHistorySession &history, std::string_view title,
                                                                   std::uint64_t now) const;

    private:
        AgentHistoryExchangeReview(AgentHistoryProjectRevision project, std::string prompt, std::string response);
        AgentHistoryProjectRevision project_;
        std::string prompt_;
        std::string response_;
    };
}  // namespace Horo::Agent
