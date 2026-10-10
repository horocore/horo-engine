#pragma once

/**
 * @file SaveOperationArbiter.h
 * @brief Bounded owner-thread arbitration for incompatible runtime save and load operations.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Save/SaveNamespace.h"
#include "Horo/Runtime/Save/SaveOperation.h"
#include "Horo/Runtime/Save/SaveProjectPolicy.h"
#include "Horo/Runtime/Save/SaveSafePointCoordinator.h"
#include "Horo/Runtime/Save/SaveStoragePolicy.h"

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
        Terminal,
        WaitingForRetry
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

    /** @brief Finite nonblocking storage retry policy; only known uncommitted transient I/O is eligible. */
    struct SaveArbiterRetryPolicy final {
        std::uint8_t maximumRetries{}; /**< Zero disables retries; at most MaximumSaveStorageAutomaticRetries. */
        std::uint64_t initialBackoffMilliseconds{100};
        std::uint64_t maximumBackoffMilliseconds{5'000};
        std::uint64_t maximumElapsedMilliseconds{30'000};

        [[nodiscard]] bool operator==(const SaveArbiterRetryPolicy &) const noexcept = default;
    };

    /** @brief Exact original publication evidence retained across attempts; retries never recapture or change identity. */
    struct SaveArbiterRetryPreconditions final {
        SaveNamespaceAccessRequest access;
        SaveNamespaceBindingState bindingState{SaveNamespaceBindingState::Available};
        SaveRuntimeGeneration runtime;
        bool authorized{true}; /**< Trusted current product authority; false invalidates the attempt. */
        std::uint64_t catalogRevision{};
        SaveGameSlotId slot;
        std::uint64_t compatibilityRevision{1}; /**< Host version/trust policy revision validated for this archive. */
        std::optional<SlotGenerationId> expectedGeneration;
        SlotGenerationId publicationGeneration; /**< Same newly issued archive generation for every attempt. */

        [[nodiscard]] bool operator==(const SaveArbiterRetryPreconditions &other) const noexcept {
            return access.expected == other.access.expected && access.expectedRevision == other.access.expectedRevision &&
                   bindingState == other.bindingState && runtime == other.runtime && authorized == other.authorized &&
                   catalogRevision == other.catalogRevision && slot == other.slot && compatibilityRevision == other.compatibilityRevision &&
                   expectedGeneration == other.expectedGeneration && publicationGeneration == other.publicationGeneration;
        }
    };

    /** @brief Host-issued retry capability copied at operation admission, before any worker executes. */
    struct SaveArbiterRetryDescriptor final {
        SaveArbiterRetryPolicy policy;
        SaveArbiterRetryPreconditions preconditions;
        std::uint64_t admittedAtMilliseconds{}; /**< Absolute host monotonic clock baseline. */
    };

    /** @brief Bounded backoff diagnostics attached to the original operation, never a second job or terminal receipt. */
    struct SaveArbiterRetrySnapshot final {
        std::uint8_t completedRetries{};
        std::uint64_t eligibleAtMilliseconds{};
        std::optional<Error> lastError;
    };

    /** @brief Fully owned request copied at bounded arbiter admission. */
    struct SaveArbiterRequest final {
        SaveOperationDescriptor operation;
        SavePolicyMode mode{SavePolicyMode::Manual};
        std::optional<SaveArbiterAddress> address;
        SaveArbiterPriority priority{SaveArbiterPriority::Normal};
        SaveArbiterConflictPolicy conflict{SaveArbiterConflictPolicy::Reject};
        std::optional<SaveArbiterRetryDescriptor> retry;
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
        SaveArbiterRetrySnapshot retry;
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
        /** @brief Handles one storage failure before the commit gate, yielding the active slot during bounded backoff.
         * @param operation Active Save identity in Encoding or Committing; only Encoding can defer. @param failure Positively classified
         * provider failure/publication evidence. Caller retry counters are ignored; the admitted finite policy is authoritative.
         * @param monotonicMilliseconds Nondecreasing host clock.
         * @param now Host steady clock for the original operation cancellation/deadline.
         * @return True when deferred; false when the original operation terminalizes with its typed cause, or invalid-state/outcome error.
         * @pre The worker attempt has settled and released its mutation lease; immutable capture/generation pins remain owned.
         * @post Only TransientIo with NotCommitted can defer. Captured archive/pins remain host-owned; no new job is dispatched.
         */
        [[nodiscard]] Result<bool> DeferStorageRetry(OperationId operation, SaveStorageFailureInput failure,
                                                     std::uint64_t monotonicMilliseconds,
                                                     std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
        /** @brief Revalidates original evidence and resumes a due storage attempt only when manual/queued work has precedence.
         * @param operation Deferred identity. @param current Trusted current facts observed under the publication lease.
         * @param monotonicMilliseconds Nondecreasing host clock.
         * @param now Host steady clock for the original operation cancellation/deadline.
         * @return Original active snapshot when eligible, empty when deferred/cancelled/failed, or invalid-state error.
         * @post No capture, I/O, wait or callback occurs. Host must retain/revalidate its immutable archive before dispatch and
         * publication.
         */
        [[nodiscard]] Result<std::optional<SaveArbiterSnapshot>> ResumeStorageRetry(
            OperationId operation, const SaveArbiterRetryPreconditions &current, std::uint64_t monotonicMilliseconds,
            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
        /** @brief Closes admission and terminalizes queued/deferred work once; active precommit producers cancel cooperatively.
         * @return Success; active publication beyond its commit gate remains host-owned.
         */
        [[nodiscard]] Result<void> BeginShutdown();
        /** @brief Explicitly cancels queued or active pre-commit work. @param operation Identity. @return Atomic request disposition. */
        [[nodiscard]] SaveCancellationRequestResult Cancel(OperationId operation);
        /** @brief Lets the active producer acknowledge a pending cooperative cancellation. @param operation Active identity.
         * @return Success only when cancellation publishes the immutable terminal result.
         */
        [[nodiscard]] Result<void> ObserveCancellation(OperationId operation);
        /** @brief Polls caller, parent and deadline cancellation while active work awaits a safe point.
         * @param operation Active identity. @return True when cancellation terminalized it, false otherwise, or invalid identity.
         * Preserves the winning cancellation reason; never crosses a commit gate or waits for work.
         */
        [[nodiscard]] Result<bool> PollCancellation(OperationId operation);
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
