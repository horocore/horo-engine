#pragma once

#include "BackendOperationValidation.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <utility>

namespace Horo::Extensions {
    struct BackendOperationProviderState final {
        BackendOperationProviderDescriptor descriptor;
        std::atomic_bool registered{true};
        std::atomic_size_t activeOperations{};
    };

    class BackendOperationStateSynchronization final {
    public:
        [[nodiscard]] std::mutex &Mutex() const noexcept {
            return mutex_;
        }

    private:
        mutable std::mutex mutex_;
    };

    struct BackendOperationStateData final {
        BackendOperationStateSynchronization synchronization;
        BackendOperationSnapshot snapshot;
        std::weak_ptr<BackendOperationRegistryState> registry;
        std::shared_ptr<BackendOperationProviderState> provider;
        CancellationToken parentCancellation;
        CancellationSource cancellation;
        Error cancellationError;
        Error abandonmentError;
        std::size_t retainedDiagnosticBytes{};
        bool terminal{};

        BackendOperationStateData(std::weak_ptr<BackendOperationRegistryState> registryState,
                                  std::shared_ptr<BackendOperationProviderState> providerState, BackendOperationSnapshot initialSnapshot,
                                  CancellationToken parent)
            : snapshot(std::move(initialSnapshot)), registry(std::move(registryState)), provider(std::move(providerState)),
              parentCancellation(parent), cancellation(parent) {}
    };

    struct BackendOperationRegistryState final {
        std::mutex mutex;
        BackendOperationRegistryConfig config;
        std::vector<std::shared_ptr<BackendOperationProviderState>> providers;
        std::vector<std::shared_ptr<BackendOperationStateData>> operations;
        std::uint64_t nextOperationId{1};
        bool shutdown{};
    };

    namespace BackendOperationRegistryDetail {
        [[nodiscard]] const BackendOperationTypeDescriptor *FindType(const BackendOperationProviderState &provider,
                                                                     const BackendOperationTypeId &type) noexcept {
            const auto found = std::ranges::find(provider.descriptor.operationTypes, type, &BackendOperationTypeDescriptor::type);
            return found == provider.descriptor.operationTypes.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] bool IsTerminal(const BackendOperationStateData &state) noexcept {
            return state.terminal;
        }

        void RetireOperation(const std::shared_ptr<BackendOperationStateData> &operation) noexcept {
            if (const auto registry = operation->registry.lock()) {
                std::scoped_lock lock{registry->mutex};
                const auto removed = std::erase(registry->operations, operation);
                if (removed != 0U)
                    operation->provider->activeOperations.fetch_sub(1U, std::memory_order_relaxed);
            }
        }

        void SetCancellationRequested(BackendOperationStateData &state, const BackendOperationCancellationReason reason) noexcept {
            if (!state.snapshot.cancellationRequested) {
                state.snapshot.cancellationRequested = true;
                state.snapshot.cancellationReason = reason;
                ++state.snapshot.revision;
            }
            state.cancellation.RequestCancellation();
        }

        [[nodiscard]] std::optional<BackendOperationCancellationReason> PendingCancellation(
            const BackendOperationStateData &state) noexcept {
            if (state.snapshot.cancellationRequested)
                return state.snapshot.cancellationReason;
            if (state.parentCancellation.IsCancellationRequested())
                return BackendOperationCancellationReason::Parent;
            return std::nullopt;
        }

        void TerminalizeLocked(BackendOperationStateData &state, const BackendOperationState terminalState, std::optional<Error> error,
                               const BackendOperationCancellationReason reason = BackendOperationCancellationReason::None) noexcept {
            if (state.terminal)
                return;
            state.snapshot.state = terminalState;
            state.snapshot.terminalError = std::move(error);
            state.snapshot.cancellationReason = reason;
            state.snapshot.cancellationRequested =
                reason != BackendOperationCancellationReason::None || state.snapshot.cancellationRequested;
            ++state.snapshot.revision;
            state.terminal = true;
        }

        [[nodiscard]] bool TerminalizePendingCancellationLocked(BackendOperationStateData &state) noexcept {
            const auto reason = PendingCancellation(state);
            if (!reason.has_value())
                return false;
            TerminalizeLocked(state, BackendOperationState::Cancelled, std::move(state.cancellationError), *reason);
            return true;
        }

        template <typename Finalize>
        [[nodiscard]] BackendOperationTransitionResult ApplyTerminalTransition(const std::shared_ptr<BackendOperationStateData> &operation,
                                                                               Finalize &&finalize) {
            using enum BackendOperationTransitionResult;
            if (!operation)
                return AlreadyTerminal;
            BackendOperationTransitionResult transition = InvalidTransition;
            {
                std::scoped_lock lock{operation->synchronization.Mutex()};
                if (IsTerminal(*operation))
                    return AlreadyTerminal;
                transition =
                    TerminalizePendingCancellationLocked(*operation) ? CancellationWon : std::forward<Finalize>(finalize)(*operation);
            }
            if (transition == Applied || transition == CancellationWon)
                RetireOperation(operation);
            return transition;
        }

        [[nodiscard]] BackendOperationCancellationObservation CancelIfRequested(
            const std::shared_ptr<BackendOperationStateData> &operation) noexcept {
            if (!operation)
                return BackendOperationCancellationObservation::AlreadyTerminal;
            bool cancelled = false;
            {
                std::scoped_lock lock{operation->synchronization.Mutex()};
                if (IsTerminal(*operation))
                    return BackendOperationCancellationObservation::AlreadyTerminal;
                if (const auto reason = PendingCancellation(*operation); reason.has_value()) {
                    TerminalizeLocked(*operation, BackendOperationState::Cancelled, std::move(operation->cancellationError), *reason);
                    cancelled = true;
                }
            }
            if (cancelled)
                RetireOperation(operation);
            return cancelled ? BackendOperationCancellationObservation::Cancelled : BackendOperationCancellationObservation::NotRequested;
        }

        void ForceCancel(const std::shared_ptr<BackendOperationStateData> &operation,
                         const BackendOperationCancellationReason reason) noexcept {
            if (!operation)
                return;
            bool terminalized = false;
            {
                std::scoped_lock lock{operation->synchronization.Mutex()};
                if (!IsTerminal(*operation)) {
                    // Cancellation source selection and terminalization share this mutex. The
                    // winning request therefore remains observable even when teardown races a
                    // caller or parent cancellation request.
                    const auto winningReason = PendingCancellation(*operation).value_or(reason);
                    SetCancellationRequested(*operation, winningReason);
                    TerminalizeLocked(*operation, BackendOperationState::Cancelled, std::move(operation->cancellationError), winningReason);
                    terminalized = true;
                }
            }
            if (terminalized)
                RetireOperation(operation);
        }

        [[nodiscard]] std::optional<BackendOperationId> AllocateOperationId(BackendOperationRegistryState &registry) noexcept {
            if (registry.nextOperationId == 0)
                return std::nullopt;
            const BackendOperationId id{registry.nextOperationId};
            ++registry.nextOperationId;
            return id;
        }
    }  // namespace BackendOperationRegistryDetail
}  // namespace Horo::Extensions
