#include "HeadlessDecisionQueries.h"

#include "HeadlessNavigationPathScope.h"
#include "Horo/AI/AIErrors.h"
#include "Horo/AI/EnvironmentQueryCache.h"
#include "Horo/AI/EnvironmentQueryPerception.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "ProjectScenePerception.h"
#include "ScenePhysicsHost.h"

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

        using Visibility = Result<std::optional<AI::QueryPhysicsVisibilityResult>>;

        /** @brief Leaves competing batches untouched and drains its own result before the Physics tick advances. */
        Visibility RunVisibilityBatch(Physics::PhysicsWorld &world, const Physics::PhysicsQueryEventCapability &capability,
                                      const AI::QueryPhysicsPublication &publication, AI::QueryStageId stage,
                                      std::span<const Physics::PhysicsRayQuery> rays, Physics::PhysicsQueryFilter filter) {
            AI::QueryPhysicsProbe probe;
            const auto admitted = probe.StartVisibility(capability, publication, stage, rays, filter);
            if (admitted.HasError())
                return Visibility::Failure(admitted.ErrorValue());
            if (!admitted.Value())
                return Visibility::Success(std::nullopt);
            // Drain this admitted batch while its exact tick still owns the native query publication.
            if (const auto processed = world.ProcessQueryBatch(); processed.HasError())
                return Visibility::Failure(processed.ErrorValue());
            const auto completed = probe.PollVisibility(publication);
            if (completed.HasError())
                return Visibility::Failure(completed.ErrorValue());
            if (!completed.Value())
                return Visibility::Failure(MakeError(AI::AIErrors::EnvironmentQueryProviderFailed));
            return completed;
        }

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
            owner->bestEffort_ = settings.mode == AI::AiSchedulingMode::BestEffort;
            if (owner->bestEffort_)
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
        if (!bestEffort_ && (AI::UsesBuiltinPointVisibility(*plan->second.plan) || AI::UsesBuiltinPointNavigation(*plan->second.plan)))
            return Executor::Failure(MakeError(AI::AIErrors::EnvironmentQueryStageUnsupported));
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
        auto admitted = AdmitPlan(plan->second, operation, submitted, deadline);
        if (admitted.HasValue())
            ++scope_->active;
        return admitted;
    }

    /** @copydoc HeadlessDecisionQueries::AdmitPlan */
    Execution HeadlessDecisionQueries::AdmitPlan(const CookedSceneQueryPlan &plan, const AiTaskOperationContext &operation,
                                                 const std::uint64_t submitted, const std::uint64_t deadline) {
        const auto owner = scope_->runtime.ResolveDecisionQueryOwnerAtSafePoint(operation.agent);
        if (owner.HasError())
            return Execution::Failure(owner.ErrorValue());
        AI::QueryContextCapture capture;
        AI::QueryContextCaptureSource source{.querier = owner.Value(), .executionRevision = submitted};
        const bool perception = AI::UsesBuiltinPerceptionQuery(*plan.plan);
        if (!perception) {
            const auto position = CaptureProjectSceneWorldPosition(scope_->scene, owner.Value(), scope_->origin);
            if (position.HasError())
                return Execution::Failure(position.ErrorValue());
            source.querierLocation = position.Value();
        }
        const auto contexts = capture.Capture(*plan.plan, *registry_, *contexts_, scope_->scene, source);
        if (contexts.HasError())
            return Execution::Failure(contexts.ErrorValue());
        AI::QueryExecutionInputs inputs{operation.agent, contexts.Value(), operation.ownerCancellation};
        const AI::QueryExecutionSettings settings{.submittedTick = submitted,
                                                  .deadlineTick = deadline,
                                                  .maximumCandidates = Candidates,
                                                  .bestCount = 1};
        if (AI::UsesBuiltinPointVisibility(*plan.plan)) {
            if (AI::UsesBuiltinPointNavigation(*plan.plan))
                return AdmitMixedPointPlan(*plan.plan, owner.Value(), std::move(inputs), settings);
            return AI::AdmitBuiltinPointVisibilityQuery(*plan.plan, *registry_, scope_->scene, owner.Value(), std::move(inputs), settings,
                                                        *this);
        }
        if (AI::UsesBuiltinPointNavigation(*plan.plan))
            return AI::AdmitBuiltinPointNavigationQuery(*plan.plan, *registry_, scope_->scene, std::move(inputs), settings, *this);
        if (!perception)
            return AI::AdmitBuiltinPointQuery(*plan.plan, *registry_, scope_->scene, std::move(inputs), settings, {cache_, plan.digest});

        auto observations = scope_->runtime.CaptureQueryPerceptionAtSafePoint(scope_->scene, operation.agent);
        if (observations.HasError())
            return Execution::Failure(std::move(observations).ErrorValue());
        return AI::AdmitBuiltinPerceptionQuery(*plan.plan, *registry_, std::move(inputs), std::move(observations).Value(), settings);
    }

    /** @copydoc HeadlessDecisionQueries::AdmitMixedPointPlan */
    Execution HeadlessDecisionQueries::AdmitMixedPointPlan(const AI::EnvironmentQueryPlan &plan, Runtime::EntityRef querier,
                                                           AI::QueryExecutionInputs inputs, AI::QueryExecutionSettings settings) {
        auto visibility = AI::PrepareBuiltinPointVisibilityTest(plan, *registry_, scope_->scene, querier, inputs, settings, *this);
        if (visibility.HasError())
            return Execution::Failure(visibility.ErrorValue());
        auto navigation = AI::PrepareBuiltinPointNavigationTest(plan, *registry_, scope_->scene, inputs, settings, *this);
        if (navigation.HasError())
            return Execution::Failure(navigation.ErrorValue());
        std::array tests{std::move(visibility).Value(), std::move(navigation).Value()};
        return AI::AdmitPreparedPointQuery(plan, *registry_, std::move(inputs), settings, tests);
    }

    /** @copydoc HeadlessDecisionQueries::PrepareVisibilityCapability */
    Result<void> HeadlessDecisionQueries::PrepareVisibilityCapability(Physics::PhysicsWorld &world) {
        if (visibilityScene_.SameIncarnation(scope_->scene) && visibility_.Identity().world == world.Identity())
            return Result<void>::Success();
        auto capability = world.IssueQueryEventCapability();
        if (capability.HasError())
            return Result<void>::Failure(std::move(capability).ErrorValue());
        visibility_ = std::move(capability).Value();
        visibilityScene_ = scope_->scene;
        return Result<void>::Success();
    }

    /** @copydoc HeadlessDecisionQueries::RetainsVisibilityAuthority */
    bool HeadlessDecisionQueries::RetainsVisibilityAuthority(const Runtime::RuntimeSceneView &scene,
                                                             const QueryPhysicsPublication &publication) const noexcept {
        if (!scope_ || !scope_->physics || !visibilityScene_.SameIncarnation(scene) || !scope_->scene.SameIncarnation(scene))
            return false;
        const auto identity = visibility_.Identity();
        if (identity.world != publication.identity.world || identity.capabilityGeneration != publication.identity.capabilityGeneration)
            return false;
        try {
            const auto scope = scope_->physics->Borrow();
            if (scope.HasError() || scope.Value().scene.value != publication.sceneGeneration ||
                scope.Value().physics.Identity() != identity.world)
                return false;
            const auto tick = scope.Value().physics.PublishedTick();
            return visibility_.ValidateQueryPublication(publication.sceneGeneration, tick.completedTick, tick.publicationRevision)
                .HasValue();
        } catch (const std::bad_alloc &) {
            return false;
        }
    }

    /** @copydoc HeadlessDecisionQueries::CaptureVisibility */
    Result<std::optional<QueryPhysicsVisibilityResult>> HeadlessDecisionQueries::CaptureVisibility(
        const QueryPhysicsVisibilityRequest &request) {
        using Captured = Result<std::optional<QueryPhysicsVisibilityResult>>;
        if (!scope_ || !scope_->physics)
            return Captured::Failure(MakeError(Physics::PhysicsErrors::CapabilityUnavailable));
        auto projected = ProjectQueryPhysicsVisibilityRays(request, scope_->origin);
        if (projected.HasError())
            return Captured::Failure(std::move(projected).ErrorValue());
        const auto native = scope_->physics->Borrow();
        if (native.HasError())
            return Captured::Failure(native.ErrorValue());
        const auto &binding = native.Value();
        const auto querier = scope_->scene.Get(request.querier);
        if (binding.scene != scope_->scene.RuntimeId() || querier.HasError())
            return Captured::Failure(MakeError(Physics::PhysicsErrors::QuerySnapshotStale));
        if (auto capability = PrepareVisibilityCapability(binding.physics); capability.HasError())
            return Captured::Failure(std::move(capability).ErrorValue());

        const auto tick = binding.physics.PublishedTick();
        const QueryPhysicsPublication publication{visibility_.Identity(), binding.scene.value, tick.publicationRevision,
                                                  tick.completedTick};
        QueryPhysicsVisibilityResult result{.publication = publication, .stage = request.stage, .count = request.points.size()};
        std::fill_n(result.clear.begin(), result.count, true);
        const auto &rays = projected.Value();
        if (rays.count == 0) {
            const auto valid =
                visibility_.ValidateQueryPublication(publication.sceneGeneration, publication.completedTick, publication.revision);
            return valid.HasValue() ? Captured::Success(result) : Captured::Failure(valid.ErrorValue());
        }
        Physics::PhysicsQueryFilter filter{.channel = request.channel};
        const auto body = std::ranges::find(binding.bodyBindings, querier.Value().authoredObject.value_or(Runtime::SceneObjectId{}),
                                            &Physics::PhysicsSceneBodyBinding::object);
        if (body != binding.bodyBindings.end())
            filter.excludedBody = body->handle;
        const auto completed =
            RunVisibilityBatch(binding.physics, visibility_, publication, request.stage, std::span{rays.rays}.first(rays.count), filter);
        if (completed.HasError() || !completed.Value())
            return completed;
        for (std::size_t index = 0; index < rays.count; ++index)
            result.clear[rays.indices[index]] = completed.Value()->clear[index];
        return Captured::Success(result);
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

    std::optional<AI::QueryNavigationAccess> HeadlessDecisionQueries::ReadQueryNavigation(
        const Runtime::RuntimeSceneView &scene) const noexcept {
        if (!scope_ || !scope_->navigation || !scope_->scene.SameIncarnation(scene))
            return std::nullopt;
        return scope_->navigation->ReadQueryAccess(scope_->tick);
    }

    /** @copydoc HeadlessDecisionQueries::UsesNavigation */
    bool HeadlessDecisionQueries::UsesNavigation() const noexcept {
        return std::ranges::any_of(plans_, [](const auto &entry) {
            return AI::UsesBuiltinPointNavigation(*entry.second.plan);
        });
    }

    /** @copydoc HeadlessDecisionQueries::RunAtFixedTick */
    Result<void> HeadlessDecisionQueries::RunAtFixedTick(AI::AiSceneRuntime &runtime, const Runtime::RuntimeSceneView &scene,
                                                         Math::WorldCoordinate64 origin, std::uint64_t tick, ScenePhysicsHost *physics,
                                                         const HeadlessNavigationPathScope *navigation) {
        if (scope_ || !scene.IsCurrent() || tick == 0)
            return Result<void>::Failure(MakeError(AI::AIErrors::TaskContextInvalid));
        if (auto prepared = PrepareCache(scene); prepared.HasError())
            return prepared;
        std::erase_if(tasks_, [](const auto &task) {
            return task.expired();
        });
        TickScope current{runtime, scene, origin, tick, physics, navigation};

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
