#include "HeadlessDecisionQueries.h"

#include "Horo/AI/AIErrors.h"
#include "Horo/AI/EnvironmentQueryCache.h"
#include "ProjectScenePerception.h"

#include <algorithm>
#include <bit>
#include <new>

namespace Horo::Application::Internal {
    namespace {
        using namespace AI;
        using Executor = Result<std::unique_ptr<IBehaviorTreeExecutor>>;
        using Execution = Result<std::unique_ptr<QueryExecution>>;

        /** @brief Retains the executor independently of the driver's weak phase inventory. */
        class OwnedQueryExecutor final : public IBehaviorTreeExecutor {
        public:
            explicit OwnedQueryExecutor(std::shared_ptr<DecisionQueryTask> task) : task_(std::move(task)) {}

            Result<AiTaskState> Start(const BehaviorTreeEvaluationContext &context,
                                      const AiTaskOperationContext &operation) noexcept override {
                return task_->Start(context, operation);
            }

            Result<AiTaskState> Resume(const BehaviorTreeEvaluationContext &context,
                                       const AiTaskOperationContext &operation) noexcept override {
                return task_->Resume(context, operation);
            }

            void Abort(const AiTaskOperationContext &operation, AiTaskCancellationReason reason) noexcept override {
                task_->Abort(operation, reason);
            }

            void Cleanup(const AiTaskOperationContext &operation, const AiTaskTerminalResult &result) noexcept override {
                task_->Cleanup(operation, result);
            }

            Result<bool> Check(const BehaviorTreeEvaluationContext &context) noexcept override {
                return task_->Check(context);
            }

            Result<void> Service(const BehaviorTreeEvaluationContext &context) noexcept override {
                return task_->Service(context);
            }

        private:
            std::shared_ptr<DecisionQueryTask> task_;
        };

        constexpr auto Candidates = EnvironmentQuerySchemaLimits::MaximumResults;
        constexpr auto AgentUnits = Candidates * (3U + std::bit_width(Candidates));
    }  // namespace

    /** @copydoc HeadlessDecisionQueries::Create */
    Result<std::shared_ptr<HeadlessDecisionQueries>> HeadlessDecisionQueries::Create(
        HeadlessQueryPlanCatalog plans, std::shared_ptr<const AI::QuerySchemaRegistry> registry,
        const AI::AiTaskSchedulerSettings &settings, const std::size_t workUnitsLimit,
        const std::optional<AI::QueryResultCacheLimits> cacheLimits) {
        using Created = Result<std::shared_ptr<HeadlessDecisionQueries>>;
        if (!registry || plans.empty() || settings.evaluationsPerTick == 0 ||
            settings.evaluationsPerTick > AI::AiTaskSchedulerSettings::HardAgents || workUnitsLimit == 0 ||
            workUnitsLimit > AI::AiTaskSchedulerSettings::HardWorkUnits)
            return Created::Failure(MakeError(AI::AIErrors::SchedulerPolicyInvalid));
        auto contexts = AI::QueryContextProviderRegistry::Capture({}, *registry);
        if (contexts.HasError())
            return Created::Failure(contexts.ErrorValue());
        try {
            auto owner = std::make_shared<HeadlessDecisionQueries>();
            owner->plans_ = std::move(plans);
            owner->registry_ = std::move(registry);
            owner->contexts_.emplace(std::move(contexts).Value());
            owner->maximumActive_ = settings.evaluationsPerTick;
            owner->workUnitsLimit_ = workUnitsLimit;
            if (settings.mode == AI::AiSchedulingMode::BestEffort)
                owner->cacheLimits_ = cacheLimits;
            owner->budgets_.resize(AI::MaximumAiSceneAgents);
            return Created::Success(std::move(owner));
        } catch (const std::bad_alloc &) {
            return Created::Failure(MakeError(AI::AIErrors::EnvironmentQueryLimitExceeded));
        }
    }

    /** @copydoc HeadlessDecisionQueries::BindQueryTask */
    Executor HeadlessDecisionQueries::BindQueryTask(const AI::BehaviorTreeExecutionPlan &tree,
                                                    const AI::DecisionNodeConfiguration &configuration) {
        if (scope_)
            return Executor::Failure(MakeError(AI::AIErrors::TaskContextInvalid));
        const auto property =
            std::ranges::find(configuration.properties, AI::BehaviorTreePropertyKind::QueryOutput, &AI::BehaviorTreeProperty::kind);
        const auto *output =
            property == configuration.properties.end() ? nullptr : std::get_if<AI::BehaviorTreeQueryOutput>(&property->value);
        if (!output)
            return Executor::Failure(MakeError(AI::AIErrors::EnvironmentQuerySchemaInvalid));
        const auto plan = plans_.find(output->query);
        if (plan == plans_.end())
            return Executor::Failure(MakeError(AI::AIErrors::EnvironmentQueryDescriptorUnavailable));
        if (tasks_.size() == AI::AiTaskSchedulerSettings::HardAgents) {
            std::erase_if(tasks_, [](const auto &task) {
                return task.expired();
            });
            if (tasks_.size() == AI::AiTaskSchedulerSettings::HardAgents)
                return Executor::Failure(MakeError(AI::AIErrors::EnvironmentQueryLimitExceeded));
        }
        auto task = AI::DecisionQueryTask::BindAuthored(configuration, *plan->second.plan, *registry_, tree, *this);
        if (task.HasError())
            return Executor::Failure(task.ErrorValue());
        auto retained = std::shared_ptr<AI::DecisionQueryTask>{std::move(task).Value()};
        auto executor = std::make_unique<OwnedQueryExecutor>(retained);
        tasks_.emplace_back(retained);
        return Executor::Success(std::move(executor));
    }

    /** @copydoc HeadlessDecisionQueries::Admit */
    Execution HeadlessDecisionQueries::Admit(const AI::DecisionQueryTaskDescriptor &descriptor, const AI::AiTaskOperationContext &operation,
                                             std::uint64_t submitted, std::uint64_t deadline) {
        if (!scope_ || submitted != scope_->tick)
            return Execution::Failure(MakeError(AI::AIErrors::TaskContextInvalid));
        if (scope_->active >= maximumActive_ || operation.agent.slot.index >= budgets_.size())
            return Execution::Failure(MakeError(AI::AIErrors::SchedulerBudgetExhausted));
        const auto plan = plans_.find(descriptor.query);
        if (plan == plans_.end())
            return Execution::Failure(MakeError(AI::AIErrors::EnvironmentQueryDescriptorUnavailable));
        const auto owner = scope_->runtime.ResolveDecisionQueryOwnerAtSafePoint(operation.agent);
        if (owner.HasError())
            return Execution::Failure(owner.ErrorValue());
        const auto position = CaptureProjectSceneWorldPosition(scope_->scene, owner.Value(), scope_->origin);
        if (position.HasError())
            return Execution::Failure(position.ErrorValue());
        AI::QueryContextCapture capture;
        const auto contexts =
            capture.Capture(*plan->second.plan, *registry_, *contexts_, scope_->scene,
                            {.querier = owner.Value(), .querierLocation = position.Value(), .executionRevision = submitted});
        if (contexts.HasError())
            return Execution::Failure(contexts.ErrorValue());
        auto admitted = AI::AdmitBuiltinPointQuery(*plan->second.plan, *registry_, scope_->scene,
                                                   {operation.agent, contexts.Value(), operation.ownerCancellation},
                                                   {.submittedTick = submitted,
                                                    .deadlineTick = deadline,
                                                    .maximumCandidates = Candidates,
                                                    .bestCount = 1},
                                                   {cache_, plan->second.digest});
        if (admitted.HasValue())
            ++scope_->active;
        return admitted;
    }

    /** @copydoc HeadlessDecisionQueries::PublishOutputs */
    Result<void> HeadlessDecisionQueries::PublishOutputs(TickScope &scope) {
        for (const auto &entry : tasks_) {
            const auto task = entry.lock();
            if (!task || !task->ActiveAgent().IsValid())
                continue;
            std::optional<AI::DecisionQueryPublication> publication;
            const auto observe = replayTick_ == scope.tick && replayAgent_ == task->ActiveAgent();
            const auto synced =
                scope.runtime.SyncDecisionQueryAtBlackboardSync(*task, scope.scene, scope.tick, observe ? &publication : nullptr);
            if (publication) {
                if (replayCount_ < replayPublications_.size())
                    replayPublications_[replayCount_++] = *publication;
                else
                    replayOverflow_ = true;
            }
            if (synced.HasError() && synced.ErrorValue().code.Value() != AI::AIErrors::EnvironmentQueryContextStale.code.Value())
                return Result<void>::Failure(synced.ErrorValue());
            scope.active += task->ActiveAgent().IsValid();
        }
        return Result<void>::Success();
    }

    /** @copydoc HeadlessDecisionQueries::ArmReplay */
    void HeadlessDecisionQueries::ArmReplay(const AI::AgentHandle agent, const std::uint64_t tick) noexcept {
        replayAgent_ = agent;
        replayTick_ = tick;
        replayCount_ = 0;
        replayOverflow_ = false;
    }

    /** @copydoc HeadlessDecisionQueries::ReadReplay */
    Result<std::span<const AI::DecisionQueryPublication>> HeadlessDecisionQueries::ReadReplay(const std::uint64_t tick) const {
        using Replay = Result<std::span<const AI::DecisionQueryPublication>>;
        if (tick == 0 || tick != replayTick_ || replayOverflow_)
            return Replay::Failure(MakeError(AI::AIErrors::EnvironmentQueryLimitExceeded));
        return Replay::Success(std::span{replayPublications_}.first(replayCount_));
    }

    /** @copydoc HeadlessDecisionQueries::ReplayProfile */
    Sha256Digest HeadlessDecisionQueries::ReplayProfile() const noexcept {
        Sha256Builder hash;
        for (const auto &entry : plans_)
            (void)hash.Update(std::as_bytes(std::span{entry.second.digest.bytes}));
        // The default keeps its digest for existing recordings. Non-default budgets add a canonical 32-bit suffix.
        if (workUnitsLimit_ != AI::AiTaskSchedulerSettings::HardWorkUnits) {
            const std::array encoded{std::byte(workUnitsLimit_ >> 24U), std::byte(workUnitsLimit_ >> 16U), std::byte(workUnitsLimit_ >> 8U),
                                     std::byte(workUnitsLimit_)};
            (void)hash.Update(encoded);
        }
        return hash.Finalize();
    }

    /** @copydoc HeadlessDecisionQueries::AdvanceQueries */
    void HeadlessDecisionQueries::AdvanceQueries(std::uint64_t tick) {
        if (tasks_.empty())
            return;
        AI::AiWorkBudget global{workUnitsLimit_};
        for (std::size_t visited = 0; visited < tasks_.size(); ++visited) {
            const auto task = tasks_[(cursor_ + visited) % tasks_.size()].lock();
            if (!task)
                continue;
            const auto agent = task->ActiveAgent();
            if (!agent.IsValid())
                continue;
            auto &budget = budgets_[agent.slot.index];
            if (budget.tick != tick)
                budget = {tick, AgentUnits};
            AI::AiWorkBudget query{AgentUnits};
            AI::AiWorkBudget agentWork{budget.remaining};
            task->AdvanceQuery(tick, {query, agentWork, global});
            budget.remaining = agentWork.Remaining();
        }
        cursor_ = (cursor_ + 1) % tasks_.size();
    }

    /** @copydoc HeadlessDecisionQueries::RunAtFixedTick */
    Result<void> HeadlessDecisionQueries::RunAtFixedTick(AI::AiSceneRuntime &runtime, const Runtime::RuntimeSceneView &scene,
                                                         Math::WorldCoordinate64 origin, std::uint64_t tick) {
        if (scope_ || !scene.IsCurrent() || tick == 0)
            return Result<void>::Failure(MakeError(AI::AIErrors::TaskContextInvalid));
        if (auto prepared = PrepareCache(scene); prepared.HasError())
            return prepared;
        std::erase_if(tasks_, [](const auto &task) {
            return task.expired();
        });
        TickScope current{runtime, scene, origin, tick};

        struct Reset final {
            TickScope *&slot;

            ~Reset() {
                slot = nullptr;
            }
        } reset{scope_};

        scope_ = &current;
        const auto scheduler = runtime.TaskSchedulerAtSafePoint();
        if (scheduler.HasError())
            return Result<void>::Failure(scheduler.ErrorValue());
        if (auto published = PublishOutputs(current); published.HasError())
            return published;
        if (const auto evaluated = scheduler.Value()->EvaluateAtDecision(tick); evaluated.HasError())
            return Result<void>::Failure(evaluated.ErrorValue());
        if (const auto committed = scheduler.Value()->CommitAtIntentDispatch(tick); committed.HasError())
            return Result<void>::Failure(committed.ErrorValue());
        AdvanceQueries(tick);
        return Result<void>::Success();
    }

    Result<void> HeadlessDecisionQueries::PrepareCache(const Runtime::RuntimeSceneView &scene) {
        if (!cacheLimits_ || (cache_ && cache_->IsFor(scene)))
            return Result<void>::Success();
        cache_.reset();
        auto created = AI::QueryResultCache::Create(scene, *cacheLimits_);
        if (created.HasError())
            return Result<void>::Failure(created.ErrorValue());
        try {
            cache_ = std::make_shared<AI::QueryResultCache>(std::move(created).Value());
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(AI::AIErrors::EnvironmentQueryLimitExceeded));
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Application::Internal
