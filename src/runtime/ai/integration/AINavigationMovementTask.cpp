#include "Horo/AI/AINavigationMovementTask.h"

#include "Horo/AI/AIErrors.h"
#include "Horo/Navigation/NavigationErrors.h"

#include <new>
#include <utility>

namespace Horo::AI {
    namespace N = Navigation;

    namespace {
        using Update = Result<std::optional<N::NavigationMovementIntent>>;

        /** @brief Carries the provider cause when available and preserves the identity of owner-side failures. */
        Error MovementError(const N::NavigationMovementStatus &status) {
            if (status.error.has_value())
                return *status.error;
            switch (status.state) {
                case N::NavigationMovementState::Unreachable:
                    return MakeError(AIErrors::NavigationTaskUnreachable);
                case N::NavigationMovementState::WorldLost:
                    return MakeError(N::NavigationErrors::InvalidWorld);
                case N::NavigationMovementState::AgentLost:
                    return MakeError(N::NavigationErrors::AgentRegistryStale);
                case N::NavigationMovementState::TargetLost:
                    return MakeError(N::NavigationErrors::InvalidHandle, "The admitted movement target was retired.");
                default:
                    return MakeError(N::NavigationErrors::ProviderFailed);
            }
        }

        /** @brief Retains exact cancellation diagnostics across the Navigation and AI owners. */
        Result<void> Transition(const Result<AiTaskTransitionDisposition> &result) {
            return result.HasValue() ? Result<void>::Success() : Result<void>::Failure(result.ErrorValue());
        }
    }  // namespace

    AiNavigationMovementTask::AiNavigationMovementTask(AiRuntimeCapability ai, const AgentHandle agent, const TaskHandle task,
                                                       N::NavigationMovementController &movement) noexcept
        : ai_(std::move(ai)), agent_(agent), task_(task), movement_(&movement), owner_(std::this_thread::get_id()) {}

    AiNavigationMovementTask::AiNavigationMovementTask(AiNavigationMovementTask &&other) noexcept
        : ai_(std::move(other.ai_)), agent_(other.agent_), task_(other.task_), movement_(std::exchange(other.movement_, nullptr)),
          operation_(other.operation_), owner_(other.owner_) {}

    AiNavigationMovementTask::~AiNavigationMovementTask() {
        if (!CancelMotion())
            return;
        try {
            (void)ai_.CancelTask(agent_, task_, AiTaskCancellationReason::OwnerShutdown);
        } catch (const std::bad_alloc &) {
            // Navigation is already retired. A rejected AI publication can only allocate diagnostic storage here.
        }
    }

    /** @copydoc AiNavigationMovementTask::Start */
    Result<AiNavigationMovementTask> AiNavigationMovementTask::Start(AiRuntimeCapability ai, const AgentHandle agent, const TaskHandle task,
                                                                     N::NavigationMovementController &movement,
                                                                     N::NavigationMovementRequest request,
                                                                     const N::NavigationMovementObservation &current) {
        using StartResult = Result<AiNavigationMovementTask>;
        const auto owner = ai.ResolveAgentOwner(agent);
        if (owner.HasError())
            return StartResult::Failure(owner.ErrorValue());
        const auto nav = movement.Owner();
        const auto entity = owner.Value();
        if (nav.scene.Value() != entity.runtime.value || nav.entityIndex != entity.entity.index ||
            nav.entityGeneration != entity.entity.generation)
            return StartResult::Failure(MakeError(AIErrors::TaskContextInvalid));
        const auto execution = ai.InspectTask(agent, task);
        if (execution.HasError())
            return StartResult::Failure(execution.ErrorValue());
        if (execution.Value().state != AiTaskState::Running)
            return StartResult::Failure(MakeError(AIErrors::TaskTransitionInvalid));
        if (const auto state = movement.Status().state;
            state >= N::NavigationMovementState::Querying && state < N::NavigationMovementState::Arrived)
            return StartResult::Failure(MakeError(N::NavigationErrors::AdmissionRejected));

        AiNavigationMovementTask binding(std::move(ai), agent, task, movement);
        const auto accepted = movement.Start(std::move(request), current);
        if (accepted.HasError())
            return StartResult::Failure(binding.RecordFailure(AiTaskFailureKind::Admission, accepted.ErrorValue()));
        binding.operation_ = accepted.Value().accepted;
        return StartResult::Success(std::move(binding));
    }

    /** @copydoc AiNavigationMovementTask::CanAdvance */
    Result<bool> AiNavigationMovementTask::CanAdvance() {
        if (!movement_ || owner_ != std::this_thread::get_id())
            return Result<bool>::Failure(MakeError(AIErrors::RuntimeUnavailable));
        const auto task = ai_.InspectTask(agent_, task_);
        if (task.HasError()) {
            CancelMotion();
            return Result<bool>::Failure(task.ErrorValue());
        }
        if (task.Value().state != AiTaskState::Running) {
            CancelMotion();
            return Result<bool>::Success(false);
        }
        if (movement_->Status().id != operation_) {
            const auto cancelled = ai_.CancelTask(agent_, task_, AiTaskCancellationReason::Superseded);
            return cancelled.HasValue() ? Result<bool>::Success(false) : Result<bool>::Failure(cancelled.ErrorValue());
        }
        return Result<bool>::Success(true);
    }

    /** @copydoc AiNavigationMovementTask::Advance */
    Update AiNavigationMovementTask::Advance(const N::NavigationMovementObservation &current) {
        const auto active = CanAdvance();
        if (active.HasError())
            return Update::Failure(active.ErrorValue());
        if (!active.Value())
            return Update::Success(std::nullopt);

        auto intent = movement_->Update(current);
        if (intent.HasError())
            return Update::Failure(RecordFailure(AiTaskFailureKind::Execution, std::move(intent).ErrorValue()));
        if (auto outcome = ResolveOutcome(); outcome.HasError())
            return Update::Failure(std::move(outcome).ErrorValue());
        return Update::Success(std::move(intent).Value());
    }

    /** @copydoc AiNavigationMovementTask::PrepareSafeMotion */
    Result<std::optional<N::NavigationVelocityWork>> AiNavigationMovementTask::PrepareSafeMotion(
        N::NavigationVelocityPipeline &pipeline, const N::NavigationMovementObservation &current,
        const N::NavigationCrowdSnapshot &snapshot, N::NavigationAvoidanceRequest request) {
        using Return = Result<std::optional<N::NavigationVelocityWork>>;
        const auto active = CanAdvance();
        if (active.HasError())
            return Return::Failure(active.ErrorValue());
        if (!active.Value())
            return Return::Success(std::nullopt);
        if (!pipeline.UsesMovement(*movement_))
            return Return::Failure(MakeError(AIErrors::TaskContextInvalid));
        auto work = pipeline.Prepare(current, snapshot, request);
        if (work.HasError())
            return Return::Failure(RecordFailure(AiTaskFailureKind::Execution, std::move(work).ErrorValue()));
        if (auto outcome = ResolveOutcome(); outcome.HasError()) {
            (void)pipeline.CancelPending(work.Value());
            return Return::Failure(std::move(outcome).ErrorValue());
        }
        return Return::Success(std::move(work).Value());
    }

    /** @copydoc AiNavigationMovementTask::Cancel */
    Result<AiTaskTransitionDisposition> AiNavigationMovementTask::Cancel() {
        if (!movement_ || owner_ != std::this_thread::get_id())
            return Result<AiTaskTransitionDisposition>::Failure(MakeError(AIErrors::RuntimeUnavailable));
        CancelMotion();
        return ai_.CancelTask(agent_, task_);
    }

    /** @copydoc AiNavigationMovementTask::RecordFailure */
    Error AiNavigationMovementTask::RecordFailure(const AiTaskFailureKind kind, Error cause) {
        CancelMotion();
        auto failed = ai_.FailTask(agent_, task_, {kind, cause});
        if (failed.HasError())
            return std::move(failed).ErrorValue();
        return cause;
    }

    /** @copydoc AiNavigationMovementTask::ResolveOutcome */
    Result<void> AiNavigationMovementTask::ResolveOutcome() {
        const auto &status = movement_->Status();
        using enum N::NavigationMovementState;
        switch (status.state) {
            case Arrived:
                return Transition(ai_.CompleteTask(agent_, task_));
            case Cancelled:
                return Transition(ai_.CancelTask(agent_, task_, AiTaskCancellationReason::ContextCancelled));
            case TimedOut:
                return Transition(ai_.CancelTask(agent_, task_, AiTaskCancellationReason::TimedOut));
            case AgentLost:
            case TargetLost:
            case WorldLost:
                return Result<void>::Failure(RecordFailure(AiTaskFailureKind::DependencyUnavailable, MovementError(status)));
            case Unreachable:
            case Failed:
                return Result<void>::Failure(RecordFailure(AiTaskFailureKind::Execution, MovementError(status)));
            default:
                return Result<void>::Success();
        }
    }

    /** @copydoc AiNavigationMovementTask::CancelMotion */
    bool AiNavigationMovementTask::CancelMotion() noexcept {
        return movement_ && owner_ == std::this_thread::get_id() && operation_.sequence != 0 && movement_->Cancel(operation_);
    }
}  // namespace Horo::AI
