#pragma once

#include "AgentContext.h"
#include "Horo/Agent/ModelProvider.h"

#include <atomic>
#include <chrono>
#include <functional>

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
        std::optional<Security::AgentPolicyReason> contextError{}; /**< Structured context admission or freshness denial. */
        std::optional<ModelUsage> finalUsage{};                    /**< Provider-reported counts for the final successful inference only. */
        std::optional<std::chrono::seconds> retryAfter{};         /**< Bounded retry hint for a throttled provider. */
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

    /** @brief Synchronous worker observer for accepted deltas from an inference with no tools. */
    using AgentTurnTextObserver = std::function<void(std::string_view)>;

    /** @brief Lifecycle of an admitted tool dispatch, with no model name, arguments or result payload. */
    struct AgentToolActivity final {
        std::size_t ordinal{};
        std::optional<AgentTurnStatus> outcome; /**< Empty immediately before dispatch, terminal when settled. */
    };

    /** @brief Synchronous worker observer for transient tool presentation. Exceptions are isolated from the turn. */
    using AgentToolActivityObserver = std::function<void(AgentToolActivity)>;

    /** @brief Cold caller's synchronous revocation check over an owned turn authority snapshot. */
    using AgentTurnFreshness = std::function<bool()>;

    /** @brief Host-declared effect of an inspected tool, independent of the model's reasoning. */
    enum class AgentPlannedEffect : std::uint8_t {
        Read,
        Presentation,
        Mutation
    };

    /** @brief Object identifiers scoped to the exact document revision named by a planned operation. */
    struct AgentPlanObjectScope final {
        std::uint64_t opening{};
        std::uint64_t revision{};
        std::vector<std::uint64_t> objects;
    };

    /** @brief Authoritative review projection for one inert model intent. */
    struct AgentPlannedTool final {
        AgentPlannedEffect effect{};
        std::optional<AgentPlanObjectScope> scope;
    };

    /** @brief Disposition of an inert proposal. Success requires an effect from the host. */
    struct AgentPlanToolInspection final {
        AgentTurnStatus status{AgentTurnStatus::ToolDenied};
        std::optional<AgentPlannedTool> tool;
    };

    /** @brief Validates one proposed tool against host policy. Builds no operation and consumes no approval. */
    using AgentPlanToolValidator = std::function<AgentPlanToolInspection(const ModelToolIntent &)>;

    /** @brief Monotonic elapsed times for the stages of an inert plan. */
    struct AgentPlanTiming final {
        std::chrono::steady_clock::duration inference{};                      /**< Provider call, including validated stream delivery. */
        std::optional<std::chrono::steady_clock::duration> toolInspection{}; /**< Host inspection when the draft contains tool intents. */
    };

    /** @brief One bounded inference for review. Draft intents carry neither execution authority nor tool results. */
    struct AgentTurnPlan final {
        AgentTurnStatus status{AgentTurnStatus::InvalidRequest};
        std::optional<ModelMessage> draft;
        std::optional<ModelErrorCode> providerError;
        std::optional<ModelUsage> usage;
        std::vector<AgentPlannedTool> tools; /**< One host result per draft intent, in the same order. */
        std::optional<Security::AgentPolicyReason> contextError;
        std::optional<std::chrono::seconds> retryAfter{}; /**< Bounded retry hint for a throttled provider. */
        std::optional<AgentPlanTiming> timing{};          /**< Elapsed stage timings for a complete draft. */
    };

    /** @brief Editor-owned admission for at most one active authoring turn.
     * @details The owner must outlive its worker. The atomic flag protects turn admission across host callers and
     * rejects reentrant entry without blocking the provider, tool adapters or editor thread.
     */
    class AgentTurnRuntime final {
    public:
        AgentTurnRuntime() = default;

        /** @brief Binds the runtime to a revocable owner snapshot for all its turns. */
        explicit AgentTurnRuntime(AgentTurnFreshness isCurrent);

        /** @brief Executes a bounded authoring turn on a host worker while the existing owners pump MCP.
         * @param provider Exact host-selected provider; calls remain synchronous on this worker.
         * @param tools Host bridge to an independently authorized MCP session, or null for text-only turns.
         * @param turnId Unique nonzero host-issued turn identity within the admitted session generation.
         * @param request Initial conversation and host-admitted tools. A model cannot extend the list during the turn.
         * @param limits Host-approved finite budgets. Transient failures terminate the turn without automatic retry.
         * @param cancellation Host-owned signal observed cooperatively by the provider and tools.
         * @param context Optional exact preview already confirmed by the user. Its policy must be owned by this calling lane.
         * @param observeText Optional ephemeral text observer. Requires a turn with no tool controller.
         * @param observeTool Optional observer for the actual admitted tool dispatch and terminal outcome.
         * @return One terminal outcome. The tool controller retains authority to reject and cancel operations.
         */
        [[nodiscard]] AgentTurnResult Run(IModelProvider &provider, const IAgentTurnTools *tools, std::uint64_t turnId,
                                          ModelRequest request, const AgentTurnLimits &limits, std::stop_token cancellation = {},
                                          const AgentTurnContext *context = nullptr, const AgentTurnTextObserver &observeText = {},
                                          const AgentToolActivityObserver &observeTool = {});

        /** @brief Continues an already timed tool turn without extending its deadline.
         * @param provider Exact host-selected provider. @param tools Independently authorized MCP session.
         * @param turnId Nonzero host-issued identity. @param request Turn with host-admitted tools.
         * @param limits Remaining non-time budgets and the runtime's maximum duration.
         * @param deadline Absolute limit established by the caller before preflight.
         * @param cancellation Host cancellation. @param observeTool Optional tool lifecycle observer.
         * @return Terminal turn outcome, including timeout before any dispatch if the deadline has passed.
         */
        [[nodiscard]] AgentTurnResult RunBefore(IModelProvider &provider, const IAgentTurnTools &tools, std::uint64_t turnId,
                                                ModelRequest request, const AgentTurnLimits &limits,
                                                std::chrono::steady_clock::time_point deadline, std::stop_token cancellation,
                                                const AgentToolActivityObserver &observeTool);

        /** @brief Runs a text-only turn under a revocable host scope.
         * @param provider Exact host-selected provider. @param turnId Nonzero host-issued identity.
         * @param request Bounded conversation without tools. @param limits Budget covering the entire turn.
         * @param cancellation Host cancellation.
         * @param providerAuthority Required thread-safe check over immutable or synchronized lifetime state.
         * @return Terminal turn result. The provider is signalled even when it is waiting without emitting events.
         */
        [[nodiscard]] AgentTurnResult RunScopedText(IModelProvider &provider, std::uint64_t turnId, ModelRequest request,
                                                    const AgentTurnLimits &limits, std::stop_token cancellation,
                                                    const AgentTurnFreshness &providerAuthority);

        /** @brief Collects one model round and validates every proposed tool without dispatching any.
         * @param provider Exact host-selected provider. @param turnId Nonzero host-issued identity.
         * @param request Conversation and host-approved tool definitions.
         * @param limits Budget covering the entire draft. @param validateTool Independent, side-effect-free admission.
         * @param cancellation Host cancellation. @param context Optional exact user-approved context on this lane.
         * @param providerAuthority Optional passive check used on the deadline thread to stop quiet providers. It must be
         * thread-safe, bounded, and must only read immutable or synchronized lifetime state.
         * @return A complete review draft or a typed failure with no draft.
         */
        [[nodiscard]] AgentTurnPlan Plan(IModelProvider &provider, std::uint64_t turnId, ModelRequest request,
                                         const AgentTurnLimits &limits, const AgentPlanToolValidator &validateTool,
                                         std::stop_token cancellation = {}, const AgentTurnContext *context = nullptr,
                                         const AgentTurnFreshness &providerAuthority = {});

    private:
        struct PlanReview final {
            const AgentPlanToolValidator &inspect;
            std::vector<AgentPlannedTool> tools;
            AgentPlanTiming timing;
        };

        struct TurnPorts final {
            const IAgentTurnTools *tools;
            const AgentTurnContext *context;
            const AgentTurnTextObserver &observeText;
            const AgentToolActivityObserver &observeTool;
            PlanReview *plan;
            const AgentTurnFreshness *providerAuthority{};
        };

        [[nodiscard]] AgentTurnResult RunTurn(
            IModelProvider &provider, std::uint64_t turnId, ModelRequest request, const AgentTurnLimits &limits,
            std::stop_token cancellation, const TurnPorts &ports,
            std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max());

        AgentTurnFreshness isCurrent_;
        std::atomic_flag active_{};
    };
}  // namespace Horo::Agent
