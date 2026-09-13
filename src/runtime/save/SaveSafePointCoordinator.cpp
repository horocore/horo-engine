#include "Horo/Runtime/Save/SaveSafePointCoordinator.h"

#include "Horo/Runtime/Save/SaveErrors.h"

#include <algorithm>
#include <exception>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Runtime {
    namespace SaveSafePointDetail {
        struct SharedState final {
            mutable std::mutex mutex;  // NOSONAR(cpp:S8379) - translation-unit state is accessed only by the guarded helpers below.
            std::thread::id ownerThread;
            SaveRuntimeGeneration activeGeneration;
            std::size_t maximumOperations{};
            std::vector<SaveSafePointOperationSnapshot> operations;
            bool suspended{};
            bool stopped{};
            bool shuttingDown{};
            bool safePointActive{};
        };
    }  // namespace SaveSafePointDetail

    namespace {
        using SharedState = SaveSafePointDetail::SharedState;

        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor) {
            return Result<T>::Failure(MakeError(descriptor));
        }

        [[nodiscard]] bool IsKnown(const SaveSafePointAction action) noexcept {
            return action == SaveSafePointAction::Capture || action == SaveSafePointAction::Restore;
        }

        [[nodiscard]] bool IsKnown(const SaveWorkerCompletionOutcome outcome) noexcept {
            using enum SaveWorkerCompletionOutcome;
            return outcome == Succeeded || outcome == Failed || outcome == Cancelled;
        }

        [[nodiscard]] bool IsOwnerThread(const SharedState &state) noexcept {
            return state.ownerThread == std::this_thread::get_id();
        }

        template <typename State> [[nodiscard]] auto FindOperation(State &state, const OperationId operation) {
            return std::ranges::find(state.operations, operation, [](const SaveSafePointOperationSnapshot &snapshot) {
                return snapshot.descriptor.operation;
            });
        }

        void Terminalize(SaveSafePointOperationSnapshot &snapshot, const SaveSafePointOperationState state,
                         const SaveLifecycleDisposition disposition, std::optional<Error> error = std::nullopt) {
            snapshot.state = state;
            snapshot.disposition = disposition;
            snapshot.terminalError = std::move(error);
            ++snapshot.revision;
        }

        [[nodiscard]] bool IsEligible(const SaveSafePointOperationSnapshot &snapshot) noexcept {
            return (snapshot.descriptor.action == SaveSafePointAction::Capture &&
                    snapshot.state == SaveSafePointOperationState::AwaitingSafePoint) ||
                   (snapshot.descriptor.action == SaveSafePointAction::Restore &&
                    snapshot.state == SaveSafePointOperationState::ReadyToApply);
        }

        [[nodiscard]] Result<void> ValidateOwner(const SharedState &state) {
            if (!IsOwnerThread(state))
                return Failure<void>(SaveErrors::ThreadAffinityViolation);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateLifecycleMutation(const SharedState &state) {
            if (state.shuttingDown)
                return Failure<void>(SaveErrors::LifecycleUnavailable);
            if (state.safePointActive)
                return Failure<void>(SaveErrors::LifecycleReentrant);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateAvailable(const SharedState &state) {
            if (const Result<void> lifecycle = ValidateLifecycleMutation(state); lifecycle.HasError())
                return lifecycle;
            if (state.stopped)
                return Failure<void>(SaveErrors::LifecycleUnavailable);
            return Result<void>::Success();
        }

        template <typename Mutation> [[nodiscard]] Result<void> MutateOnOwner(SharedState &state, Mutation &&mutation) {
            if (const Result<void> owner = ValidateOwner(state); owner.HasError())
                return owner;
            std::lock_guard lock(state.mutex);
            return std::forward<Mutation>(mutation)();
        }

        [[nodiscard]] Result<void> ValidateAdmission(const SharedState &state, const SaveSafePointOperationDescriptor &descriptor) {
            if (const Result<void> available = ValidateAvailable(state); available.HasError())
                return available;
            if (descriptor.operation == 0 || !IsKnown(descriptor.action) || !descriptor.generation.IsValid())
                return Failure<void>(SaveErrors::LifecycleInvalid);
            if (descriptor.generation != state.activeGeneration)
                return Failure<void>(SaveErrors::GenerationStale);
            if (FindOperation(state, descriptor.operation) != state.operations.end())
                return Failure<void>(SaveErrors::OperationInvalid);
            if (state.operations.size() == state.maximumOperations)
                return Failure<void>(SaveErrors::LifecycleCapacityExceeded);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateWorkerCompletion(const SaveWorkerCompletion &completion) {
            if (completion.operation == 0 || !completion.generation.IsValid() || !IsKnown(completion.outcome) ||
                (completion.outcome == SaveWorkerCompletionOutcome::Failed) != completion.error.has_value())
                return Failure<void>(SaveErrors::CompletionInvalid);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ApplyWorkerCompletion(SaveSafePointOperationSnapshot &operation, SaveWorkerCompletion completion) {
            using enum SaveWorkerCompletionOutcome;
            if (completion.outcome == Succeeded) {
                if (operation.descriptor.action == SaveSafePointAction::Capture)
                    Terminalize(operation, SaveSafePointOperationState::Completed, SaveLifecycleDisposition::None);
                else {
                    operation.state = SaveSafePointOperationState::ReadyToApply;
                    ++operation.revision;
                }
                return Result<void>::Success();
            }
            if (completion.outcome == Failed) {
                Terminalize(operation, SaveSafePointOperationState::Failed, SaveLifecycleDisposition::WorkerFailure,
                            std::move(completion.error));
                return Result<void>::Success();
            }
            Terminalize(operation, SaveSafePointOperationState::Cancelled, SaveLifecycleDisposition::WorkerCancellation,
                        MakeError(SaveErrors::OperationCancelled));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateSafePointRequest(const SharedState &state, const RuntimePhase phase,
                                                            const SaveRuntimeGeneration generation, const std::size_t maximumOperations) {
            if (phase != RuntimePhase::CommitDeferredLifecycleChanges || maximumOperations == 0 ||
                maximumOperations > state.maximumOperations)
                return Failure<void>(SaveErrors::SafePointInvalid);
            if (const Result<void> available = ValidateAvailable(state); available.HasError())
                return available;
            if (state.suspended)
                return Failure<void>(SaveErrors::LifecycleSuspended);
            if (!generation.IsValid() || generation != state.activeGeneration)
                return Failure<void>(SaveErrors::GenerationStale);
            return Result<void>::Success();
        }

        struct SafePointSelection final {
            std::size_t index{};
            SaveSafePointOperationDescriptor descriptor;
        };

        [[nodiscard]] std::optional<SafePointSelection> SelectSafePointOperation(SharedState &state, std::size_t &cursor) {
            for (; cursor < state.operations.size(); ++cursor) {
                if (!IsEligible(state.operations[cursor]))
                    continue;
                const std::size_t selected = cursor++;
                SaveSafePointOperationSnapshot &operation = state.operations[selected];
                operation.state = SaveSafePointOperationState::ExecutingAtSafePoint;
                ++operation.revision;
                return SafePointSelection{.index = selected, .descriptor = operation.descriptor};
            }
            return std::nullopt;
        }

        void ApplySafePointOutcome(SaveSafePointOperationSnapshot &operation, const SaveSafePointAction action,
                                   const Result<void> &executed, SaveSafePointDrainResult &result) {
            using enum SaveSafePointAction;
            using enum SaveSafePointOperationState;
            if (executed.HasError()) {
                Terminalize(operation, Failed, SaveLifecycleDisposition::SafePointFailure, executed.ErrorValue());
                ++result.failed;
                return;
            }
            if (action == Capture) {
                operation.state = DetachedWork;
                ++operation.revision;
                ++result.captured;
                return;
            }
            Terminalize(operation, Completed, SaveLifecycleDisposition::None);
            ++result.restored;
        }

        void InvalidatePriorGenerationWork(SharedState &state, const SaveLifecycleDisposition disposition) {
            for (SaveSafePointOperationSnapshot &operation : state.operations) {
                const bool uncaptured = operation.descriptor.action == SaveSafePointAction::Capture &&
                                        operation.state == SaveSafePointOperationState::AwaitingSafePoint;
                const bool restore = operation.descriptor.action == SaveSafePointAction::Restore && !operation.IsTerminal();
                if (uncaptured || restore)
                    Terminalize(operation, SaveSafePointOperationState::Stale, disposition, MakeError(SaveErrors::GenerationStale));
            }
        }

        [[nodiscard]] Result<void> ApplySceneTransition(SharedState &state, const SaveRuntimeGeneration generation) {
            if (const Result<void> validation = ValidateLifecycleMutation(state); validation.HasError())
                return validation;
            if (!generation.IsValid() || generation == state.activeGeneration)
                return Failure<void>(SaveErrors::LifecycleInvalid);
            InvalidatePriorGenerationWork(state, SaveLifecycleDisposition::SceneTransition);
            state.activeGeneration = generation;
            state.stopped = false;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ApplyRegistryRebind(SharedState &state, const SaveRuntimeGeneration generation) {
            const SaveRuntimeGeneration active = state.activeGeneration;
            if (const Result<void> validation = ValidateLifecycleMutation(state); validation.HasError())
                return validation;
            if (!generation.IsValid() || generation.runtime != active.runtime || generation.scene != active.scene ||
                generation.registry == active.registry)
                return Failure<void>(SaveErrors::LifecycleInvalid);
            InvalidatePriorGenerationWork(state, SaveLifecycleDisposition::RegistryRebind);
            state.activeGeneration = generation;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ApplySuspension(SharedState &state, const bool suspended) {
            if (const Result<void> validation = ValidateLifecycleMutation(state); validation.HasError())
                return validation;
            state.suspended = suspended;
            return Result<void>::Success();
        }

        void CancelNonterminal(SharedState &state, const SaveLifecycleDisposition disposition) {
            for (SaveSafePointOperationSnapshot &snapshot : state.operations) {
                if (!snapshot.IsTerminal())
                    Terminalize(snapshot, SaveSafePointOperationState::Cancelled, disposition, MakeError(SaveErrors::OperationCancelled));
            }
        }

        [[nodiscard]] Result<void> ExecuteContained(ISaveSafePointExecutor &executor, const SaveSafePointOperationDescriptor &descriptor) {
            try {
                return descriptor.action == SaveSafePointAction::Capture ? executor.Capture(descriptor.operation, descriptor.generation)
                                                                         : executor.Restore(descriptor.operation, descriptor.generation);
            } catch (...) {  // NOSONAR(cpp:S1181, cpp:S2738) - public host seam containment boundary.
                return Failure<void>(SaveErrors::LifecycleCallbackFailed);
            }
        }

        class SafePointScope final {
        public:
            explicit SafePointScope(std::shared_ptr<SharedState> state) noexcept : state_(std::move(state)) {}

            ~SafePointScope() {
                std::lock_guard lock(state_->mutex);
                state_->safePointActive = false;
            }

            SafePointScope(const SafePointScope &) = delete;
            SafePointScope &operator=(const SafePointScope &) = delete;

        private:
            std::shared_ptr<SharedState> state_;
        };
    }  // namespace

    /** @copydoc SaveSafePointOperationSnapshot::IsTerminal */
    bool SaveSafePointOperationSnapshot::IsTerminal() const noexcept {
        using enum SaveSafePointOperationState;
        return state == Completed || state == Failed || state == Cancelled || state == Stale;
    }

    /** @copydoc SaveSafePointCoordinator::Create */
    Result<std::unique_ptr<SaveSafePointCoordinator>> SaveSafePointCoordinator::Create(const SaveRuntimeGeneration activeGeneration,
                                                                                       const std::size_t maximumOperations) {
        if (!activeGeneration.IsValid() || maximumOperations == 0 || maximumOperations > MaximumSaveLifecycleOperations)
            return Failure<std::unique_ptr<SaveSafePointCoordinator>>(SaveErrors::LifecycleInvalid);
        try {
            auto state = std::make_shared<SharedState>();
            state->ownerThread = std::this_thread::get_id();
            state->activeGeneration = activeGeneration;
            state->maximumOperations = maximumOperations;
            state->operations.reserve(maximumOperations);
            std::unique_ptr<SaveSafePointCoordinator> coordinator(  // NOSONAR - constructor is intentionally private.
                new SaveSafePointCoordinator(std::move(state)));    // NOSONAR - make_unique cannot access a private constructor.
            return Result<std::unique_ptr<SaveSafePointCoordinator>>::Success(std::move(coordinator));
        } catch (const std::bad_alloc &) {
            return Failure<std::unique_ptr<SaveSafePointCoordinator>>(SaveErrors::OperationAllocationFailed);
        }
    }

    SaveSafePointCoordinator::SaveSafePointCoordinator(std::shared_ptr<SaveSafePointDetail::SharedState> state) noexcept
        : state_(std::move(state)) {}

    SaveSafePointCoordinator::~SaveSafePointCoordinator() = default;

    /** @copydoc SaveSafePointCoordinator::Admit */
    Result<void> SaveSafePointCoordinator::Admit(  // NOSONAR(cpp:S5817) - mutates the pointed-to shared lifecycle state.
        const SaveSafePointOperationDescriptor &descriptor) {
        return MutateOnOwner(*state_, [this, &descriptor] {
            if (const Result<void> validation = ValidateAdmission(*state_, descriptor); validation.HasError())
                return validation;
            try {
                SaveSafePointOperationSnapshot snapshot{.descriptor = descriptor};
                if (descriptor.action == SaveSafePointAction::Restore)
                    snapshot.state = SaveSafePointOperationState::DetachedWork;
                state_->operations.push_back(std::move(snapshot));
                return Result<void>::Success();
            } catch (const std::bad_alloc &) {
                return Failure<void>(SaveErrors::OperationAllocationFailed);
            }
        });
    }

    /** @copydoc SaveSafePointCoordinator::PublishWorkerCompletion */
    Result<void> SaveSafePointCoordinator::PublishWorkerCompletion(  // NOSONAR(cpp:S5817) - publishes into shared lifecycle state.
        SaveWorkerCompletion completion) {
        if (const Result<void> validation = ValidateWorkerCompletion(completion); validation.HasError())
            return validation;

        std::lock_guard lock(state_->mutex);
        const auto operation = FindOperation(*state_, completion.operation);
        if (operation == state_->operations.end())
            return Failure<void>(SaveErrors::CompletionInvalid);
        if (operation->descriptor.generation != completion.generation)
            return Failure<void>(SaveErrors::GenerationStale);
        if (operation->state != SaveSafePointOperationState::DetachedWork)
            return Failure<void>(SaveErrors::CompletionInvalid);
        return ApplyWorkerCompletion(*operation, std::move(completion));
    }

    /** @copydoc SaveSafePointCoordinator::CommitAtSafePoint */
    Result<SaveSafePointDrainResult> SaveSafePointCoordinator::CommitAtSafePoint(  // NOSONAR(cpp:S5817) - drains shared lifecycle state.
        const RuntimePhase phase, const SaveRuntimeGeneration generation, const std::size_t maximumOperations,
        ISaveSafePointExecutor &executor) {
        if (const Result<void> owner = ValidateOwner(*state_); owner.HasError())
            return Result<SaveSafePointDrainResult>::Failure(owner.ErrorValue());

        {
            std::lock_guard lock(state_->mutex);
            if (const Result<void> validation = ValidateSafePointRequest(*state_, phase, generation, maximumOperations);
                validation.HasError())
                return Result<SaveSafePointDrainResult>::Failure(validation.ErrorValue());
            state_->safePointActive = true;
        }
        const SafePointScope safePointScope(state_);

        SaveSafePointDrainResult result;
        std::size_t cursor = 0;
        while (result.inspected < maximumOperations) {
            std::optional<SafePointSelection> selected;
            {
                std::lock_guard lock(state_->mutex);
                selected = SelectSafePointOperation(*state_, cursor);
            }
            if (!selected.has_value())
                break;
            ++result.inspected;

            const Result<void> executed = ExecuteContained(executor, selected->descriptor);
            std::lock_guard lock(state_->mutex);
            ApplySafePointOutcome(state_->operations[selected->index], selected->descriptor.action, executed, result);
        }
        return Result<SaveSafePointDrainResult>::Success(result);
    }

    /** @copydoc SaveSafePointCoordinator::TransitionScene */
    Result<void> SaveSafePointCoordinator::TransitionScene(  // NOSONAR(cpp:S5817) - transitions shared lifecycle state.
        const SaveRuntimeGeneration generation) {
        return MutateOnOwner(*state_, [this, generation] {
            return ApplySceneTransition(*state_, generation);
        });
    }

    /** @copydoc SaveSafePointCoordinator::RebindRegistry */
    Result<void> SaveSafePointCoordinator::RebindRegistry(  // NOSONAR(cpp:S5817) - rebinds shared lifecycle state.
        const SaveRuntimeGeneration generation) {
        return MutateOnOwner(*state_, [this, generation] {
            return ApplyRegistryRebind(*state_, generation);
        });
    }

    /** @copydoc SaveSafePointCoordinator::SetSuspended */
    Result<void> SaveSafePointCoordinator::SetSuspended(  // NOSONAR(cpp:S5817) - mutates shared lifecycle state.
        const bool suspended) {
        return MutateOnOwner(*state_, [this, suspended] {
            return ApplySuspension(*state_, suspended);
        });
    }

    /** @copydoc SaveSafePointCoordinator::OnPieStop */
    Result<void> SaveSafePointCoordinator::OnPieStop() {  // NOSONAR(cpp:S5817) - cancels shared lifecycle state.
        return MutateOnOwner(*state_, [this] {
            if (const Result<void> lifecycle = ValidateLifecycleMutation(*state_); lifecycle.HasError())
                return lifecycle;
            state_->stopped = true;
            CancelNonterminal(*state_, SaveLifecycleDisposition::PieStop);
            return Result<void>::Success();
        });
    }

    /** @copydoc SaveSafePointCoordinator::BeginShutdown */
    Result<void> SaveSafePointCoordinator::BeginShutdown() {  // NOSONAR(cpp:S5817) - closes shared lifecycle state.
        return MutateOnOwner(*state_, [this] {
            if (state_->shuttingDown)
                return Result<void>::Success();
            if (state_->safePointActive)
                return Failure<void>(SaveErrors::LifecycleReentrant);
            state_->shuttingDown = true;
            CancelNonterminal(*state_, SaveLifecycleDisposition::HostShutdown);
            return Result<void>::Success();
        });
    }

    /** @copydoc SaveSafePointCoordinator::Snapshot */
    Result<SaveSafePointOperationSnapshot> SaveSafePointCoordinator::Snapshot(const OperationId operation) const {
        std::lock_guard lock(state_->mutex);
        const auto found = FindOperation(*state_, operation);
        if (found == state_->operations.end())
            return Failure<SaveSafePointOperationSnapshot>(SaveErrors::OperationInvalid);
        try {
            return Result<SaveSafePointOperationSnapshot>::Success(*found);
        } catch (const std::bad_alloc &) {
            return Failure<SaveSafePointOperationSnapshot>(SaveErrors::OperationAllocationFailed);
        }
    }

    /** @copydoc SaveSafePointCoordinator::Acknowledge */
    Result<void> SaveSafePointCoordinator::Acknowledge(  // NOSONAR(cpp:S5817) - erases shared lifecycle state.
        const OperationId operation) {
        return MutateOnOwner(*state_, [this, operation] {
            if (state_->safePointActive)
                return Failure<void>(SaveErrors::LifecycleReentrant);
            const auto found = FindOperation(*state_, operation);
            if (found == state_->operations.end())
                return Failure<void>(SaveErrors::OperationInvalid);
            if (!found->IsTerminal())
                return Failure<void>(SaveErrors::CompletionInvalid);
            state_->operations.erase(found);
            return Result<void>::Success();
        });
    }
}  // namespace Horo::Runtime
