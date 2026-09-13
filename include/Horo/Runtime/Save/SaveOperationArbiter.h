#pragma once

/**
 * @file SaveOperationArbiter.h
 * @brief Bounded owner-thread arbitration for incompatible runtime save and load operations.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Save/SaveNamespace.h"
#include "Horo/Runtime/Save/SaveOperation.h"
#include "Horo/Runtime/Save/SaveProjectPolicy.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace Horo::Runtime {
    namespace SaveOperationArbiterDetail {
        struct State;
    }

    /** @brief Stable admission priority; higher values are selected first without preempting active work. */
    enum class SaveArbiterPriority : std::uint8_t {
        Background,
        Normal,
        UserBlocking
    };

    /** @brief Explicit disposition for an incompatible or equivalent queued request. */
    enum class SaveArbiterConflictPolicy : std::uint8_t {
        Reject,
        Queue,
        CoalesceEquivalent,
        ReplaceQueuedEquivalent
    };

    /** @brief Owner-controlled lifecycle projection spanning safe-point, worker, and commit phases. */
    enum class SaveArbiterState : std::uint8_t {
        Queued,
        WaitingForSafePoint,
        Capturing,
        Encoding,
        Committing,
        Loading,
        Activating,
        Cancelling,
        Terminal
    };

    /** @brief Result category for one successful admission attempt. */
    enum class SaveArbiterAdmissionDisposition : std::uint8_t {
        Accepted,
        Coalesced,
        ReplacedQueued
    };

    /** @brief Typed logical target used only for slot-conflict arbitration. */
    struct SaveArbiterAddress final {
        SaveNamespaceId nameSpace;
        SaveGameSlotId slot;

        [[nodiscard]] constexpr auto operator<=>(const SaveArbiterAddress &) const noexcept = default;
    };

    /** @brief Fully owned request copied at bounded arbiter admission. */
    struct SaveArbiterRequest final {
        SaveOperationDescriptor operation;
        SavePolicyMode mode{SavePolicyMode::Manual};
        std::optional<SaveArbiterAddress> address;
        SaveArbiterPriority priority{SaveArbiterPriority::Normal};
        SaveArbiterConflictPolicy conflict{SaveArbiterConflictPolicy::Reject};
    };

    /** @brief Immutable copy returned to polling UI, host, or headless callers. */
    struct SaveArbiterSnapshot final {
        SaveOperationSnapshot operation;
        SaveArbiterState state{SaveArbiterState::Queued};
        SavePolicyMode mode{SavePolicyMode::Manual};
        std::optional<SaveArbiterAddress> address;
        SaveArbiterPriority priority{SaveArbiterPriority::Normal};
        std::uint64_t enqueueOrder{};
        std::uint64_t revision{1};
    };

    /** @brief Successful admission including the effective operation after coalescing. */
    struct SaveArbiterAdmission final {
        SaveOperationHandle handle;
        SaveArbiterAdmissionDisposition disposition{SaveArbiterAdmissionDisposition::Accepted};
        std::optional<OperationId> replacedOperation;
    };

    /** @brief Finite retained-operation capacity validated before arbiter construction. */
    struct SaveOperationArbiterLimits final {
        std::size_t maximumRetainedOperations{};
    };

    /**
     * @brief Owner-thread state machine that serializes incompatible save-domain operations.
     *
     * The arbiter owns producer controllers until acknowledgement or destruction. Consumer
     * handle release and observer removal therefore never cancel admitted work implicitly.
     */
    class SaveOperationArbiter final {
    public:
        ~SaveOperationArbiter();
        SaveOperationArbiter(SaveOperationArbiter &&) noexcept;
        SaveOperationArbiter &operator=(SaveOperationArbiter &&) noexcept;
        SaveOperationArbiter(const SaveOperationArbiter &) = delete;
        SaveOperationArbiter &operator=(const SaveOperationArbiter &) = delete;

        /** @brief Validates, copies, queues, coalesces, or replaces one request. @param request Candidate request.
         * @return Effective consumer handle and disposition, or stable invalid/conflict/capacity error.
         */
        [[nodiscard]] Result<SaveArbiterAdmission> Admit(SaveArbiterRequest request);
        /** @brief Selects the highest-priority oldest queued request when idle. @return Started snapshot, or empty when busy/idle. */
        [[nodiscard]] std::optional<SaveArbiterSnapshot> StartNext();
        /** @brief Advances the active request through one allowed nonterminal phase. @param operation Active identity.
         * @param state Requested next phase. @param progress Exact phase-local progress.
         * @return Success or an invalid-transition error.
         */
        [[nodiscard]] Result<void> Advance(OperationId operation, SaveArbiterState state, SaveOperationProgress progress = {});
        /** @brief Completes the active operation after its commit contract is satisfied. @param operation Active identity.
         * @return Success or an invalid-transition error.
         */
        [[nodiscard]] Result<void> Complete(OperationId operation);
        /** @brief Fails the active operation with its original typed cause. @param operation Active identity. @param error Cause.
         * @param outcome Publication evidence. @return Success or an invalid-transition error.
         */
        [[nodiscard]] Result<void> Fail(OperationId operation, Error error,
                                        SaveOperationCommitOutcome outcome = SaveOperationCommitOutcome::NotCommitted);
        /** @brief Explicitly cancels queued or active pre-commit work. @param operation Identity. @return Atomic request disposition. */
        [[nodiscard]] SaveCancellationRequestResult Cancel(OperationId operation);
        /** @brief Lets the active producer acknowledge a pending cooperative cancellation. @param operation Active identity.
         * @return Success only when cancellation publishes the immutable terminal result.
         */
        [[nodiscard]] Result<void> ObserveCancellation(OperationId operation);
        /** @brief Copies one retained immutable projection. @param operation Identity. @return Empty for unknown/acknowledged identity. */
        [[nodiscard]] std::optional<SaveArbiterSnapshot> Snapshot(OperationId operation) const;
        /** @brief Releases one terminal retained record. @param operation Identity. @return True only when terminal state was erased. */
        [[nodiscard]] bool Acknowledge(OperationId operation);
        /** @brief Returns active identity, if any. @return Empty while idle. */
        [[nodiscard]] std::optional<OperationId> ActiveOperation() const noexcept;
        /** @brief Returns queued request count. @return Bounded count. */
        [[nodiscard]] std::size_t QueuedCount() const noexcept;

    private:
        explicit SaveOperationArbiter(std::unique_ptr<SaveOperationArbiterDetail::State> state) noexcept;
        friend Result<SaveOperationArbiter> CreateSaveOperationArbiter(SaveOperationArbiterLimits limits);

        std::unique_ptr<SaveOperationArbiterDetail::State> state_;
    };

    /** @brief Creates an empty bounded owner-thread arbiter. @param limits Positive finite capacity.
     * @return Arbiter or stable configuration/allocation failure.
     */
    [[nodiscard]] Result<SaveOperationArbiter> CreateSaveOperationArbiter(SaveOperationArbiterLimits limits);
}  // namespace Horo::Runtime
