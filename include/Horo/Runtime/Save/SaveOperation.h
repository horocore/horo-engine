#pragma once

/**
 * @file SaveOperation.h
 * @brief Thread-safe asynchronous runtime-save operation progress and cancellation contract.
 */

#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/OperationStore.h"
#include "Horo/Foundation/Result.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace Horo::Runtime {
    namespace SaveOperationDetail {
        struct SharedState;
    }
    class SaveOperationController;

    /** @brief Save-domain operation admitted under an application-owned OperationId. */
    enum class SaveOperationKind : std::uint8_t {
        Save,
        Load,
        RefreshCatalog,
        Delete
    };

    /** @brief Observable nonterminal and terminal operation lifecycle. */
    enum class SaveOperationState : std::uint8_t {
        Queued,
        Running,
        Completed,
        Failed,
        Cancelled
    };

    /** @brief Typed save/load pipeline stage; operations may skip stages not applicable to their kind. */
    enum class SaveOperationStage : std::uint8_t {
        Queued,
        CapturingSnapshot,
        Serializing,
        FinalizingArchive,
        WritingTemporary,
        CommitStarted,
        VerifyingArchive,
        Migrating,
        PreparingRestore,
        ReadyToCommit,
        ApplyingState,
        RefreshingCatalog,
        Deleting
    };

    /** @brief Durable publication evidence carried by terminal operation snapshots. */
    enum class SaveOperationCommitOutcome : std::uint8_t {
        NotCommitted,
        Committed,
        Unknown
    };

    /** @brief Source of the cooperative cancellation request that won before commit. */
    enum class SaveCancellationReason : std::uint8_t {
        None,
        Caller,
        Parent,
        Deadline,
        Shutdown
    };

    /** @brief Result of a non-blocking cancellation request. */
    enum class SaveCancellationRequestResult : std::uint8_t {
        Requested,
        AlreadyRequested,
        TooLate,
        AlreadyTerminal,
        InvalidHandle
    };

    /** @brief Result of the producer's atomic cancellation observation. */
    enum class SaveCancellationObservation : std::uint8_t {
        NotRequested,
        Cancelled,
        TooLate,
        AlreadyTerminal
    };

    /** @brief Result of atomically entering the non-cancellable publication window. */
    enum class SaveCommitGateResult : std::uint8_t {
        Entered,
        CancellationWon,
        AlreadyEntered,
        NotReady,
        NotRequired,
        AlreadyTerminal
    };

    /** @brief Result of a producer progress or terminal state transition. */
    enum class SaveOperationTransitionResult : std::uint8_t {
        Applied,
        CancellationWon,
        AlreadyTerminal,
        InvalidTransition
    };

    /** @brief Bounded exact work-unit progress for one typed stage. */
    struct SaveOperationProgress final {
        std::uint64_t completedUnits{}; /**< Completed work units, no greater than totalUnits. */
        std::uint64_t totalUnits{1};    /**< Positive stage-local work-unit bound. */

        /** @brief Returns normalized progress without changing the exact counters. @return Value in [0, 1]. */
        [[nodiscard]] double Fraction() const noexcept;
    };

    /** @brief Owned immutable projection safe to copy from any allowed thread. */
    struct SaveOperationSnapshot final {
        OperationId operation{};                                                     /**< Application OperationStore identity. */
        SaveOperationKind kind{SaveOperationKind::Save};                             /**< Admitted operation kind. */
        SaveOperationState state{SaveOperationState::Queued};                        /**< Current or terminal lifecycle state. */
        SaveOperationStage stage{SaveOperationStage::Queued};                        /**< Current typed pipeline stage. */
        SaveOperationProgress progress{};                                            /**< Exact stage-local progress. */
        SaveOperationCommitOutcome commit{SaveOperationCommitOutcome::NotCommitted}; /**< Publication evidence. */
        SaveCancellationReason cancellationReason{SaveCancellationReason::None};     /**< Winning/requested cause. */
        bool cancellationRequested{};                                                /**< Cooperative request is pending or won. */
        bool cancellable{true};                                                      /**< False after the atomic commit gate. */
        std::optional<std::chrono::steady_clock::time_point> deadline;               /**< Process-local admitted deadline. */
        std::optional<Error> terminalError;                                          /**< Preserved terminal cause, when any. */
        std::uint64_t revision{1};                                                   /**< Monotonic snapshot revision. */

        /** @brief Reports whether this snapshot is immutable terminal state. @return True for completed, failed, or cancelled. */
        [[nodiscard]] bool IsTerminal() const noexcept;
    };

    /** @brief Completion observer invoked exactly once outside the operation lock. */
    using SaveOperationCompletionCallback = std::function<void(const SaveOperationSnapshot &)>;

    /** @brief Qualified upper bound for retained completion observers on one operation. */
    inline constexpr std::size_t MaximumSaveOperationCompletionCallbacks = 64;

    /** @brief Inputs validated and copied when an application operation is admitted. */
    struct SaveOperationDescriptor final {
        OperationId operation{};                                       /**< Non-zero identity allocated by OperationStore. */
        SaveOperationKind kind{SaveOperationKind::Save};               /**< Admitted operation kind. */
        std::size_t maximumCompletionCallbacks{};                      /**< Positive bounded retained callback count. */
        std::optional<std::chrono::steady_clock::time_point> deadline; /**< Optional cooperative deadline. */
        CancellationToken parentCancellation;                          /**< Optional parent session/shutdown cancellation. */
    };

    /** @brief Copyable thread-safe polling, cancellation, and completion-observation capability. */
    class SaveOperationHandle final {
    public:
        SaveOperationHandle() = default;

        /** @brief Reports whether this handle owns an admitted operation. @return True for a usable handle. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns the application operation identity. @return Zero only for an invalid handle. */
        [[nodiscard]] OperationId Id() const noexcept;
        /** @brief Copies the latest state without waiting for work or performing I/O. @return Empty for an invalid handle. */
        [[nodiscard]] std::optional<SaveOperationSnapshot> Snapshot() const;
        /** @brief Requests cooperative caller cancellation without waiting. @return Atomic request disposition. */
        [[nodiscard]] SaveCancellationRequestResult RequestCancellation() const noexcept;
        /** @brief Registers an observer or invokes it immediately for terminal state.
         * @param callback Non-empty observer; it runs on the registering or terminalizing thread and must not block.
         * @return Success or a stable invalid/capacity error.
         */
        [[nodiscard]] Result<void> OnCompletion(SaveOperationCompletionCallback callback) const;

    private:
        explicit SaveOperationHandle(std::shared_ptr<SaveOperationDetail::SharedState> state) noexcept;
        friend class SaveOperationController;

        std::shared_ptr<SaveOperationDetail::SharedState> state_;
    };

    /** @brief Move-only producer capability that guarantees a terminal result on release. */
    class SaveOperationController final {
    public:
        /** @brief Publishes abandonment if this producer still owns a nonterminal operation. */
        ~SaveOperationController();
        /** @brief Transfers the sole producer capability without changing operation state. */
        SaveOperationController(SaveOperationController &&other) noexcept;
        /** @brief Installs the replacement capability, then abandons the detached prior operation. */
        SaveOperationController &operator=(SaveOperationController &&other) noexcept;
        SaveOperationController(const SaveOperationController &) = delete;
        SaveOperationController &operator=(const SaveOperationController &) = delete;

        /** @brief Returns a copyable consumer handle. @return Handle sharing this operation state. */
        [[nodiscard]] SaveOperationHandle Handle() const noexcept;
        /** @brief Publishes bounded stage progress, or lets pre-commit cancellation win.
         * @param stage Current typed pipeline stage.
         * @param progress Exact stage-local progress.
         * @param now Current monotonic time used to observe the deadline.
         * @return Atomic transition disposition.
         * @pre Pre-commit stages do not regress; after commit only the kind's commit stage is accepted.
         */
        [[nodiscard]] SaveOperationTransitionResult PublishProgress(
            SaveOperationStage stage, SaveOperationProgress progress,
            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
        /** @brief Atomically observes caller, parent, deadline, or shutdown cancellation.
         * @param now Current monotonic time used to observe the deadline.
         * @return Cancellation disposition; Cancelled publishes terminal state exactly once.
         */
        [[nodiscard]] SaveCancellationObservation ObserveCancellation(
            std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
        /** @brief Atomically enters the non-cancellable commit window or lets prior cancellation win.
         * @param now Current monotonic time used to observe the deadline.
         * @return Commit-gate disposition.
         * @pre The kind-specific ready stage has published complete progress.
         */
        [[nodiscard]] SaveCommitGateResult BeginCommit(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
        /** @brief Requests cancellation on behalf of owning-session shutdown without waiting. @return Atomic disposition. */
        [[nodiscard]] SaveCancellationRequestResult RequestShutdownCancellation() const noexcept;
        /** @brief Publishes immutable successful completion exactly once.
         * @param outcome Committed for mutations, NotCommitted for catalog refresh.
         * @param now Current monotonic time used for the final pre-commit cancellation check.
         * @return Atomic terminal transition disposition.
         */
        [[nodiscard]] SaveOperationTransitionResult Complete(SaveOperationCommitOutcome outcome,
                                                             std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
        /** @brief Publishes immutable failure while preserving its typed cause.
         * @param error Original typed operation failure.
         * @param outcome Known or unknown publication evidence.
         * @param now Current monotonic time used for the final pre-commit cancellation check.
         * @return Atomic terminal transition disposition.
         * @details Lvalue errors are copied before attempting the transition; allocation failure leaves the operation unchanged.
         * An rvalue error transfers into storage reserved at admission without constructing another Error on the terminal path.
         */
        [[nodiscard]] SaveOperationTransitionResult Fail(const Error &error, SaveOperationCommitOutcome outcome,
                                                         std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
        /** @copydoc SaveOperationController::Fail */
        [[nodiscard]] SaveOperationTransitionResult Fail(Error &&error, SaveOperationCommitOutcome outcome,
                                                         std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    private:
        explicit SaveOperationController(std::shared_ptr<SaveOperationDetail::SharedState> state) noexcept;
        friend Result<SaveOperationController> CreateSaveOperation(SaveOperationDescriptor descriptor);

        void Abandon() noexcept;
        std::shared_ptr<SaveOperationDetail::SharedState> state_;
    };

    /** @brief Creates producer/consumer state for one already-admitted application operation.
     * @param descriptor Non-zero OperationStore identity, kind, deadline, parent token, and callback capacity.
     * @return Move-only producer, OperationInvalid, or OperationAllocationFailed.
     */
    [[nodiscard]] Result<SaveOperationController> CreateSaveOperation(SaveOperationDescriptor descriptor);
}  // namespace Horo::Runtime
