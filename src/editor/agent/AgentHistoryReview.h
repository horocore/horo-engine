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
    /** @brief Checks the complete lines and private-key state surrounding a selected range of bounded display text.
     * @details A fragment alone may not carry its credential label. Invalid ranges are treated as sensitive.
     */
    [[nodiscard]] bool IsSensitiveDisplayRange(std::string_view source, std::size_t offset, std::size_t length);

    /** @brief Exact project and durable message identity returned after approving a recoverable ask. */
    struct AgentHistoryRecoveryTurn final {
        AgentHistoryProjectRevision project;
        AgentHistoryStartedTurn turn;
    };

    /** @brief Redacted prompt preview with no provider request or captured context. */
    class AgentHistoryPromptReview final {
    public:
        /** @brief Prepare one visible plain-text prompt for separate user approval. */
        [[nodiscard]] static Result<AgentHistoryPromptReview> Prepare(AgentHistoryProjectRevision project, std::string_view prompt);

        /** @brief Borrow the exact text that would be written to the checkpoint. */
        [[nodiscard]] std::string_view Prompt() const noexcept {
            return prompt_;
        }

        /** @brief Queue a durable streaming marker with the approved prompt and an empty assistant response. */
        [[nodiscard]] Result<AgentHistoryOperationId> StartApproved(AgentHistorySession &history, std::string_view title,
                                                                    std::uint64_t now) const;

    private:
        AgentHistoryPromptReview(AgentHistoryProjectRevision project, std::string prompt);
        AgentHistoryProjectRevision project_;
        std::string prompt_;
    };

    /** @brief Redacted terminal answer for one exact previously approved recoverable turn. */
    class AgentHistoryAnswerReview final {
    public:
        /** @brief Prepare only the visible answer for explicit approval and exact checkpoint replacement. */
        [[nodiscard]] static Result<AgentHistoryAnswerReview> Prepare(AgentHistoryRecoveryTurn recovery, std::string_view answer,
                                                                      AgentHistoryUsage usage = {});

        /** @brief Borrow the exact redacted answer to show before approval. */
        [[nodiscard]] std::string_view Answer() const noexcept {
            return answer_;
        }

        /** @brief Returns the bounded numeric metadata to show before saving this answer. */
        [[nodiscard]] AgentHistoryUsage Usage() const noexcept {
            return usage_;
        }

        /** @brief Queue completion of the exact checkpoint after the user approves this answer. */
        [[nodiscard]] Result<AgentHistoryOperationId> SaveApproved(AgentHistorySession &history, std::uint64_t now) const;

    private:
        AgentHistoryAnswerReview(AgentHistoryRecoveryTurn recovery, std::string answer, AgentHistoryUsage usage);
        AgentHistoryRecoveryTurn recovery_;
        std::string answer_;
        AgentHistoryUsage usage_;
    };

    /** @brief Immutable sanitized display pair for one exact plain local chat exchange. */
    class AgentHistoryExchangeReview final {
    public:
        /** @brief Capture the exact text to show before offering persistence.
         * @param project Exact history project selection captured with the exchange.
         * @param prompt Visible prompt from a plain local text request.
         * @param response Visible terminal assistant response.
         * @param usage Optional provider-reported numeric metadata for review.
         * @return Sanitized pair or typed invalid, capacity, or unsupported-text error.
         */
        [[nodiscard]] static Result<AgentHistoryExchangeReview> Prepare(AgentHistoryProjectRevision project, std::string_view prompt,
                                                                        std::string_view response, AgentHistoryUsage usage = {});

        /** @brief Borrow the exact sanitized prompt for presentation. */
        [[nodiscard]] std::string_view Prompt() const noexcept {
            return prompt_;
        }

        /** @brief Borrow the exact sanitized answer for presentation. */
        [[nodiscard]] std::string_view Response() const noexcept {
            return response_;
        }

        /** @brief Returns the bounded numeric metadata to show with the sanitized display pair. */
        [[nodiscard]] AgentHistoryUsage Usage() const noexcept {
            return usage_;
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
        AgentHistoryExchangeReview(AgentHistoryProjectRevision project, std::string prompt, std::string response, AgentHistoryUsage usage);
        AgentHistoryProjectRevision project_;
        std::string prompt_;
        std::string response_;
        AgentHistoryUsage usage_;
    };
}  // namespace Horo::Agent
