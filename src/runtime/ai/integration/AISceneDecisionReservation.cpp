#include "AISceneRuntimeDetail.h"
#include "Horo/AI/DecisionGraphInstance.h"
#include "Horo/AI/DecisionQueryTask.h"

#include <limits>
#include <new>
#include <utility>

namespace Horo::AI {
    namespace {
        /** @brief Resolves only an active exact agent after the caller has checked the real owner lane. */
        Result<Detail::AgentRuntimeState *> DecisionAgent(Detail::AiSceneRuntimeState &state, const AgentHandle agent) {
            auto *slot = Detail::FindAgent(state, agent);
            if (!slot || slot->record.state != AiAgentActivationState::Active || !slot->blackboard || !slot->blackboard->IsActive())
                return Result<Detail::AgentRuntimeState *>::Failure(MakeError(AIErrors::HandleInvalid));
            return Result<Detail::AgentRuntimeState *>::Success(slot);
        }

        /** @brief Freezes the blackboard owned by an exact active agent generation. */
        Result<BlackboardSnapshot> CaptureDecisionSnapshot(Detail::AiSceneRuntimeState &state, const AgentHandle agent) {
            const auto selected = DecisionAgent(state, agent);
            if (selected.HasError())
                return Result<BlackboardSnapshot>::Failure(selected.ErrorValue());
            return selected.Value()->blackboard->Snapshot();
        }

        /** @brief Flat cooked decisions own their task lease and borrow only a weak exact Scene publication. */
        class ScheduledCookedDecision final : public IAiScheduledDecision {
        public:
            ScheduledCookedDecision(const std::shared_ptr<Detail::AiSceneRuntimeState> &state,
                                    std::unique_ptr<DecisionGraphInstance> decision) noexcept
                : state_(state), decision_(std::move(decision)) {}

            Result<AiSliceOutcome> Evaluate(const AiDecisionSliceContext &context, AiWorkBudget &budget) noexcept override {
                providerFailure_.reset();
                evaluationTick_ = context.tick;
                const auto state = state_.lock();
                if (!state || state->closed || state->owner != std::this_thread::get_id())
                    return Result<AiSliceOutcome>::Failure(MakeError(AIErrors::RuntimeUnavailable));
                const auto snapshot = CaptureDecisionSnapshot(*state, context.agent);
                if (snapshot.HasError())
                    return Result<AiSliceOutcome>::Failure(snapshot.ErrorValue());
                const auto step =
                    decision_->StepBudgeted({context.tick, snapshot.Value().Binding(), snapshot.Value(), {}}, budget, &providerFailure_);
                if (step.HasError())
                    return Result<AiSliceOutcome>::Failure(step.ErrorValue());
                return Result<AiSliceOutcome>::Success(step.Value() ? AiSliceOutcome::Complete : AiSliceOutcome::BudgetExhausted);
            }

            std::optional<DecisionProviderFailure> TakeProviderFailure(std::uint64_t tick) noexcept override {
                return tick == evaluationTick_ ? std::exchange(providerFailure_, std::nullopt) : std::nullopt;
            }

            Result<bool> Commit(const AiDecisionSliceContext &, AiWorkBudget &) noexcept override {
                // These native decision callbacks receive no Scene command writer.
                return Result<bool>::Success(true);
            }

            void Cancel() noexcept override {
                providerFailure_.reset();
                static_cast<void>(decision_->Cancel(AiTaskCancellationReason::AgentGenerationRetired));
            }

        private:
            std::weak_ptr<Detail::AiSceneRuntimeState> state_;
            std::unique_ptr<DecisionGraphInstance> decision_;
            std::optional<DecisionProviderFailure> providerFailure_;
            std::uint64_t evaluationTick_{};
        };
    }  // namespace

    /** @copydoc AiSceneRuntime::ResolveDecisionQueryOwnerAtSafePoint */
    Result<Runtime::EntityRef> AiSceneRuntime::ResolveDecisionQueryOwnerAtSafePoint(AgentHandle agent) const {
        using Owner = Result<Runtime::EntityRef>;
        if (auto valid = CapabilityAtSafePoint().ValidateAtSafePoint(); valid.HasError())
            return Owner::Failure(valid.ErrorValue());
        const auto selected = DecisionAgent(*active_, agent);
        if (selected.HasError())
            return Owner::Failure(selected.ErrorValue());
        return Owner::Success(selected.Value()->record.owner);
    }

    /** @copydoc AiSceneRuntime::ReadDecisionFailureAtSafePoint */
    Result<std::optional<AiRuntimeDecisionDiagnostic>> AiSceneRuntime::ReadDecisionFailureAtSafePoint(
        const Runtime::RuntimeSceneView &scene) const {
        using Diagnostic = Result<std::optional<AiRuntimeDecisionDiagnostic>>;
        if (auto owner = CapabilityAtSafePoint().ValidateAtSafePoint(); owner.HasError())
            return Diagnostic::Failure(owner.ErrorValue());
        const auto scheduler = TaskSchedulerAtSafePoint();
        if (scheduler.HasError())
            return Diagnostic::Failure(scheduler.ErrorValue());
        if (!scene.IsCurrent() || scene.RuntimeId() != active_->binding.scene)
            return Diagnostic::Failure(MakeError(AIErrors::TaskContextInvalid, "Decision diagnostics require the current AI Scene."));
        const auto *report = scheduler.Value()->CompletedReportAtSafePoint();
        if (!report)
            return Diagnostic::Success(std::nullopt);
        const bool fatal = report->firstFailure && report->firstFailureOrigin;
        const auto *advisory = report->firstAdvisoryFailure ? &*report->firstAdvisoryFailure : nullptr;
        if (!fatal && !advisory)
            return Diagnostic::Success(std::nullopt);
        const auto &origin = fatal ? *report->firstFailureOrigin : advisory->origin;
        const auto &error = fatal ? *report->firstFailure : advisory->error;
        const auto *slot = Detail::FindAgent(*active_, origin.agent);
        std::optional<Runtime::EntityRef> entity;
        if (slot && slot->record.state == AiAgentActivationState::Active && slot->record.agent.agent == origin.identity &&
            scene.Get(slot->record.owner).HasValue())
            entity = slot->record.owner;
        return Diagnostic::Success(AiRuntimeDecisionDiagnostic{report->tick, origin, error, entity});
    }

    /** @copydoc AiSceneRuntime::SyncDecisionQueryAtBlackboardSync */
    Result<bool> AiSceneRuntime::SyncDecisionQueryAtBlackboardSync(DecisionQueryTask &task, const Runtime::RuntimeSceneView &scene,
                                                                   std::uint64_t tick) const {
        using Sync = Result<bool>;
        if (auto valid = CapabilityAtSafePoint().ValidateAtSafePoint(); valid.HasError())
            return Sync::Failure(valid.ErrorValue());
        if (tick == 0)
            return Sync::Failure(MakeError(AIErrors::TaskContextInvalid));
        const auto state = active_;
        if (!scene.IsCurrent() || scene.RuntimeId() != state->binding.scene)
            return Sync::Failure(MakeError(AIErrors::EnvironmentQueryContextStale));
        const auto owner = task.ActiveAgent();
        if (!owner.IsValid())
            return Sync::Success(false);
        const auto selected = DecisionAgent(*state, owner);
        if (selected.HasError())
            return Sync::Failure(selected.ErrorValue());
        task.SyncQuery(tick);
        return task.CommitOutput(tick, scene, *selected.Value()->blackboard);
    }

    /** @copydoc AiSceneRuntime::PrepareCookedDecisionRegistrations */
    Result<std::vector<AiScheduledDecisionRegistration>> AiSceneRuntime::PrepareCookedDecisionRegistrations(
        const std::shared_ptr<Detail::AiSceneRuntimeState> &state, const std::span<const Detail::AgentRuntimeState> agents,
        std::uint32_t &nextTaskSlot) const {
        using Output = Result<std::vector<AiScheduledDecisionRegistration>>;
        if (!decisionExecution_)
            return Output::Success({});
        const auto &policy = decisionExecution_->agent;
        auto cursor = nextTaskSlot;
        try {
            std::vector<AiScheduledDecisionRegistration> registrations;
            for (const auto &agent : agents) {
                const auto &plan = agent.cookedPlan;
                if (!plan)
                    continue;
                auto limit = plan->MaximumDecisionWorkUnits();
                if (!limit)
                    return Output::Failure(
                        MakeError(AIErrors::CapabilityUnavailable, "Scene scheduler requires a flat cooked Behavior Tree."));
                if (*limit > policy.workUnitsPerTick)
                    return Output::Failure(MakeError(AIErrors::SchedulerBudgetExhausted));
                const auto &root = plan->Graphs()[plan->Occurrences().front().plan];
                if (!root.executionTask.IsValid())
                    return Output::Failure(
                        MakeError(AIErrors::TaskContextInvalid, "Scheduled cooked decisions require an authored execution task."));

                constexpr auto End = Horo::Handle<TaskHandleTag>::InvalidIndex;
                const auto taskSlots = plan->Storage().taskSlots;
                if (taskSlots >= End - cursor)
                    return Output::Failure(MakeError(AIErrors::TaskCapacityExceeded));
                const auto count = static_cast<std::uint32_t>(taskSlots);
                const auto handle = agent.record.handle;
                const auto cancellation = agent.cancellation.Token();
                const AiTaskOperationContext operation{.taskDefinition = root.executionTask,
                                                       .task = {handle.incarnation, {cursor, 1}},
                                                       .agent = handle,
                                                       .cancellation = cancellation};
                AiDecisionTaskReservation reservation{agent.blackboardBinding, agent.schema, cursor + 1, count, cancellation};
                auto decision = DecisionGraphInstance::Create(plan, std::move(reservation), operation, decisionExecution_->queryTasks);
                if (decision.HasError())
                    return Output::Failure(decision.ErrorValue());

                auto bounded = policy;
                bounded.workUnitsPerTick = *limit;
                registrations.push_back({handle, agent.record.agent.agent, bounded, plan,
                                         std::make_shared<ScheduledCookedDecision>(state, std::move(decision).Value()), cancellation});
                cursor += count + 1;
            }
            nextTaskSlot = cursor;
            return Output::Success(std::move(registrations));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AIErrors::DecisionAssetStorageUnavailable));
        }
    }

    /** @copydoc AiSceneRuntime::PrepareCookedDecisionScheduler */
    Result<void> AiSceneRuntime::PrepareCookedDecisionScheduler(const std::shared_ptr<Detail::AiSceneRuntimeState> &state) const {
        if (!decisionExecution_)
            return Result<void>::Success();
        const auto revision = state->revision;
        auto scheduler = AiTaskScheduler::Create(decisionExecution_->jobs, state->binding.incarnation, decisionExecution_->scheduler);
        if (scheduler.HasError())
            return Result<void>::Failure(scheduler.ErrorValue());
        auto cursor = state->nextTaskSlot;
        auto registrations = PrepareCookedDecisionRegistrations(state, state->agents, cursor);
        if (registrations.HasError())
            return Result<void>::Failure(registrations.ErrorValue());
        auto staged = scheduler.Value()->PreparePopulationChange(registrations.Value(), {});
        if (staged.HasError())
            return Result<void>::Failure(staged.ErrorValue());
        if (state->closed || state->revision != revision || state->scheduler)
            return Result<void>::Failure(
                MakeError(AIErrors::SceneActivationInvalid, "The Scene owner changed while cooked decisions were prepared."));
        if (auto valid = staged.Value()->ValidatePublication(); valid.HasError())
            return valid;
        staged.Value()->Publish();
        state->scheduler = std::move(scheduler).Value();
        state->nextTaskSlot = cursor;
        return Result<void>::Success();
    }

    /** @copydoc AiSceneRuntime::ActivateCookedDecisionsAtSafePoint */
    Result<void> AiSceneRuntime::ActivateCookedDecisionsAtSafePoint(AiSceneCookedDecisionExecution execution) {
        if (auto valid = CapabilityAtSafePoint().ValidateAtSafePoint(); valid.HasError())
            return valid;
        const auto state = active_;
        if (decisionExecution_ || state->scheduler || execution.scheduler.mode != AiSchedulingMode::Deterministic)
            return Result<void>::Failure(MakeError(AIErrors::SchedulerPolicyInvalid));
        if (state->revision == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(AIErrors::CanonicalRestoreStale));
        decisionExecution_.emplace(std::move(execution));
        auto prepared = PrepareCookedDecisionScheduler(state);
        if (prepared.HasError()) {
            decisionExecution_.reset();
            return prepared;
        }
        ++state->revision;
        return Result<void>::Success();
    }

    /** @copydoc AiSceneRuntime::ReserveDecisionTaskSlotsAtSafePoint */
    Result<AiDecisionTaskReservation> AiSceneRuntime::ReserveDecisionTaskSlotsAtSafePoint(const AgentHandle agent,
                                                                                          const std::uint32_t count) const {
        using Output = Result<AiDecisionTaskReservation>;
        if (auto owner = CapabilityAtSafePoint().ValidateAtSafePoint(); owner.HasError())
            return Output::Failure(owner.ErrorValue());
        auto selected = DecisionAgent(*active_, agent);
        if (selected.HasError())
            return Output::Failure(selected.ErrorValue());
        if (active_->revision == std::numeric_limits<std::uint64_t>::max())
            return Output::Failure(MakeError(AIErrors::CanonicalRestoreStale));
        constexpr auto End = Horo::Handle<TaskHandleTag>::InvalidIndex;
        if (count > End - active_->nextTaskSlot)
            return Output::Failure(MakeError(AIErrors::TaskCapacityExceeded));
        try {
            auto &slot = *selected.Value();
            AiDecisionTaskReservation reservation{slot.blackboard->Binding(), slot.blackboard->SchemaAtSafePoint(), active_->nextTaskSlot,
                                                  count, slot.cancellation.Token()};
            active_->nextTaskSlot += count;
            ++active_->revision;
            return Output::Success(std::move(reservation));
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(AIErrors::TaskCapacityExceeded));
        }
    }

    /** @copydoc AiSceneRuntime::CaptureDecisionSnapshotAtSafePoint */
    Result<BlackboardSnapshot> AiSceneRuntime::CaptureDecisionSnapshotAtSafePoint(const AgentHandle agent) const {
        if (auto owner = CapabilityAtSafePoint().ValidateAtSafePoint(); owner.HasError())
            return Result<BlackboardSnapshot>::Failure(owner.ErrorValue());
        return CaptureDecisionSnapshot(*active_, agent);
    }
}  // namespace Horo::AI
