#pragma once
/** @file RuntimeSaveSessionInternal.h @brief Operation-owned worker and aggregate-handoff state; no live Scene borrow. */
#include "Horo/Runtime/Save/RuntimeSaveSession.h"

#include <mutex>

namespace Horo::Runtime::RuntimeSaveSessionDetail {
    /** @brief Worker closures and consumer handles pin this state; no session/host borrow crosses into a worker.
     * The mutex protects only detached staging handoff. Participant/user callbacks never execute under it.
     * Producer ownership stays fixed for Save, and transfers to the restore transaction only after the Load job settles.
     */
    struct Work final {
        explicit Work(SaveOperationController value, CancellationSource cancel)
            : producer(std::move(value)), handle(producer.Handle()), cancellation(std::move(cancel)) {}

        SaveOperationController producer;
        SaveOperationHandle handle;
        CancellationSource cancellation;
        SaveParticipantRegistrySnapshot participants;
        StagedRestoreContext context;
        std::shared_ptr<IRuntimeSaveWriter> writer;
        std::shared_ptr<IRuntimeSaveRestoreSource> source;
        RuntimeSaveSnapshot capture;
        std::mutex mutex;
        std::optional<RuntimeSaveRestoreStaging> staging;
        std::optional<JobHandle> job;
        std::shared_ptr<IRuntimeSaveRestoreHandoff> handoff;
        std::optional<RuntimeSavePreparedRestore> preparedRestore;
        std::optional<Error> handoffFailure;
        bool handoffAttempted{};
        bool completionPublished{};
    };

    /** @brief Finds only the actual remaining sole producer, if it has not been transferred to a delegate. */
    [[nodiscard]] SaveOperationController *OwnedProducer(Work &work) noexcept;
    /** @brief Reports original failures without publishing through a moved producer or guessing aggregate completion. */
    [[nodiscard]] Result<void> FailOwned(Work &work, Error error, SaveOperationCommitOutcome outcome);
    /** @brief Transfers the original settled staging packet at the existing owner safe point exactly once. */
    [[nodiscard]] Result<void> DeliverRestore(Work &work, RuntimeSaveRestoreStaging staging);
    /** @brief Requests retirement on the known delegate and preserves retained producer cancellation authority. */
    void CancelRestore(Work &work) noexcept;
    /** @brief Relays a thread-safe cancellation request to the already transferred owner on the session thread. */
    void PollRestoreCancellation(Work &work) noexcept;
    /** @brief Combines delegate quiescence with packet ownership without consuming the operation. */
    [[nodiscard]] bool RestoreSettled(const Work &work) noexcept;
}  // namespace Horo::Runtime::RuntimeSaveSessionDetail
