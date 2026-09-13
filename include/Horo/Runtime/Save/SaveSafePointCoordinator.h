#pragma once

/**
 * @file SaveSafePointCoordinator.h
 * @brief Owner-thread safe-point and generation fencing for runtime-save work.
 */

#include "Horo/Foundation/OperationStore.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/FrameScheduler.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace Horo::Runtime {
    /** @brief Qualified hard ceiling for save operations retained by one runtime session. */
    inline constexpr std::size_t MaximumSaveLifecycleOperations = 4096;

    /** @brief Exact runtime, scene, and participant-registry incarnation addressed by save work. */
    struct SaveRuntimeGeneration final {
        std::uint64_t runtime{};  /**< Non-zero runtime-session generation. */
        std::uint64_t scene{};    /**< Non-zero active scene incarnation. */
        std::uint64_t registry{}; /**< Non-zero participant-registry generation. */

        /** @brief Reports whether every generation was issued. @return True when no component is zero. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return runtime != 0 && scene != 0 && registry != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const SaveRuntimeGeneration &) const noexcept = default;
    };

    /** @brief Simulation-owned action fenced by the coordinator. */
    enum class SaveSafePointAction : std::uint8_t {
        Capture,
        Restore
    };

    /** @brief Observable lifecycle of one fenced capture or restore operation. */
    enum class SaveSafePointOperationState : std::uint8_t {
        AwaitingSafePoint,
        DetachedWork,
        ReadyToApply,
        ExecutingAtSafePoint,
        Completed,
        Failed,
        Cancelled,
        Stale
    };

    /** @brief Deterministic terminal/defer cause owned by the runtime lifecycle. */
    enum class SaveLifecycleDisposition : std::uint8_t {
        None,
        SceneTransition,
        RegistryRebind,
        PieStop,
        HostShutdown,
        WorkerFailure,
        WorkerCancellation,
        SafePointFailure
    };

    /** @brief One already-admitted application operation awaiting lifecycle coordination. */
    struct SaveSafePointOperationDescriptor final {
        OperationId operation{};            /**< Non-zero application OperationStore identity. */
        SaveSafePointAction action{};       /**< Capture or restore simulation-owned state. */
        SaveRuntimeGeneration generation{}; /**< Exact target incarnation. */
    };

    /** @brief Immutable polling projection for one lifecycle-fenced operation. */
    struct SaveSafePointOperationSnapshot final {
        SaveSafePointOperationDescriptor descriptor;                                       /**< Original admitted identity and target. */
        SaveSafePointOperationState state{SaveSafePointOperationState::AwaitingSafePoint}; /**< Current/terminal state. */
        SaveLifecycleDisposition disposition{SaveLifecycleDisposition::None};              /**< Lifecycle outcome cause. */
        std::optional<Error> terminalError;                                                /**< Original safe-point/worker failure. */
        std::uint64_t revision{1};                                                         /**< Monotonic observation revision. */

        /** @brief Reports whether this state can no longer change. @return True for terminal states. */
        [[nodiscard]] bool IsTerminal() const noexcept;
    };

    /** @brief Outcome published by detached worker work without touching simulation-owned state. */
    enum class SaveWorkerCompletionOutcome : std::uint8_t {
        Succeeded,
        Failed,
        Cancelled
    };

    /** @brief Generation-scoped completion accepted from an arbitrary worker thread. */
    struct SaveWorkerCompletion final {
        OperationId operation{};               /**< Existing fenced operation identity. */
        SaveRuntimeGeneration generation{};    /**< Exact generation copied at admission. */
        SaveWorkerCompletionOutcome outcome{}; /**< Success, typed failure, or cancellation. */
        std::optional<Error> error;            /**< Required only for Failed. */
    };

    /** @brief Bounded result of one owner-thread lifecycle commit safe point. */
    struct SaveSafePointDrainResult final {
        std::size_t inspected{}; /**< Eligible operations inspected under the requested budget. */
        std::size_t captured{};  /**< Captures detached successfully at this boundary. */
        std::size_t restored{};  /**< Restore candidates applied successfully at this boundary. */
        std::size_t failed{};    /**< Safe-point callbacks that failed and became terminal. */
    };

    /**
     * @brief Host seam invoked only on the owner thread inside CommitDeferredLifecycleChanges.
     *
     * Implementations report expected failures through Result and must not throw. The coordinator
     * nevertheless contains unexpected exceptions and terminalizes the affected operation.
     */
    class ISaveSafePointExecutor {
    public:
        virtual ~ISaveSafePointExecutor() = default;

        /** @brief Detaches one coherent immutable capture. @param operation Application operation identity.
         * @param generation Exact active generation. @return Success or the original typed capture failure.
         */
        [[nodiscard]] virtual Result<void> Capture(OperationId operation, SaveRuntimeGeneration generation) = 0;
        /** @brief Applies one fully prepared restore candidate. @param operation Application operation identity.
         * @param generation Exact active generation. @return Success or the original typed restore failure.
         */
        [[nodiscard]] virtual Result<void> Restore(OperationId operation, SaveRuntimeGeneration generation) = 0;
    };

    namespace SaveSafePointDetail {
        struct SharedState;
    }

    /**
     * @brief Session-owned coordinator that prevents save paths from racing live simulation mutation.
     *
     * Admission, lifecycle transitions, and safe-point drains are restricted to the construction
     * thread. Worker completion publication and immutable snapshot polling are thread-safe.
     */
    class SaveSafePointCoordinator final {
    public:
        /** @brief Creates a coordinator bound to the calling owner thread.
         * @param activeGeneration Exact currently active runtime/scene/registry incarnation.
         * @param maximumOperations Positive retained-operation capacity within the qualified maximum.
         * @return Owned coordinator or a typed invalid/allocation error.
         */
        [[nodiscard]] static Result<std::unique_ptr<SaveSafePointCoordinator>> Create(SaveRuntimeGeneration activeGeneration,
                                                                                      std::size_t maximumOperations);

        ~SaveSafePointCoordinator();
        SaveSafePointCoordinator(const SaveSafePointCoordinator &) = delete;
        SaveSafePointCoordinator &operator=(const SaveSafePointCoordinator &) = delete;

        /** @brief Admits one capture or restore fence on the owner thread.
         * @param descriptor Exact operation, action, and active generation.
         * @return Success or a typed affinity, stale, duplicate, lifecycle, capacity, or allocation error.
         */
        [[nodiscard]] Result<void> Admit(const SaveSafePointOperationDescriptor &descriptor);

        /** @brief Publishes detached worker outcome from any thread without invoking simulation code.
         * @param completion Exact operation/generation and outcome.
         * @return Success or a typed malformed, stale, or state error.
         */
        [[nodiscard]] Result<void> PublishWorkerCompletion(SaveWorkerCompletion completion);

        /** @brief Runs bounded simulation work at the sole declared lifecycle commit safe point.
         * @param phase Must be RuntimePhase::CommitDeferredLifecycleChanges.
         * @param generation Must exactly match the active generation.
         * @param maximumOperations Positive number of eligible operations to inspect.
         * @param executor Host-owned simulation seam; callbacks run synchronously on the owner thread.
         * @return Drain counts or a typed affinity, phase, generation, suspension, or lifecycle error.
         */
        [[nodiscard]] Result<SaveSafePointDrainResult> CommitAtSafePoint(RuntimePhase phase, SaveRuntimeGeneration generation,
                                                                         std::size_t maximumOperations, ISaveSafePointExecutor &executor);

        /** @brief Publishes a replacement runtime/scene/registry generation on the owner thread.
         * @param generation New exact active generation, distinct from the current value.
         * @return Success or a typed affinity, invalid, duplicate, or shutdown error.
         * @post Uncaptured work and all restore work for the prior scene become terminal Stale;
         * detached captures remain valid for worker serialization.
         */
        [[nodiscard]] Result<void> TransitionScene(SaveRuntimeGeneration generation);

        /** @brief Publishes a quiescent participant-registry generation rebind on the owner thread.
         * @param generation Generation with the current runtime/scene and a distinct non-zero registry value.
         * @return Success or a typed affinity, invalid, reentrant, or shutdown error.
         * @post Uncaptured work and all restore work for the prior registry become terminal Stale;
         * detached captures retain their pinned registry leases and may finish serialization.
         */
        [[nodiscard]] Result<void> RebindRegistry(SaveRuntimeGeneration generation);

        /** @brief Enters or leaves suspension on the owner thread without running save callbacks.
         * @param suspended True to defer all safe-point work; false to resume eligibility.
         * @return Success or a typed affinity/shutdown error.
         */
        [[nodiscard]] Result<void> SetSuspended(bool suspended);

        /** @brief Cancels every nonterminal operation for deterministic PIE stop on the owner thread. @return Success or affinity error. */
        [[nodiscard]] Result<void> OnPieStop();
        /** @brief Closes admission and cancels every nonterminal operation for host teardown. Idempotent on the owner thread.
         * @return Success or a typed affinity error.
         */
        [[nodiscard]] Result<void> BeginShutdown();

        /** @brief Copies one operation state from any thread. @param operation Existing operation identity.
         * @return Immutable snapshot or a typed unknown-operation error.
         */
        [[nodiscard]] Result<SaveSafePointOperationSnapshot> Snapshot(OperationId operation) const;

        /** @brief Releases one terminal coordinator record on the owner thread.
         * @param operation Existing terminal operation identity.
         * @return Success or a typed affinity, unknown-operation, or nonterminal-state error.
         */
        [[nodiscard]] Result<void> Acknowledge(OperationId operation);

    private:
        SaveSafePointCoordinator(std::shared_ptr<SaveSafePointDetail::SharedState> state) noexcept;
        std::shared_ptr<SaveSafePointDetail::SharedState> state_;
    };
}  // namespace Horo::Runtime
