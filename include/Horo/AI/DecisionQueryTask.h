#pragma once

/** @file DecisionQueryTask.h
 * @brief Bound behavior-tree task that consumes a nonblocking tactical query at owner safe points.
 */

#include "Horo/AI/BehaviorTreeRuntime.h"
#include "Horo/AI/EnvironmentQueryExecution.h"

namespace Horo::AI {
    struct DecisionNodeConfiguration;

    /** @brief Inert authored node and exact query result/output declaration. */
    struct DecisionQueryTaskDescriptor final {
        DecisionNodeId node;
        TaskId task;
        QueryId query;
        QueryResultSchemaId resultSchema;
        std::uint32_t resultVersion{};
        QueryItemTypeId itemType;
        BlackboardKeyId output;
        std::uint64_t timeoutTicks{};
    };

    /** @brief Explicit host boundary for admitting existing staged query executions. */
    class IDecisionQueryTaskHost {
    public:
        virtual ~IDecisionQueryTaskHost() = default;
        /** @brief Captures the exact owner, query and input generations without dispatching provider work inline.
         * @param descriptor Admitted declaration. @param operation Exact owning task generation.
         * @param submittedTick Current decision tick. @param deadlineTick Last eligible completion tick.
         * @return Unique admitted execution or a typed failure. The host must outlive the executor.
         */
        [[nodiscard]] virtual Result<std::unique_ptr<QueryExecution>> Admit(const DecisionQueryTaskDescriptor &descriptor,
                                                                            const AiTaskOperationContext &operation,
                                                                            std::uint64_t submittedTick, std::uint64_t deadlineTick) = 0;
    };

    /** @brief One authored EQS task executor. All methods use the simulation owner lane.
     * @details The host advances existing EQS owner phases. Behavior-tree evaluation only observes publication.
     * The task stays running until its result commits at BlackboardSync and is visible to the next decision.
     */
    class DecisionQueryTask final : public IBehaviorTreeExecutor {
    public:
        /** @brief Validates result identity and a writable scalar destination before tree activation.
         * @param descriptor Inert node declaration. @param plan Admitted query plan.
         * @param registry Exact registry used to compile the query. @param tree Compiled decision tree and output bindings.
         * @param host Explicit admission adapter. @return Bound executor or typed schema/storage failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<DecisionQueryTask>> Bind(const DecisionQueryTaskDescriptor &descriptor,
                                                                             const EnvironmentQueryPlan &plan,
                                                                             const QuerySchemaRegistry &registry,
                                                                             const BehaviorTreeExecutionPlan &tree,
                                                                             IDecisionQueryTaskHost &host);

        /** @brief Binds a compiled task using its one explicit authored query output property.
         * @param configuration Authored properties of the exact compiled node. @param plan Admitted query plan.
         * @param registry Exact registry used to compile the query. @param tree Compiled decision tree and Blackboard bindings.
         * @param host Explicit query admission adapter. @return Bound task or a typed declaration or output failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<DecisionQueryTask>> BindAuthored(const DecisionNodeConfiguration &configuration,
                                                                                     const EnvironmentQueryPlan &plan,
                                                                                     const QuerySchemaRegistry &registry,
                                                                                     const BehaviorTreeExecutionPlan &tree,
                                                                                     IDecisionQueryTaskHost &host);

        /** @copydoc IBehaviorTreeExecutor::Start */
        [[nodiscard]] Result<AiTaskState> Start(const BehaviorTreeEvaluationContext &context,
                                                const AiTaskOperationContext &operation) noexcept override;
        /** @copydoc IBehaviorTreeExecutor::Resume */
        [[nodiscard]] Result<AiTaskState> Resume(const BehaviorTreeEvaluationContext &context,
                                                 const AiTaskOperationContext &operation) noexcept override;
        /** @copydoc IBehaviorTreeExecutor::Abort */
        void Abort(const AiTaskOperationContext &operation, AiTaskCancellationReason reason) noexcept override;
        /** @copydoc IBehaviorTreeExecutor::Cleanup */
        void Cleanup(const AiTaskOperationContext &operation, const AiTaskTerminalResult &result) noexcept override;
        /** @copydoc IBehaviorTreeExecutor::Check */
        [[nodiscard]] Result<bool> Check(const BehaviorTreeEvaluationContext &context) noexcept override;
        /** @copydoc IBehaviorTreeExecutor::Service */
        [[nodiscard]] Result<void> Service(const BehaviorTreeEvaluationContext &context) noexcept override;

        /** @brief Identifies the exact active result owner on the simulation lane.
         * @return Live-operation identity, or an empty handle when the query is idle or retired.
         */
        [[nodiscard]] AgentHandle ActiveAgent() const noexcept;
        /** @brief Delegates budgeted work to the admitted query after BlackboardSync. */
        void AdvanceQuery(std::uint64_t tick, QueryExecutionBudget budget);
        /** @brief Collects already available provider completion at BlackboardSync. */
        void SyncQuery(std::uint64_t tick);
        /** @brief Applies one eligible result to the exact active destination before the task succeeds.
         * @param tick Current BlackboardSync tick. @param scene Fresh destination Scene.
         * @param blackboard Exact destination admitted by the decision node.
         * @return True once on commit, false when no output is ready or the task retains a write failure, or a typed stale error.
         * @details A rejected Blackboard write is reported by the task's next Resume and is never retried.
         */
        [[nodiscard]] Result<bool> CommitOutput(std::uint64_t tick, const Runtime::RuntimeSceneView &scene, BlackboardInstance &blackboard);

    private:
        /** @brief Retains only typed admission evidence and the explicit host port. */
        DecisionQueryTask(DecisionQueryTaskDescriptor descriptor, BlackboardSchemaId schema, std::uint32_t schemaVersion,
                          BlackboardValueKind kind, IDecisionQueryTaskHost &host) noexcept;
        [[nodiscard]] bool Matches(const AiTaskOperationContext &operation) const noexcept;
        [[nodiscard]] bool Accepts(const BehaviorTreeEvaluationContext &context) const noexcept;
        /** @brief Transfers a rejected write into the next decision evaluation without failing BlackboardSync. */
        [[nodiscard]] Result<bool> RejectOutput(Error error);
        void Discard() noexcept;
        DecisionQueryTaskDescriptor descriptor_;
        BlackboardSchemaId schema_;
        std::uint32_t schemaVersion_;
        BlackboardValueKind kind_;
        IDecisionQueryTaskHost &host_;
        AiTaskOperationContext operation_;
        BlackboardInstanceBinding destination_;
        std::unique_ptr<QueryExecution> query_;
        std::optional<Error> outputFailure_;
        std::uint64_t deadline_{};
        std::uint64_t resolvedTick_{};
        bool ready_{};
        bool outputCommitted_{};
    };
}  // namespace Horo::AI
