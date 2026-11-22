#include "Horo/AI/DecisionQueryTask.h"

#include "Horo/AI/AIErrors.h"
#include "Horo/AI/DecisionNodeRegistration.h"

#include <algorithm>
#include <limits>
#include <new>

namespace Horo::AI {
    namespace {
        template <typename T> Result<T> Failure(const ErrorCodeDescriptor &error) {
            return Result<T>::Failure(MakeError(error));
        }

        /** @brief Copies the retained cause through the task callback's no-throw boundary. */
        Result<AiTaskState> TaskFailure(const Error &error) noexcept {
            try {
                return Result<AiTaskState>::Failure(error);
            } catch (const std::bad_alloc &) {
                return Failure<AiTaskState>(AIErrors::EnvironmentQueryLimitExceeded);
            }
        }

        /** @brief Resolves only a compiled task with the given semantic identity. */
        const BehaviorTreeExecutionNode *FindTask(const BehaviorTreeExecutionPlan &tree, DecisionNodeId id) {
            const auto node = std::ranges::find(tree.Nodes(), id, [](const auto &entry) {
                return entry.execution.id;
            });
            if (node == tree.Nodes().end() || node->execution.operation != BehaviorTreeOperation::Task)
                return nullptr;
            return &node->execution;
        }

        /** @brief Accepts exactly one explicitly authored query output from the task properties. */
        const BehaviorTreeQueryOutput *FindAuthoredOutput(std::span<const BehaviorTreeProperty> properties) {
            const BehaviorTreeQueryOutput *output{};
            for (const auto &property : properties) {
                if (property.kind != BehaviorTreePropertyKind::QueryOutput)
                    continue;
                if (output || !property.id.IsValid())
                    return nullptr;
                output = std::get_if<BehaviorTreeQueryOutput>(&property.value);
                if (!output || !output->IsValid())
                    return nullptr;
            }
            return output;
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
        /** @brief Detaches stable task metadata and the first ranked item before observer reentry. */
        DecisionQueryPublication ObservePublication(const DecisionQueryTaskDescriptor &descriptor, const QueryExecutionResult &result) {
            return {descriptor,
                    result.status,
                    result.mode,
                    result.publicationTick,
                    result.contextRevision,
                    result.generator,
                    result.generated,
                    result.evaluated,
                    result.invalidCoordinates,
                    result.winners.empty() ? std::nullopt : std::optional{result.winners.front()},
                    false,
                    result.failure};
        }

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
        if (!kind.has_value() || !descriptor.output.IsValid() || descriptor.timeoutTicks == 0)
            return Failure<Output>(AIErrors::EnvironmentQuerySchemaInvalid);
        if (const auto *node = FindTask(tree, descriptor.node); !node || node->task != descriptor.task)
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

    /** @copydoc DecisionQueryTask::BindAuthored */
    Result<std::unique_ptr<DecisionQueryTask>> DecisionQueryTask::BindAuthored(const DecisionNodeConfiguration &configuration,
                                                                               const EnvironmentQueryPlan &plan,
                                                                               const QuerySchemaRegistry &registry,
                                                                               const BehaviorTreeExecutionPlan &tree,
                                                                               IDecisionQueryTaskHost &host) {
        const auto *node = FindTask(tree, configuration.node);
        const auto *output = FindAuthoredOutput(configuration.properties);
        if (!node || !output)
            return Failure<std::unique_ptr<DecisionQueryTask>>(AIErrors::EnvironmentQuerySchemaInvalid);
        return Bind({configuration.node, node->task, output->query, output->result, output->resultVersion, output->item, output->key,
                     output->timeoutTicks},
                    plan, registry, tree, host);
    }

    /** @copydoc DecisionQueryTask::DecisionQueryTask */
    DecisionQueryTask::DecisionQueryTask(const DecisionQueryTaskDescriptor descriptor, const BlackboardSchemaId schema,
                                         const std::uint32_t schemaVersion, const BlackboardValueKind kind,
                                         IDecisionQueryTaskHost &host) noexcept
        : descriptor_(descriptor), schema_(schema), schemaVersion_(schemaVersion), kind_(kind), host_(host) {}

    bool DecisionQueryTask::Matches(const AiTaskOperationContext &operation) const noexcept {
        return operation_.task.IsValid() && operation.task == operation_.task && operation.agent == operation_.agent &&
               operation.taskDefinition == descriptor_.task;
    }

    bool DecisionQueryTask::Accepts(const BehaviorTreeEvaluationContext &context) const noexcept {
        return context.node == descriptor_.node && context.blackboard.Binding().schema == schema_ &&
               context.blackboard.Binding().schemaVersion == schemaVersion_ &&
               std::ranges::any_of(context.bindings, [this](const auto &binding) {
            return binding.key == descriptor_.output && WritableScalar(binding, kind_);
        });
    }

    DecisionQueryTask::ProviderCall::ProviderCall(DecisionQueryTask &owner) noexcept : owner_(owner) {
        owner_.providerCall_ = true;
    }

    DecisionQueryTask::ProviderCall::~ProviderCall() {
        owner_.providerCall_ = false;
        if (owner_.discardRequested_)
            owner_.Discard();
    }

    bool DecisionQueryTask::InputsCurrent() noexcept {
        const ProviderCall call{*this};
        return query_->InputsAreCurrent() && !discardRequested_ && !operation_.IsCancellationRequested();
    }

    void DecisionQueryTask::Discard() noexcept {
        if (providerCall_) {
            discardRequested_ = true;
            return;
        }
        const ProviderCall call{*this};
        discardRequested_ = false;
        auto retired = std::move(query_);
        operation_ = {};
        continuation_ = {};
        wakePending_ = false;
        resolvedTick_ = 0;
        ready_ = false;
        outputCommitted_ = false;
        outputFailure_.reset();
        if (retired)
            retired->Cancel();
    }

    /** @copydoc DecisionQueryTask::Start */
    Result<AiTaskState> DecisionQueryTask::Start(const BehaviorTreeEvaluationContext &context,
                                                 const AiTaskOperationContext &operation) noexcept {
        if (operation.IsCancellationRequested())
            return Result<AiTaskState>::Success(AiTaskState::Cancelled);
        if (providerCall_ || query_ || !Accepts(context) || operation.taskDefinition != descriptor_.task || !operation.task.IsValid() ||
            !operation.agent.IsValid() || operation.task.incarnation != operation.agent.incarnation ||
            context.blackboard.Binding().agent != operation.agent ||
            descriptor_.timeoutTicks >= std::numeric_limits<std::uint64_t>::max() - context.tick)
            return Failure<AiTaskState>(AIErrors::EnvironmentQueryContextInvalid);
        const ProviderCall call{*this};
        operation_ = operation;
        try {
            const auto deadline = context.tick + descriptor_.timeoutTicks;
            auto admitted = host_.Admit(descriptor_, operation, context.tick, deadline);
            // Admission can revoke the task's owner. Release the detached query before retaining any task state.
            if (discardRequested_ || operation.IsCancellationRequested()) {
                Discard();
                return Result<AiTaskState>::Success(AiTaskState::Cancelled);
            }
            if (admitted.HasError()) {
                Discard();
                return Result<AiTaskState>::Failure(std::move(admitted).ErrorValue());
            }
            auto query = std::move(admitted).Value();
            if (!query || !query->MatchesAdmission(descriptor_.query, operation.agent, context.tick, deadline)) {
                Discard();
                return Failure<AiTaskState>(AIErrors::EnvironmentQueryContextInvalid);
            }
            continuation_ = context.continuation;
            destination_ = context.blackboard.Binding();
            deadline_ = deadline;
            query_ = std::move(query);
            return Result<AiTaskState>::Success(AiTaskState::Running);
        } catch (...) {
            const bool cancelled = discardRequested_ || operation.IsCancellationRequested();
            Discard();
            if (cancelled)
                return Result<AiTaskState>::Success(AiTaskState::Cancelled);
            return Failure<AiTaskState>(AIErrors::EnvironmentQueryProviderFailed);
        }
    }

    /** @copydoc DecisionQueryTask::Resume */
    Result<AiTaskState> DecisionQueryTask::Resume(const BehaviorTreeEvaluationContext &context,
                                                  const AiTaskOperationContext &operation) noexcept {
        if (providerCall_ || !query_ || !Matches(operation))
            return Result<AiTaskState>::Success(AiTaskState::Cancelled);
        if (operation.IsCancellationRequested() || !Accepts(context) || context.blackboard.Binding() != destination_ || !InputsCurrent()) {
            Discard();
            return Result<AiTaskState>::Success(AiTaskState::Cancelled);
        }
        const auto *result = query_->Published();
        if (outputCommitted_)
            return Result<AiTaskState>::Success(AiTaskState::Succeeded);
        if (outputFailure_)
            return TaskFailure(*outputFailure_);
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
            if (result->error)
                return TaskFailure(*result->error);
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

    /** @copydoc DecisionQueryTask::ActiveAgent */
    AgentHandle DecisionQueryTask::ActiveAgent() const noexcept {
        return query_ && !discardRequested_ && !operation_.IsCancellationRequested() ? operation_.agent : AgentHandle{};
    }

    /** @copydoc DecisionQueryTask::AdvanceQuery */
    void DecisionQueryTask::AdvanceQuery(const std::uint64_t tick, const QueryExecutionBudget budget) {
        if (providerCall_ || !query_)
            return;
        const ProviderCall call{*this};
        if (!operation_.IsCancellationRequested())
            query_->Advance(tick, budget);
        if (operation_.IsCancellationRequested())
            Discard();
    }

    /** @copydoc DecisionQueryTask::InspectQuery */
    std::optional<QueryDebugSnapshot> DecisionQueryTask::InspectQuery(QueryId query, const Runtime::RuntimeSceneView &scene) const {
        return query_ && descriptor_.query == query && !providerCall_ ? query_->ReadDebugSnapshot(scene) : std::nullopt;
    }

    /** @copydoc DecisionQueryTask::SyncQuery */
    void DecisionQueryTask::SyncQuery(const std::uint64_t tick, std::optional<DecisionQueryPublication> *publication) {
        if (providerCall_ || !query_)
            return;
        const ProviderCall call{*this};
        const bool published = !operation_.IsCancellationRequested() && query_->Sync(tick);
        if (discardRequested_ || operation_.IsCancellationRequested()) {
            Discard();
            return;
        }
        if (published) {
            wakePending_ = true;
            if (publication)
                *publication = ObservePublication(descriptor_, *query_->Published());
        }
        FlushWake();
    }

    /** @copydoc DecisionQueryTask::FlushWake */
    void DecisionQueryTask::FlushWake() noexcept {
        if (wakePending_)
            wakePending_ = continuation_.NotifyEvent() == AiTaskPublicationDisposition::Contended;
    }

    /** @copydoc DecisionQueryTask::RejectOutput */
    Result<bool> DecisionQueryTask::RejectOutput(Error error) {
        ready_ = false;
        outputFailure_.emplace(std::move(error));
        wakePending_ = true;
        FlushWake();
        return Result<bool>::Success(false);
    }

    /** @copydoc DecisionQueryTask::CommitOutput */
    Result<bool> DecisionQueryTask::CommitOutput(const std::uint64_t tick, const Runtime::RuntimeSceneView &scene,
                                                 BlackboardInstance &blackboard, std::optional<DecisionQueryPublication> *publication) {
        auto prepared = PrepareOutput(tick, scene);
        if (prepared.HasError() || !prepared.Value())
            return prepared;
        return CommitPreparedOutput(scene, blackboard, publication);
    }

    Result<bool> DecisionQueryTask::PrepareOutput(const std::uint64_t tick, const Runtime::RuntimeSceneView &scene) {
        if (providerCall_ || !ready_ || tick <= resolvedTick_)
            return Result<bool>::Success(false);
        if (tick != resolvedTick_ + 1 || operation_.IsCancellationRequested() || !InputsCurrent() || !scene.IsCurrent() ||
            scene.RuntimeId() != query_->Published()->scene) {
            Discard();
            return Failure<bool>(AIErrors::EnvironmentQueryContextStale);
        }
        return Result<bool>::Success(true);
    }

    Result<bool> DecisionQueryTask::CommitPreparedOutput(const Runtime::RuntimeSceneView &scene, BlackboardInstance &blackboard,
                                                         std::optional<DecisionQueryPublication> *publication) {
        if (operation_.IsCancellationRequested() || blackboard.Binding() != destination_) {
            Discard();
            return Failure<bool>(AIErrors::EnvironmentQueryContextStale);
        }
        const auto &result = *query_->Published();
        auto value = WinnerValue(result.winners.front().item, scene, kind_);
        if (value.HasError()) {
            Discard();
            return Result<bool>::Failure(std::move(value).ErrorValue());
        }
        auto batch = blackboard.BeginWriteBatch();
        if (batch.HasError())
            return RejectOutput(std::move(batch).ErrorValue());
        auto write = std::move(batch).Value();
        if (auto staged = write.Stage({descriptor_.output, BlackboardValue{std::move(value).Value()}, query_->OutputAuthorization()});
            staged.HasError())
            return RejectOutput(std::move(staged).ErrorValue());
        // Hide the pending output while notifying observers, which may synchronously call back.
        const auto task = operation_.task;
        auto detached = publication ? std::optional{ObservePublication(descriptor_, result)} : std::nullopt;
        ready_ = false;
        auto committed = blackboard.CommitAtBlackboardSync(std::move(write));
        if (detached && committed.HasValue()) {
            detached->blackboardCommitted = true;
            *publication = std::move(detached);
        }
        if (!query_ || operation_.task != task)
            return Result<bool>::Success(committed.HasValue());
        if (committed.HasError())
            return RejectOutput(std::move(committed).ErrorValue());
        outputCommitted_ = true;
        wakePending_ = true;
        FlushWake();
        return Result<bool>::Success(true);
    }
}  // namespace Horo::AI
