#pragma once

#include "AgentContext.h"
#include "Horo/Agent/ModelProvider.h"

#include <atomic>

namespace Horo::Agent {
    /** @brief Finite policy for one authoring turn. Only one request and one tool operation may be active at a time. */
    struct AgentTurnLimits final {
        std::size_t maximumRounds{4};
        std::size_t maximumEvents{1024};
        std::size_t maximumToolCalls{8};
        std::size_t maximumHistoryBytes{256U << 10U};
        std::uint32_t maximumOutputTokens{4096};
        std::chrono::milliseconds timeout{30'000};
    };

    /** @brief One stable terminal outcome for the complete turn. */
    enum class AgentTurnStatus {
        Completed,
        Busy,
        InvalidRequest,
        InvalidModelResponse,
        BudgetExceeded,
        Cancelled,
        TimedOut,
        ProviderFailed,
        ToolDenied,
        ToolFailed,
        ContextDenied
    };

    /** @brief Complete response history on success and no partial conversational publication on failure. */
    struct AgentTurnResult final {
        AgentTurnStatus status{AgentTurnStatus::InvalidRequest};
        std::vector<ModelMessage> messages;
        std::optional<ModelErrorCode> providerError{};             /**< Structured provider failure without response text or credentials. */
        std::optional<Security::AgentPolicyReason> contextError{}; /**< Structured pre-dispatch context denial. */
    };

    /** @brief Optional consented evidence borrowed on the synchronous turn caller's Security policy owner lane. */
    struct AgentTurnContext final {
        const AgentContextPreview &preview;
        Security::AgentPolicy &policy;
        const Platform::ProjectReadContext &project;
        const std::function<bool(const AgentContextProvenance &)> &isCurrent;
        Security::AgentProviderResidence provider; /**< Actual host-confirmed residence of the passed provider. */
    };

    /** @brief Restricted editor-host bridge for already admitted local authoring tools. */
    class IAgentTurnTools {
    public:
        virtual ~IAgentTurnTools() = default;

        /** @brief Checks the tool's bounded argument shape before its round submits any operation. */
        [[nodiscard]] virtual bool ValidArguments(std::string_view arguments) const = 0;

        /** @brief Executes and settles one exact host-authorized tool call or returns its cancellation outcome.
         * @param turnId Host-issued turn identity in the captured tool session.
         * @param callOrdinal Unique sequential call identity within the turn.
         * @param intent Model's inert proposal, already checked against the host's advertised tool list.
         * @param deadline Absolute monotonic deadline inherited from the turn.
         * @param stop Cooperative cancellation for this exact turn.
         * @param result Filled only after the owner reports successful completion.
         * @return Completion or a typed terminal outcome.
         */
        [[nodiscard]] virtual AgentTurnStatus Run(std::uint64_t turnId, std::size_t callOrdinal, const ModelToolIntent &intent,
                                                  std::chrono::steady_clock::time_point deadline, std::stop_token stop,
                                                  std::string &result) const = 0;
    };

    /** @brief Editor-owned admission for at most one active authoring turn.
     * @details The owner must outlive its worker. The atomic flag protects turn admission across host callers and
     * rejects reentrant entry without blocking the provider, tool adapters or editor thread.
     */
    class AgentTurnRuntime final {
    public:
        /** @brief Executes a bounded authoring turn on a host worker while the existing owners pump MCP.
         * @param provider Exact host-selected provider; calls remain synchronous on this worker.
         * @param tools Host bridge to an independently authorized MCP session, or null for text-only turns.
         * @param turnId Unique nonzero host-issued turn identity within the admitted session generation.
         * @param request Initial conversation and host-admitted tools. A model cannot extend the list during the turn.
         * @param limits Host-approved finite budgets. Transient failures terminate the turn without automatic retry.
         * @param cancellation Host-owned signal observed cooperatively by the provider and tools.
         * @param context Optional exact preview already confirmed by the user. Its policy must be owned by this calling lane.
         * @return One terminal outcome. The tool controller retains authority to reject and cancel operations.
         */
        [[nodiscard]] AgentTurnResult Run(IModelProvider &provider, const IAgentTurnTools *tools, std::uint64_t turnId,
                                          ModelRequest request, const AgentTurnLimits &limits, std::stop_token cancellation = {},
                                          const AgentTurnContext *context = nullptr);

    private:
        std::atomic_flag active_{};
    };
}  // namespace Horo::Agent
