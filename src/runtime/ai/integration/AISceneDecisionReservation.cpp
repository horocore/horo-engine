#include "AISceneRuntimeDetail.h"
#include "Horo/AI/DecisionGraphInstance.h"

#include <limits>
#include <new>

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
                const auto state = state_.lock();
                if (!state || state->closed || state->owner != std::this_thread::get_id())
                    return Result<AiSliceOutcome>::Failure(MakeError(AIErrors::RuntimeUnavailable));
                const auto snapshot = CaptureDecisionSnapshot(*state, context.agent);
                if (snapshot.HasError())
                    return Result<AiSliceOutcome>::Failure(snapshot.ErrorValue());
                const auto step = decision_->StepBudgeted({context.tick, snapshot.Value().Binding(), snapshot.Value(), {}}, budget);
                if (step.HasError())
                    return Result<AiSliceOutcome>::Failure(step.ErrorValue());
                return Result<AiSliceOutcome>::Success(step.Value() ? AiSliceOutcome::Complete : AiSliceOutcome::BudgetExhausted);
            }

            Result<bool> Commit(const AiDecisionSliceContext &, AiWorkBudget &) noexcept override {
                // These native decision callbacks receive no Scene command writer.
                return Result<bool>::Success(true);
            }

            void Cancel() noexcept override {
                static_cast<void>(decision_->Cancel(AiTaskCancellationReason::AgentGenerationRetired));
            }

        private:
            std::weak_ptr<Detail::AiSceneRuntimeState> state_;
            std::unique_ptr<DecisionGraphInstance> decision_;
        };
    }  // namespace

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
                auto decision = DecisionGraphInstance::Create(plan, std::move(reservation), operation);
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
        if (auto valid = staged.Value()->ValidatePublication(); valid.HasError())
            return valid;
        staged.Value()->Publish();
        state->scheduler = std::move(scheduler).Value();
        state->nextTaskSlot = cursor;
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
