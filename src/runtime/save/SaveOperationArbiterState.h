#pragma once

/** @file SaveOperationArbiterState.h
 * @brief Target-private shared owner state for arbiter admission and bounded storage retry.
 */

#include "Horo/Runtime/Save/SaveErrors.h"
#include "Horo/Runtime/Save/SaveOperationArbiter.h"

#include <algorithm>
#include <vector>

namespace Horo::Runtime::SaveOperationArbiterDetail {
    /** @brief Sole session-owned record collection shared by admission and retry; never installed or independently scheduled. */
    struct State final {
        /** @brief One original operation controller, immutable request and bounded retry projection. */
        struct Record final {
            SaveArbiterRequest request;
            SaveOperationController controller;
            SaveArbiterState state{SaveArbiterState::Queued};
            std::uint64_t enqueueOrder{};
            std::uint64_t revision{1};
            SaveArbiterRetrySnapshot retry;
        };

        SaveOperationArbiterLimits limits;
        std::vector<Record> records;
        std::optional<OperationId> active;
        std::uint64_t nextEnqueueOrder{1};
        std::optional<std::uint64_t> retryClock;
        bool closed{};
    };

    using RecordIterator = std::vector<State::Record>::iterator;

    /** @brief Looks up one retained operation in the sole owner state. */
    template <typename StateType> [[nodiscard]] inline auto Find(StateType &state, const OperationId operation) {
        return std::ranges::find_if(state.records, [operation](const State::Record &record) {
            return record.request.operation.operation == operation;
        });
    }

    /** @brief Observes immutable terminal state through the retained consumer handle. */
    [[nodiscard]] inline bool IsTerminal(const State::Record &record) {
        const std::optional<SaveOperationSnapshot> snapshot = record.controller.Handle().Snapshot();
        return snapshot.has_value() && snapshot->IsTerminal();
    }

    /** @brief Copies bounded owner and operation evidence without changing it. */
    [[nodiscard]] inline SaveArbiterSnapshot CopySnapshot(const State::Record &record) {
        return {.operation = *record.controller.Handle().Snapshot(),
                .state = IsTerminal(record) ? SaveArbiterState::Terminal : record.state,
                .mode = record.request.mode,
                .address = record.request.address,
                .priority = record.request.priority,
                .enqueueOrder = record.enqueueOrder,
                .revision = record.revision,
                .retry = record.retry};
    }

    /** @brief Releases active ownership only for the operation that terminalized. */
    [[nodiscard]] inline bool SynchronizeTerminal(State &state, State::Record &record) {
        if (!IsTerminal(record))
            return false;
        record.state = SaveArbiterState::Terminal;
        ++record.revision;
        if (state.active == record.request.operation.operation)
            state.active.reset();
        return true;
    }

    /** @brief Finds only the exact active operation. */
    [[nodiscard]] inline RecordIterator FindActiveRecord(State &state, const OperationId operation) {
        if (!state.active.has_value() || *state.active != operation)
            return state.records.end();
        return Find(state, operation);
    }

    /** @brief Synchronizes one producer terminal transition with owner arbitration. */
    [[nodiscard]] inline Result<void> FinishTerminalTransition(State &state, State::Record &record,
                                                               const SaveOperationTransitionResult transition) {
        if (transition != SaveOperationTransitionResult::Applied)
            return SynchronizeTerminal(state, record) ? Result<void>::Success()
                                                      : Result<void>::Failure(MakeError(SaveErrors::ArbiterInvalid));
        record.state = SaveArbiterState::Terminal;
        ++record.revision;
        state.active.reset();
        return Result<void>::Success();
    }

    /** @brief Validates an optional host retry capability before operation admission.
     * @param request Original typed request and publication evidence. @return True only for a finite exact-address capability.
     */
    [[nodiscard]] bool IsRetryValid(const SaveArbiterRequest &request) noexcept;
}  // namespace Horo::Runtime::SaveOperationArbiterDetail
