#pragma once

/**
 * @file EnvironmentQueryExecution.h
 * @brief Owner-thread staged execution of one adaptive tactical query.
 */

#include "Horo/AI/AITaskScheduler.h"
#include "Horo/AI/EnvironmentQueryGenerators.h"
#include "Horo/AI/EnvironmentQueryScoring.h"

#include <memory>

namespace Horo::AI {
    /** @brief Exact owner and immutable query context passed to the host adapter. */
    struct QueryExecutionInputs final {
        AgentHandle agent;
        std::shared_ptr<const QueryContextSnapshot> contexts;
    };

    /** @brief One query's host-composed generator and asynchronous test adapter.
     * @details All methods run serially on the scene owner lane. Implementations may enqueue detached work
     * on the appropriate provider owner, retaining copied inputs and leases until retirement. Calls must
     * be bounded, nonblocking and must never reenter the query executor.
     */
    class IQueryExecutionSource {
    public:
        virtual ~IQueryExecutionSource() = default;

        /** @brief Checks the exact agent, scene and provider generations pinned by this adapter.
         * @param inputs The original submission. @return Whether all captured inputs can still be honored.
         */
        [[nodiscard]] virtual bool IsCurrent(const QueryExecutionInputs &inputs) const noexcept = 0;
        /** @brief Generates a bounded batch using the captured query inputs.
         * @param inputs The original submission. @param output Preallocated bounded storage.
         * @return Generated count and provenance or a typed provider error.
         */
        [[nodiscard]] virtual Result<QueryGenerationResult> Generate(const QueryExecutionInputs &inputs,
                                                                     const QueryGeneratorOutput &output) = 0;
        /** @brief Attempts admission of one test at the adapter's provider boundary.
         * @param inputs The original submission. @param stage Exact admitted stage. @param item Candidate value.
         * @return True transfers ownership of one request to this adapter. False means backpressure.
         * @post A false result, error or exception leaves no new request. No borrowed input survives the call.
         */
        [[nodiscard]] virtual Result<bool> StartTest(const QueryExecutionInputs &inputs, QueryStageId stage,
                                                     const QueryGeneratedItem &item) = 0;
        /** @brief Polls an admitted request at BlackboardSync without waiting.
         * @return Empty keeps the request pending. A sample or error retires the request.
         * @post A thrown exception leaves the request owned and cancellable.
         */
        [[nodiscard]] virtual Result<std::optional<QueryTestSample>> PollTest() = 0;
        /** @brief Revokes the outstanding request and retires its owner lease without waiting. */
        virtual void CancelTest() noexcept = 0;
    };

    /** @brief Finite settings for one adaptive query. */
    struct QueryExecutionSettings final {
        std::uint64_t submittedTick{};
        std::uint64_t deadlineTick{};
        std::size_t maximumCandidates{};
        std::size_t bestCount{};
        std::size_t generatorWorkUnits{};
        bool allowPartial{};
    };

    /** @brief Caller-issued per-tick allowances. Sharing the same allowance charges it once. */
    struct QueryExecutionBudget final {
        AiWorkBudget &query;
        AiWorkBudget &agent;
        AiWorkBudget &global;
    };

    /** @brief Terminal outcomes published once at BlackboardSync. */
    enum class QueryExecutionStatus : std::uint8_t {
        Completed,
        PartialSuccess,
        TimedOut,
        Cancelled,
        StaleInputs,
        Failed,
    };

    /** @brief Copied ranked item with its stable index and score. */
    struct QueryExecutionWinner final {
        QueryGeneratedItem item;
        QueryCandidateScore score;
    };

    /** @brief Owned terminal snapshot. Failed, stale and cancelled results carry no winners. */
    struct QueryExecutionResult final {
        QueryExecutionStatus status{};
        AgentHandle agent;
        Runtime::SceneRuntimeId scene;
        std::uint64_t contextRevision{};
        QueryGeneratorProvenance generator;
        std::size_t generated{};
        std::size_t evaluated{};
        std::size_t invalidCoordinates{};
        std::vector<QueryExecutionWinner> winners;
        std::optional<Error> error;
    };

    /** @brief Scene-owner execution of one admitted adaptive query with at most one outstanding provider request.
     * @details Admit during AiDecisionEvaluate, then call Sync and Advance on later ticks in that order.
     * The caller owns scheduling across queries and issues fresh allowances once per tick. Destruction,
     * cancellation and publication all run on the same owner lane.
     */
    class QueryExecution final {
    public:
        /** @brief Preallocates bounded adaptive single-generator storage with stable-index tie breaking.
         * @param inputs Pinned owner and context. @param provenance Exact generator provenance.
         * @param scoring Captured scoring rules. @param settings Finite admission limits.
         * @param source Query-exclusive host adapter.
         * @return Owned execution or a typed invalid or limit failure.
         * @throws std::bad_alloc If bounded storage cannot be allocated.
         */
        [[nodiscard]] static Result<std::unique_ptr<QueryExecution>> Admit(QueryExecutionInputs inputs, QueryGeneratorProvenance provenance,
                                                                           QueryScoringPlan scoring, QueryExecutionSettings settings,
                                                                           std::unique_ptr<IQueryExecutionSource> source);

        QueryExecution(const QueryExecution &) = delete;
        QueryExecution &operator=(const QueryExecution &) = delete;
        /** @brief Cancels any accepted provider work before releasing the adapter. */
        ~QueryExecution();

        /** @brief Advances admitted generation and test stages after BlackboardSync.
         * @param tick Current simulation tick. @param budget Query, agent and global allowances for this tick.
         */
        void Advance(std::uint64_t tick, QueryExecutionBudget budget);
        /** @brief Drains completions and publishes at most one terminal result.
         * @param tick Current simulation tick. @param finishEarly Explicit early finish request.
         * @return Whether this call published the terminal result.
         */
        [[nodiscard]] bool Sync(std::uint64_t tick, bool finishEarly = false);
        /** @brief Requests cancellation. Accepted work is revoked immediately on the owner lane. */
        void Cancel() noexcept;
        /** @brief Reads the immutable terminal result. @return Null until publication. */
        [[nodiscard]] const QueryExecutionResult *Published() const noexcept;

    private:
        QueryExecution(QueryExecutionInputs inputs, QueryGeneratorProvenance provenance, QueryScoringPlan scoring,
                       QueryExecutionSettings settings, std::unique_ptr<IQueryExecutionSource> source);
        void Generate();
        void ScoreCandidate();
        void Poll();
        void StopTest() noexcept;
        void Publish(QueryExecutionStatus status);

        QueryExecutionInputs inputs_;
        QueryScoringPlan scoring_;
        QueryExecutionSettings settings_;
        std::unique_ptr<IQueryExecutionSource> source_;
        std::vector<QueryGeneratedItem> items_;
        std::vector<QueryTestSample> samples_;
        std::vector<std::size_t> testOrder_;
        std::vector<QueryCandidateScore> scores_;
        QueryExecutionResult result_;
        std::uint64_t lastSync_{};
        std::uint64_t lastAdvance_{};
        std::size_t nextTest_{};
        bool generated_{};
        bool pending_{};
        bool cancelled_{};
        bool stale_{};
        bool published_{};
        bool scoringReserved_{};
    };
}  // namespace Horo::AI
