#pragma once

#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Extensions/ScriptInvocation.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace Horo::Extensions {
    struct ScriptInvocationSynchronization final {
        [[nodiscard]] std::mutex &Mutex() const noexcept {
            return mutex;
        }

    private:
        mutable std::mutex mutex;
    };

    struct ScriptInvocationProviderState final {
        std::weak_ptr<ScriptInvocationRegistryState> registry;
        std::uint64_t generation{};
        std::size_t maximumInvocations{};
        bool active{true};
        CancellationSource cancellation;
        std::unordered_map<std::uint64_t, std::weak_ptr<ScriptInvocationState>> invocations;

        [[nodiscard]] std::mutex &Mutex() const noexcept {
            return synchronization.Mutex();
        }

        ScriptInvocationSynchronization synchronization;
    };

    struct ScriptInvocationContextState final {
        std::weak_ptr<ScriptInvocationRegistryState> registry;
        ScriptContextId id;
        std::thread::id ownerThread;
        std::size_t maximumInvocations{};
        std::size_t maximumHandles{};
        std::size_t maximumQueuedEvents{};
        std::size_t maximumProgressEventsPerInvocation{};
        ScriptValueLimits valueLimits;
        bool active{true};
        CancellationSource cancellation;
        std::unordered_map<std::uint64_t, std::shared_ptr<ScriptInvocationState>> invocations;
        std::deque<ScriptInvocationEvent> events;
        std::size_t reservedTerminalSlots{};
        std::uint64_t nextHandleValue{1};
        std::uint64_t nextHandleGeneration{1};
        std::vector<ScriptHandle> handles;

        [[nodiscard]] std::mutex &Mutex() const noexcept {
            return synchronization.Mutex();
        }

        ScriptInvocationContextState(std::weak_ptr<ScriptInvocationRegistryState> registryIn, ScriptContextId context,
                                     const ScriptInvocationContextDescriptor &descriptor, std::thread::id owner, std::size_t maximumQueued,
                                     std::size_t maximumProgress, ScriptValueLimits valueLimitsIn);

        ScriptInvocationSynchronization synchronization;
    };

    struct ScriptInvocationExecutionContext final {
        std::weak_ptr<ScriptInvocationRegistryState> registry;
        std::shared_ptr<ScriptInvocationContextState> context;
        std::shared_ptr<ScriptInvocationProviderState> provider;
        ScriptInvocationId id;
        ScriptInvocationRequest request;
        ScriptExportInvocationMode invocationMode{ScriptExportInvocationMode::Count};
        ScriptInvocationCompletionAffinity completionAffinity{ScriptInvocationCompletionAffinity::Count};
        std::optional<std::chrono::steady_clock::time_point> deadline;
        ScriptValueLimits valueLimits;
    };

    struct ScriptInvocationState final {
        std::weak_ptr<ScriptInvocationRegistryState> registry;
        std::shared_ptr<ScriptInvocationContextState> context;
        std::shared_ptr<ScriptInvocationProviderState> provider;
        ScriptInvocationId id;
        ScriptInvocationRequest request;
        ScriptExportInvocationMode invocationMode{ScriptExportInvocationMode::Count};
        ScriptInvocationCompletionAffinity completionAffinity{ScriptInvocationCompletionAffinity::Count};
        ScriptInvocationProgress progress;
        ScriptInvocationStateKind state{ScriptInvocationStateKind::Queued};
        ScriptInvocationCancellationReason cancellationReason{ScriptInvocationCancellationReason::None};
        std::optional<ScriptCallResult> terminalResult;
        std::uint64_t revision{1};
        bool cancellationRequested{};
        bool callerRequested{};
        bool terminalSlotReserved{true};
        std::size_t progressEventsPublished{};
        bool hasDeadline{};
        std::chrono::steady_clock::time_point deadline{};
        ScriptValueLimits valueLimits;
        CancellationSource callerCancellation;

        explicit ScriptInvocationState(ScriptInvocationExecutionContext execution);
    };

    struct ScriptInvocationRegistryState final {
        explicit ScriptInvocationRegistryState(ScriptInvocationRegistryLimits limitsIn) : limits(std::move(limitsIn)) {}

        ScriptInvocationRegistryLimits limits;
        bool shutdown{};
        std::uint64_t nextInvocation{1};
        std::unordered_map<std::uint64_t, std::shared_ptr<ScriptInvocationProviderState>> providers;
        std::unordered_map<std::uint64_t, std::shared_ptr<ScriptInvocationContextState>> contexts;
        std::atomic<std::size_t> activeInvocations{};

        [[nodiscard]] std::mutex &Mutex() const noexcept {
            return synchronization.Mutex();
        }

        ScriptInvocationSynchronization synchronization;
    };

    namespace Detail {
        template <typename T>
        [[nodiscard]] inline Result<T> InvocationFailure(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
            return Result<T>::Failure(MakeError(descriptor, std::move(message)));
        }

        [[nodiscard]] ScriptError InvocationScriptError(const ErrorCodeDescriptor &descriptor, std::string message = {},
                                                        bool cancelled = false);

        [[nodiscard]] std::size_t NormalizeBound(std::size_t value, std::size_t maximum) noexcept;
        [[nodiscard]] bool SameRegistry(const std::weak_ptr<ScriptInvocationRegistryState> &weak,
                                        const std::shared_ptr<ScriptInvocationRegistryState> &registry) noexcept;
        [[nodiscard]] std::uint64_t AllocateScriptContextId() noexcept;

        [[nodiscard]] std::uint64_t &ActiveDrainContext() noexcept;

        [[nodiscard]] ScriptInvocationCancellationReason PendingCancellationLocked(const ScriptInvocationState &state) noexcept;
        [[nodiscard]] bool HasUnreservedEventSlot(const ScriptInvocationContextState &context) noexcept;
        [[nodiscard]] ScriptError CancellationError(ScriptInvocationCancellationReason reason);
        void IncrementRevision(ScriptInvocationState &state) noexcept;
        void ApplyTerminalLocked(ScriptInvocationState &state, ScriptInvocationStateKind stateKind, ScriptCallResult result,
                                 ScriptInvocationCancellationReason cancellationReason);
        void ApplyCancellation(const std::shared_ptr<ScriptInvocationState> &state, ScriptInvocationCancellationReason reason);
        [[nodiscard]] ScriptInvocationCancellationReason ObserveAndMaybeCancel(const std::shared_ptr<ScriptInvocationState> &state);
        [[nodiscard]] Result<void> ValidateProgress(const ScriptInvocationProgress &progress, const ScriptValueLimits &limits);
        [[nodiscard]] Result<void> ValidateRequest(const ScriptInvocationRequest &request, const ScriptValueLimits &limits,
                                                   const ScriptExportFunctionDescriptor *&function);
        [[nodiscard]] Result<std::shared_ptr<ScriptInvocationState>> AdmitInvocation(
            const std::shared_ptr<ScriptInvocationRegistryState> &registry, const std::shared_ptr<ScriptInvocationContextState> &context,
            const std::shared_ptr<ScriptInvocationProviderState> &provider, ScriptInvocationRequest request,
            ScriptExportInvocationMode invocationMode);
        [[nodiscard]] Result<void> ValidateTerminalResult(const ScriptInvocationState &state, const ScriptCallResult &result);
        [[nodiscard]] ScriptInvocationSnapshot SnapshotLocked(const ScriptInvocationState &state);
        void RevokeHandlesForProvider(const std::vector<std::shared_ptr<ScriptInvocationContextState>> &contexts,
                                      std::uint64_t generation) noexcept;
    }  // namespace Detail
}  // namespace Horo::Extensions
