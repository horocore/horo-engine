#pragma once

/** @file PhysicsWorldTickHelpers.h
 * @brief Target-private fixed-tick helpers shared by the PhysicsWorld implementation unit.
 */

#include "CanonicalPhysicsRuntime.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsEventProjection.h"
#include "PhysicsSaturatingAdd.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <limits>
#include <ranges>
#include <thread>
#include <tuple>

namespace Horo::Physics::Detail {
    /** @brief Provides non-throwing mutual exclusion for bounded publication copies. */
    class PublicationGuard final {
    public:
        explicit PublicationGuard(std::atomic_flag &lock) noexcept : lock_(lock) {
            while (lock_.test_and_set())
                std::this_thread::yield();
        }

        PublicationGuard(const PublicationGuard &) = delete;
        PublicationGuard &operator=(const PublicationGuard &) = delete;

        ~PublicationGuard() {
            lock_.clear();
        }

    private:
        std::atomic_flag &lock_;
    };

    /** @brief Restores one owner-thread boolean state when a guarded scope exits. */
    class BooleanResetGuard final {
    public:
        explicit BooleanResetGuard(bool &state) noexcept : state_(state) {}

        BooleanResetGuard(const BooleanResetGuard &) = delete;
        BooleanResetGuard &operator=(const BooleanResetGuard &) = delete;

        ~BooleanResetGuard() {
            state_ = false;
        }

    private:
        bool &state_;
    };

    /** @brief Records bounded-buffer rejection metrics and preserves explicit destruction retry ownership. */
    [[nodiscard]] PhysicsCommandAdmission RejectFullCommand(auto &impl, const bool destruction) noexcept {
        ++impl.statistics.rejectedCommands;
        if (!destruction)
            return {PhysicsCommandAdmissionStatus::RejectedFull, impl.commandCount};
        ++impl.statistics.destructionRetryCount;
        return {PhysicsCommandAdmissionStatus::DestructionRetryRequired, impl.commandCount};
    }

    /** @brief Replaces every externally visible publication domain under one synchronization boundary. */
    void CommitPublishedTick(auto &impl, const std::uint64_t tick, const std::uint32_t appliedCommands,
                             const PhysicsEventProjectionResult &eventResult) noexcept {
        PublicationGuard publicationGuard{impl.publicationLock};
        const std::uint64_t revision = impl.published.publicationRevision + 1;
        impl.published = {.completedTick = tick,
                          .publicationRevision = revision,
                          .transformTick = tick,
                          .queryTick = tick,
                          .eventTick = tick,
                          .appliedCommands = appliedCommands,
                          .eventCount = eventResult.publishedRecordCount,
                          .droppedEventCount = eventResult.droppedRecordCount};
    }

    /** @brief Normalizes and canonicalizes retained commands without frame-hot allocation. */
    void CanonicalizeCommands(auto &impl) noexcept {
        if (!impl.commandOrderDirty)
            return;
        if (impl.commandHead != 0) {
            std::ranges::rotate(impl.commands, impl.commands.begin() + impl.commandHead);
            impl.commandHead = 0;
        }
        std::ranges::sort(impl.commands.begin(), impl.commands.begin() + impl.commandCount,
                          [](const PhysicsStructuralCommand &left, const PhysicsStructuralCommand &right) {
            return PhysicsCommandOrderLess(left.order, right.order);
        });
        impl.commandOrderDirty = false;
    }

    /** @brief Validates one complete tick frame after canonicalization and before observation. */
    [[nodiscard]] Result<std::uint32_t> ValidateCommandFrame(auto &impl, const PhysicsFixedTickInput &input) {
        std::uint32_t eligible{};
        while (eligible < impl.commandCount && impl.CommandAt(eligible).order.simulationTick == input.simulationTick)
            ++eligible;
        if (eligible > impl.settings.Values().budgets.maximumCommandsPerTick)
            return Result<std::uint32_t>::Failure(
                MakeError(PhysicsErrors::CapacityExceeded, "The canonical Physics command frame exceeds its admitted tick budget."));
        for (std::uint32_t offset = 0; offset < eligible; ++offset) {
            if (const PhysicsCommandOrderKey &key = impl.CommandAt(offset).order;
                key.worldGeneration != impl.identity.Value() || key.sceneGeneration != input.sceneGeneration)
                return Result<std::uint32_t>::Failure(
                    MakeError(PhysicsErrors::CommandOrderInvalid, "A Physics command targets a stale world or scene generation."));
            impl.sourceOrder[offset] = offset;
        }
        std::ranges::sort(impl.sourceOrder.begin(), impl.sourceOrder.begin() + eligible,
                          [&impl](const std::uint32_t left, const std::uint32_t right) {
            const PhysicsCommandOrderKey &leftKey = impl.CommandAt(left).order;
            const PhysicsCommandOrderKey &rightKey = impl.CommandAt(right).order;
            return std::tie(leftKey.source, leftKey.sourceSequence) < std::tie(rightKey.source, rightKey.sourceSequence);
        });
        for (std::uint32_t offset = 0; offset < eligible; ++offset) {
            const PhysicsCommandOrderKey &key = impl.CommandAt(impl.sourceOrder[offset]).order;
            const bool startsSource = offset == 0 || impl.CommandAt(impl.sourceOrder[offset - 1]).order.source != key.source;
            if (startsSource && key.sourceSequence != 1)
                return Result<std::uint32_t>::Failure(
                    MakeError(PhysicsErrors::CommandOrderInvalid, "A Physics command source sequence has a missing predecessor."));
            if (!startsSource) {
                const std::uint64_t previousSequence = impl.CommandAt(impl.sourceOrder[offset - 1]).order.sourceSequence;
                if (previousSequence == std::numeric_limits<std::uint64_t>::max() || key.sourceSequence != previousSequence + 1)
                    return Result<std::uint32_t>::Failure(
                        MakeError(PhysicsErrors::CommandOrderInvalid, "A Physics command source sequence has a missing predecessor."));
            }
        }
        return Result<std::uint32_t>::Success(eligible);
    }

    /** @brief Emits one optional synchronous phase observation on the owner thread. */
    inline void ObservePhase(const PhysicsFixedTickInput &input, const PhysicsTickPhase phase) noexcept {
        if (input.observer.phase)
            input.observer.phase(input.observer.context, phase, input.simulationTick);
    }

    /** @brief Emits eligible commands selected for one semantic safe point without mutating the queue. */
    void ObserveCommands(const auto &impl, const PhysicsFixedTickInput &input, const std::uint32_t eligible,
                         const PhysicsStructuralCommandKind selectedKind, const PhysicsCommandSafePoint safePoint,
                         std::uint32_t &applied) noexcept {
        using enum PhysicsStructuralCommandKind;
        for (std::uint32_t offset = 0; offset < eligible; ++offset) {
            const PhysicsStructuralCommand &command = impl.CommandAt(offset);
            if (const bool selected = selectedKind == Destroy ? command.order.commandKind == Destroy : command.order.commandKind != Destroy;
                !selected)
                continue;
            if (input.observer.command)
                input.observer.command(input.observer.context, command, safePoint, input.simulationTick);
            ++applied;
        }
    }

    /** @brief Validates one solver-neutral batch before any tick phase is observed. */
    [[nodiscard]] inline Result<void> ValidateSolverJobs(const JobSystem *jobs, const PhysicsSolverJobBatch &batch) {
        if (batch.jobCount == 0)
            return Result<void>::Success();
        if (jobs == nullptr)
            return Result<void>::Failure(
                MakeError(PhysicsErrors::CapabilityUnavailable, "This Physics runtime has no injected solver job system."));
        if (batch.jobs == nullptr || batch.jobCount > MaximumPhysicsSolverJobsPerTick || batch.joinTimeout <= Duration{})
            return Result<void>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Solver jobs require valid bounded storage and a positive join timeout."));
        for (std::uint32_t index = 0; index < batch.jobCount; ++index) {
            if (batch.jobs[index].execute == nullptr)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Every solver job requires an executable callback."));
        }
        return Result<void>::Success();
    }

    /** @brief Reports whether two errors preserve the same stable domain/code identity. */
    [[nodiscard]] inline bool SameErrorCode(const Error &left, const Error &right) noexcept {
        return left.domain.Value() == right.domain.Value() && left.code.Value() == right.code.Value();
    }

    /** @brief Dispatches one validated solver-neutral batch and drains it before the tick may continue. */
    [[nodiscard]] inline Result<void> RunSolverJobs(JobSystem &jobs, const PhysicsSolverJobBatch &batch) {
        TaskGroup group(jobs, TaskGroupFailurePolicy::CollectAll);
        for (std::uint32_t index = 0; index < batch.jobCount; ++index) {
            const PhysicsSolverJob job = batch.jobs[index];
            const auto spawned = group.Spawn({}, [job](const CancellationToken &cancellation) {
                return job.execute(job.context, cancellation);
            });
            if (spawned.HasError())
                return Result<void>::Failure(spawned.ErrorValue());
        }

        const Result<void> joined = group.Join({.waitPolicy = WaitPolicy::OwnerThreadBlockAllowed, .timeout = batch.joinTimeout});
        if (joined.HasValue())
            return joined;
        group.RequestCancel();
        if (const Result<void> drained = group.Join({.waitPolicy = WaitPolicy::OwnerThreadBlockAllowed,
                                                     .timeout = Duration::FromNanoseconds(std::numeric_limits<std::int64_t>::max())});
            drained.HasError() && SameErrorCode(joined.ErrorValue(), drained.ErrorValue()))
            return joined;
        return Result<void>::Failure(MakeError(PhysicsErrors::SolverDeadlineExceeded));
    }

    /** @brief Checks mutable owner affinity and lifecycle admission for one fixed tick. */
    [[nodiscard]] Result<void> CheckReadyForTick(const auto &impl) {
        if (impl.runtime->ownerThread != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl.state == PhysicsWorldState::ActiveNull)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl.state != PhysicsWorldState::ActiveSolver || impl.runtime->state != PhysicsRuntimeState::Ready || impl.stepping)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        return Result<void>::Success();
    }

    /** @brief Checks sequence and exact fixed delta before a tick begins. */
    [[nodiscard]] Result<void> ValidateTickInput(const auto &impl, const PhysicsFixedTickInput &input) {
        if (const auto configuredNanoseconds =
                static_cast<std::int64_t>(std::llround(impl.settings.Values().world.fixedDeltaSeconds * 1'000'000'000.0));
            input.simulationTick == 0 || input.sceneGeneration == 0 || input.simulationTick != impl.published.completedTick + 1 ||
            input.fixedDelta.ToNanoseconds() != configuredNanoseconds)
            return Result<void>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Physics requires the next one-based tick and the world's exact fixed delta."));
        return Result<void>::Success();
    }

    [[nodiscard]] Result<PhysicsEventProjectionResult> CompleteEventProjection(auto &impl, const PhysicsFixedTickInput &input) {
        const Result<PhysicsEventProjectionResult> result = impl.events.CompleteTick(input.simulationTick);
        if (result.HasError()) {
            impl.statistics.droppedEventCount = SaturatingAdd(impl.statistics.droppedEventCount, impl.events.DroppedRecordCount());
            if (impl.events.DroppedRecordCount() != 0)
                impl.RecordEventOverflowDiagnostic(input.sceneGeneration, input.simulationTick);
            impl.events.AbortTick();
            return result;
        }
        impl.statistics.droppedEventCount = SaturatingAdd(impl.statistics.droppedEventCount, result.Value().droppedRecordCount);
        impl.statistics.eventDepth = result.Value().publishedRecordCount;
        impl.statistics.maximumEventDepth = std::max(impl.statistics.maximumEventDepth, impl.statistics.eventDepth);
        if (result.Value().droppedRecordCount != 0)
            impl.RecordEventOverflowDiagnostic(input.sceneGeneration, input.simulationTick);
        return result;
    }

    [[nodiscard]] Result<void> RunInjectedSolverJobs(auto &impl, const PhysicsFixedTickInput &input) {
        if (input.solverJobs.jobCount == 0)
            return Result<void>::Success();
        if (const Result<void> jobs = RunSolverJobs(*impl.runtime->solverJobs, input.solverJobs); jobs.HasError()) {
            impl.Fail(jobs.ErrorValue(), input.sceneGeneration, input.simulationTick);
            return Result<void>::Failure(jobs.ErrorValue());
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
