#include "Horo/AI/DecisionQueryTask.h"

#include "Horo/AI/AIErrors.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::AI {
    namespace {
        template <typename T> Result<T> Failure(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Resolves the exact authored result and distinguishes supported native output kinds. */
        std::optional<BlackboardValueKind> OutputKind(const DecisionQueryTaskDescriptor &descriptor, const EnvironmentQueryPlan &plan,
                                                      const QuerySchemaRegistry &registry) {
            const auto &result = plan.ResultSchema();
            if (descriptor.query != plan.Id() || descriptor.resultSchema != result.id || descriptor.resultVersion != result.version ||
                descriptor.itemType != result.itemType)
                return std::nullopt;
            const auto *item = registry.Find(result.itemType);
            if (!item)
                return std::nullopt;
            if (item->kind == QueryItemKind::Point)
                return BlackboardValueKind::WorldCoordinate;
            if (item->kind == QueryItemKind::Actor)
                return BlackboardValueKind::EntityReference;
            return std::nullopt;
        }

        /** @brief Checks output declaration shared between the authored schema and the compiled node. */
        template <typename Binding> bool WritableScalar(const Binding &binding, const BlackboardValueKind kind) noexcept {
            return binding.kind == kind && binding.cardinality == BlackboardValueCardinality::Scalar &&
                   binding.access == BlackboardKeyAccess::ReadWrite;
        }

        /** @brief Converts only the admitted native winner types, with a fresh entity-generation fence. */
        Result<BlackboardScalarValue> WinnerValue(const QueryGeneratedItem &item, const Runtime::RuntimeSceneView &scene,
                                                  const BlackboardValueKind kind) {
            if (const auto *point = std::get_if<Math::WorldCoordinate64>(&item); point && kind == BlackboardValueKind::WorldCoordinate)
                return Result<BlackboardScalarValue>::Success(*point);
            if (const auto *entity = std::get_if<Runtime::EntityRef>(&item); entity && kind == BlackboardValueKind::EntityReference) {
                if (scene.Get(*entity).HasError())
                    return Failure<BlackboardScalarValue>(AIErrors::EnvironmentQueryContextStale);
                return Result<BlackboardScalarValue>::Success(
                    BlackboardStoredEntityReference{entity->runtime.value, entity->entity.index, entity->entity.generation});
            }
            return Failure<BlackboardScalarValue>(AIErrors::BlackboardValueTypeMismatch);
        }
    }  // namespace

    /** @copydoc DecisionQueryTask::Bind */
    Result<std::unique_ptr<DecisionQueryTask>> DecisionQueryTask::Bind(const DecisionQueryTaskDescriptor &descriptor,
                                                                       const EnvironmentQueryPlan &plan,
                                                                       const QuerySchemaRegistry &registry,
                                                                       const BehaviorTreeExecutionPlan &tree,
                                                                       IDecisionQueryTaskHost &host) {
        using Output = std::unique_ptr<DecisionQueryTask>;
        const auto kind = OutputKind(descriptor, plan, registry);
        if (!kind || !descriptor.node.IsValid() || !descriptor.task.IsValid() || !descriptor.output.IsValid() ||
            descriptor.timeoutTicks == 0)
            return Failure<Output>(AIErrors::EnvironmentQuerySchemaInvalid);
        const auto node = std::ranges::find(tree.Nodes(), descriptor.node, [](const auto &entry) {
            return entry.execution.id;
        });
        if (node == tree.Nodes().end() || node->execution.operation != BehaviorTreeOperation::Task ||
            node->execution.task != descriptor.task)
            return Failure<Output>(AIErrors::EnvironmentQuerySchemaInvalid);
        const auto keys = tree.Bindings().BindingsForNode(descriptor.node);
        const auto key = std::ranges::find(keys, descriptor.output, &DecisionPlanBlackboardBinding::key);
        if (key == keys.end() || !WritableScalar(*key, *kind))
            return Failure<Output>(AIErrors::BlackboardValueTypeMismatch);
        const auto schema = tree.Bindings().BlackboardSchema();
        try {
            return Result<Output>::Success(Output{new DecisionQueryTask(descriptor, schema->Identity(), schema->Version(), *kind, host)});
        } catch (const std::bad_alloc &) {
            return Failure<Output>(AIErrors::EnvironmentQueryLimitExceeded);
        }
    }

    /** @copydoc DecisionQueryTask::DecisionQueryTask */
    DecisionQueryTask::DecisionQueryTask(const DecisionQueryTaskDescriptor descriptor, const BlackboardSchemaId schema,
                                         const std::uint32_t schemaVersion, const BlackboardValueKind kind,
                                         IDecisionQueryTaskHost &host) noexcept
        : descriptor_(descriptor), schema_(schema), schemaVersion_(schemaVersion), kind_(kind), host_(host) {}

    bool DecisionQueryTask::Matches(const AiTaskOperationContext &operation) const noexcept {
        return query_ && operation.task == operation_.task && operation.agent == operation_.agent &&
               operation.taskDefinition == descriptor_.task;
    }

    bool DecisionQueryTask::Accepts(const BehaviorTreeEvaluationContext &context) const noexcept {
        return context.node == descriptor_.node && context.blackboard.Binding().schema == schema_ &&
               context.blackboard.Binding().schemaVersion == schemaVersion_ &&
               std::ranges::any_of(context.bindings, [this](const auto &binding) {
            return binding.key == descriptor_.output && WritableScalar(binding, kind_);
        });
    }

    void DecisionQueryTask::Discard() noexcept {
        if (query_)
            query_->Cancel();
        query_.reset();
        operation_ = {};
        resolvedTick_ = 0;
        ready_ = false;
        outputCommitted_ = false;
    }

    /** @copydoc DecisionQueryTask::Start */
    Result<AiTaskState> DecisionQueryTask::Start(const BehaviorTreeEvaluationContext &context,
                                                 const AiTaskOperationContext &operation) noexcept {
        if (operation.IsCancellationRequested())
            return Result<AiTaskState>::Success(AiTaskState::Cancelled);
        if (query_ || !Accepts(context) || operation.taskDefinition != descriptor_.task || !operation.task.IsValid() ||
            !operation.agent.IsValid() || operation.task.incarnation != operation.agent.incarnation ||
            context.blackboard.Binding().agent != operation.agent ||
            descriptor_.timeoutTicks >= std::numeric_limits<std::uint64_t>::max() - context.tick)
            return Failure<AiTaskState>(AIErrors::EnvironmentQueryContextInvalid);
        try {
            const auto deadline = context.tick + descriptor_.timeoutTicks;
            auto admitted = host_.Admit(descriptor_, operation, context.tick, deadline);
            // Admission can revoke the task's owner. Release the detached query before retaining any task state.
            if (operation.IsCancellationRequested())
                return Result<AiTaskState>::Success(AiTaskState::Cancelled);
            if (admitted.HasError())
                return Result<AiTaskState>::Failure(std::move(admitted).ErrorValue());
            auto query = std::move(admitted).Value();
            if (!query || !query->MatchesAdmission(descriptor_.query, operation.agent, context.tick, deadline))
                return Failure<AiTaskState>(AIErrors::EnvironmentQueryContextInvalid);
            operation_ = operation;
            destination_ = context.blackboard.Binding();
            deadline_ = deadline;
            query_ = std::move(query);
            return Result<AiTaskState>::Success(AiTaskState::Running);
        } catch (...) {
            return Failure<AiTaskState>(AIErrors::EnvironmentQueryProviderFailed);
        }
    }

    /** @copydoc DecisionQueryTask::Resume */
    Result<AiTaskState> DecisionQueryTask::Resume(const BehaviorTreeEvaluationContext &context,
                                                  const AiTaskOperationContext &operation) noexcept {
        if (!Matches(operation))
            return Result<AiTaskState>::Success(AiTaskState::Cancelled);
        const auto *result = query_->Published();
        if (operation.IsCancellationRequested() || !Accepts(context) || context.blackboard.Binding() != destination_ ||
            !query_->InputsAreCurrent()) {
            Discard();
            return Result<AiTaskState>::Success(AiTaskState::Cancelled);
        }
        if (outputCommitted_)
            return Result<AiTaskState>::Success(AiTaskState::Succeeded);
        if (ready_) {
            if (context.tick == resolvedTick_)
                return Result<AiTaskState>::Success(AiTaskState::Running);
            Discard();
            return Result<AiTaskState>::Success(AiTaskState::Cancelled);
        }
        if (context.tick > deadline_ || (context.tick == deadline_ && (!result || result->status == QueryExecutionStatus::TimedOut))) {
            Discard();
            return Result<AiTaskState>::Success(AiTaskState::Cancelled);
        }
        if (!result || result->publicationTick > context.tick)
            return Result<AiTaskState>::Success(AiTaskState::Running);
        if (result->status == QueryExecutionStatus::Cancelled || result->status == QueryExecutionStatus::StaleInputs ||
            result->status == QueryExecutionStatus::TimedOut)
            return Result<AiTaskState>::Success(AiTaskState::Cancelled);
        if (result->status != QueryExecutionStatus::Completed || result->winners.empty()) {
            try {
                if (result->error)
                    return Result<AiTaskState>::Failure(*result->error);
            } catch (const std::bad_alloc &) {
                return Failure<AiTaskState>(AIErrors::EnvironmentQueryLimitExceeded);
            }
            return Result<AiTaskState>::Success(AiTaskState::Failed);
        }
        const bool point = std::holds_alternative<Math::WorldCoordinate64>(result->winners.front().item);
        if (point != (kind_ == BlackboardValueKind::WorldCoordinate))
            return Failure<AiTaskState>(AIErrors::BlackboardValueTypeMismatch);
        resolvedTick_ = context.tick;
        ready_ = true;
        return Result<AiTaskState>::Success(AiTaskState::Running);
    }

    /** @copydoc DecisionQueryTask::Abort */
    void DecisionQueryTask::Abort(const AiTaskOperationContext &operation, AiTaskCancellationReason) noexcept {
        if (Matches(operation))
            Discard();
    }

    /** @copydoc DecisionQueryTask::Cleanup */
    void DecisionQueryTask::Cleanup(const AiTaskOperationContext &operation, const AiTaskTerminalResult &) noexcept {
        if (Matches(operation))
            Discard();
    }

    /** @copydoc DecisionQueryTask::Check */
    Result<bool> DecisionQueryTask::Check(const BehaviorTreeEvaluationContext &) noexcept {
        return Failure<bool>(AIErrors::EnvironmentQuerySchemaInvalid);
    }

    /** @copydoc DecisionQueryTask::Service */
    Result<void> DecisionQueryTask::Service(const BehaviorTreeEvaluationContext &) noexcept {
        return Failure<void>(AIErrors::EnvironmentQuerySchemaInvalid);
    }

    /** @copydoc DecisionQueryTask::AdvanceQuery */
    void DecisionQueryTask::AdvanceQuery(const std::uint64_t tick, const QueryExecutionBudget budget) {
        if (query_ && operation_.IsCancellationRequested())
            Discard();
        if (query_)
            query_->Advance(tick, budget);
    }

    /** @copydoc DecisionQueryTask::SyncQuery */
    void DecisionQueryTask::SyncQuery(const std::uint64_t tick) {
        if (query_ && operation_.IsCancellationRequested())
            Discard();
        if (query_)
            (void)query_->Sync(tick);
    }

    /** @copydoc DecisionQueryTask::CommitOutput */
    Result<bool> DecisionQueryTask::CommitOutput(const std::uint64_t tick, const Runtime::RuntimeSceneView &scene,
                                                 BlackboardInstance &blackboard) {
        if (!ready_ || tick <= resolvedTick_)
            return Result<bool>::Success(false);
        const auto &result = *query_->Published();
        if (tick != resolvedTick_ + 1 || operation_.IsCancellationRequested() || !query_->InputsAreCurrent() || !scene.IsCurrent() ||
            scene.RuntimeId() != result.scene || blackboard.Binding() != destination_) {
            Discard();
            return Failure<bool>(AIErrors::EnvironmentQueryContextStale);
        }
        auto value = WinnerValue(result.winners.front().item, scene, kind_);
        if (value.HasError()) {
            Discard();
            return Result<bool>::Failure(std::move(value).ErrorValue());
        }
        auto batch = blackboard.BeginWriteBatch();
        if (batch.HasError())
            return Result<bool>::Failure(std::move(batch).ErrorValue());
        auto write = std::move(batch).Value();
        if (auto staged = write.Stage({descriptor_.output, BlackboardValue{std::move(value).Value()}}); staged.HasError())
            return Result<bool>::Failure(std::move(staged).ErrorValue());
        // Hide the pending output while notifying observers, which may synchronously call back.
        const auto task = operation_.task;
        ready_ = false;
        auto committed = blackboard.CommitAtBlackboardSync(std::move(write));
        if (committed.HasError()) {
            ready_ = query_ && operation_.task == task;
            return Result<bool>::Failure(std::move(committed).ErrorValue());
        }
        outputCommitted_ = query_ && operation_.task == task;
        return Result<bool>::Success(true);
    }
}  // namespace Horo::AI
