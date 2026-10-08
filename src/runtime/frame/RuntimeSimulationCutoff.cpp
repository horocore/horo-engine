#include "../lifecycle/RuntimeErrors.h"
#include "Horo/Runtime/RuntimeSimulationTiming.h"
#include "internal/RuntimeSimulationTimingStorage.h"

#include <algorithm>
#include <limits>

namespace Horo::Runtime {
    namespace {
        /** @brief Reclaims released observations only after their pending work becomes terminal. */
        void ReconcileSteps(SimulationTimingDetail::Storage &state, const bool suspended) noexcept {
            using enum RuntimeSingleStepState;
            for (auto &record : state.steps) {
                if (!record.occupied)
                    continue;
                if (record.result.state == Pending) {
                    if (record.pauseRevision != state.desired.pauseRevision || !state.desired.paused) {
                        record.result.state = Cancelled;
                        record.result.cancellation = RuntimeSingleStepCancellation::PauseChanged;
                        --state.desired.pendingSteps;
                    } else {
                        record.admitted = !suspended;
                    }
                }
                if (record.result.state != Pending && record.released.load())
                    record.occupied = false;
            }
        }

        /** @brief Chooses oldest admitted pending work without allocating a queue or revisiting committed requests. */
        SimulationTimingDetail::StepRecord *NextStep(SimulationTimingDetail::Storage &state) noexcept {
            SimulationTimingDetail::StepRecord *next = nullptr;
            for (auto &record : state.steps) {
                if (record.occupied && record.admitted && record.result.state == RuntimeSingleStepState::Pending &&
                    (!next || record.result.requestSequence < next->result.requestSequence))
                    next = &record;
            }
            return next;
        }
    }  // namespace

    /** @copydoc RuntimeSimulationControl::CommitCutoff */
    Result<void> RuntimeSimulationControl::CommitCutoff(const bool suspended) {
        auto *state = storage_.Mutable();
        if (!state || state->closed)
            return Result<void>::Failure(MakeError(RuntimeErrors::SimulationTimingClosed));
        if (state->owner != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(RuntimeErrors::SimulationTimingInvalid));
        std::uint32_t released{};
        for (auto &record : state->pauses) {
            record.releaseAtCutoff = record.occupied && record.released.load();
            released += record.releaseAtCutoff ? 1U : 0U;
        }
        if (constexpr auto Maximum = std::numeric_limits<std::uint64_t>::max();
            released != 0 && (state->desired.commandRevision == Maximum || state->desired.pauseRevision == Maximum))
            return Result<void>::Failure(MakeError(RuntimeErrors::SimulationTimingOverflow));
        if (released != 0) {
            for (auto &record : state->pauses) {
                if (record.releaseAtCutoff)
                    record.occupied = false;
            }
            state->desired.pauseCount -= released;
            state->desired.paused = state->desired.pauseCount != 0;
            ++state->desired.pauseRevision;
            ++state->desired.commandRevision;
        }
        ReconcileSteps(*state, suspended);
        state->active = state->desired;
        return Result<void>::Success();
    }

    /** @copydoc RuntimeSimulationControl::HasAdmittedStep */
    bool RuntimeSimulationControl::HasAdmittedStep() const noexcept {
        const auto *state = storage_.Borrow();
        return state && std::ranges::any_of(state->steps, [](const auto &record) {
            return record.occupied && record.admitted && record.result.state == RuntimeSingleStepState::Pending;
        });
    }

    /** @copydoc RuntimeSimulationControl::CommitStep */
    void RuntimeSimulationControl::CommitStep(const std::uint64_t tick, const std::uint64_t attempt, const std::uint64_t frame,
                                              const Duration duration) noexcept {
        auto &state = *storage_.Mutable();
        if (auto *record = NextStep(state)) {
            record->result.state = RuntimeSingleStepState::Committed;
            record->result.simulationTick = tick;
            record->result.attemptNumber = attempt;
            record->result.frameNumber = frame;
            record->result.duration = duration;
            record->admitted = false;
            --state.desired.pendingSteps;
            --state.active.pendingSteps;
        }
    }

    /** @copydoc RuntimeSimulationControl::Close */
    void RuntimeSimulationControl::Close() noexcept {
        auto *state = storage_.Mutable();
        if (!state || state->closed)
            return;
        state->closed = true;
        for (auto &record : state->steps) {
            if (record.occupied && record.result.state == RuntimeSingleStepState::Pending) {
                record.result.state = RuntimeSingleStepState::Cancelled;
                record.result.cancellation = RuntimeSingleStepCancellation::HostRetired;
                record.admitted = false;
            }
        }
        state->desired.pendingSteps = 0;
        state->active.pendingSteps = 0;
    }
}  // namespace Horo::Runtime
