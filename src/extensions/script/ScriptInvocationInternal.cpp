#include "ScriptInvocationInternal.h"

#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <utility>

namespace Horo::Extensions {
    ScriptInvocationContextState::ScriptInvocationContextState(std::weak_ptr<ScriptInvocationRegistryState> registryIn,
                                                               ScriptContextId context, ScriptInvocationContextDescriptor descriptor,
                                                               std::thread::id owner, std::size_t maximumQueued,
                                                               std::size_t maximumProgress, ScriptValueLimits valueLimitsIn)
        : registry(std::move(registryIn)), id(context), ownerThread(owner), maximumInvocations(descriptor.maximumInvocations),
          maximumHandles(descriptor.maximumHandles), maximumQueuedEvents(maximumQueued),
          maximumProgressEventsPerInvocation(maximumProgress), valueLimits(std::move(valueLimitsIn)),
          cancellation(descriptor.parentCancellation) {}

    ScriptInvocationState::ScriptInvocationState(ScriptInvocationExecutionContext execution)
        : registry(std::move(execution.registry)), context(std::move(execution.context)), provider(std::move(execution.provider)),
          id(execution.id), request(std::move(execution.request)), invocationMode(execution.invocationMode),
          completionAffinity(execution.completionAffinity), hasDeadline(execution.deadline.has_value()),
          deadline(execution.deadline.value_or(std::chrono::steady_clock::time_point{})), valueLimits(std::move(execution.valueLimits)),
          callerCancellation(context->cancellation.Token()) {}

    namespace Detail {
        using namespace ExtensionErrors;

        std::atomic<std::uint64_t> NextScriptContextId{1};
        thread_local std::uint64_t ActiveDrainContext{};

        ScriptError InvocationScriptError(const ErrorCodeDescriptor &descriptor, std::string message, bool cancelled) {
            return ScriptError{
                .domain = descriptor.domain.Value(),
                .code = descriptor.code.Value(),
                .message = message.empty() ? std::string{descriptor.summary} : std::move(message),
                .retryable = descriptor.retryable,
                .cancelled = cancelled,
            };
        }

        std::size_t NormalizeBound(std::size_t value, std::size_t maximum) noexcept {
            return std::min(value == 0 ? std::size_t{1} : value, maximum);
        }

        bool SameRegistry(const std::weak_ptr<ScriptInvocationRegistryState> &weak,
                          const std::shared_ptr<ScriptInvocationRegistryState> &registry) noexcept {
            const auto owner = weak.lock();
            return owner != nullptr && owner.get() == registry.get();
        }

        std::uint64_t AllocateScriptContextId() noexcept {
            auto current = NextScriptContextId.load(std::memory_order_relaxed);
            while (current != 0) {
                const auto next = current == std::numeric_limits<std::uint64_t>::max() ? 0 : current + 1;
                if (NextScriptContextId.compare_exchange_weak(current, next, std::memory_order_relaxed))
                    return current;
            }
            return 0;
        }

        ScriptInvocationCancellationReason PendingCancellationLocked(const ScriptInvocationState &state) noexcept {
            if (!state.context->active)
                return ScriptInvocationCancellationReason::Context;
            if (!state.provider->active)
                return ScriptInvocationCancellationReason::Provider;
            if (state.context->cancellation.Token().IsCancellationRequested())
                return ScriptInvocationCancellationReason::Context;
            if (state.callerRequested || state.callerCancellation.Token().IsCancellationRequested())
                return ScriptInvocationCancellationReason::Caller;
            if (state.hasDeadline && std::chrono::steady_clock::now() >= state.deadline)
                return ScriptInvocationCancellationReason::Timeout;
            return ScriptInvocationCancellationReason::None;
        }

        bool HasUnreservedEventSlot(const ScriptInvocationContextState &context) noexcept {
            if (context.events.size() >= context.maximumQueuedEvents)
                return false;
            return context.reservedTerminalSlots < context.maximumQueuedEvents - context.events.size();
        }

        ScriptError CancellationError(ScriptInvocationCancellationReason reason) {
            switch (reason) {
                case ScriptInvocationCancellationReason::Caller:
                    return InvocationScriptError(ScriptInvocationCancelled, {}, true);
                case ScriptInvocationCancellationReason::Context:
                    return InvocationScriptError(ScriptInvocationContextRevoked, {}, true);
                case ScriptInvocationCancellationReason::Provider:
                    return InvocationScriptError(ScriptInvocationProviderRevoked, {}, true);
                case ScriptInvocationCancellationReason::Timeout:
                    return InvocationScriptError(ScriptInvocationTimeout, {}, true);
                case ScriptInvocationCancellationReason::Shutdown:
                    return InvocationScriptError(ScriptInvocationShutdown, {}, true);
                case ScriptInvocationCancellationReason::None:
                    break;
            }
            return InvocationScriptError(ScriptInvocationCancelled, {}, true);
        }

        void IncrementRevision(ScriptInvocationState &state) noexcept {
            if (state.revision != std::numeric_limits<std::uint64_t>::max())
                ++state.revision;
        }

        void ApplyTerminalLocked(ScriptInvocationState &state, ScriptInvocationStateKind stateKind, ScriptCallResult result,
                                 ScriptInvocationCancellationReason cancellationReason) {
            if (state.terminalResult.has_value())
                return;

            state.state = stateKind;
            state.cancellationReason = cancellationReason;
            state.cancellationRequested =
                cancellationReason != ScriptInvocationCancellationReason::None || (result.error.has_value() && result.error->cancelled);
            state.terminalResult = std::move(result);
            IncrementRevision(state);

            auto &context = *state.context;
            auto &provider = *state.provider;
            if (state.terminalSlotReserved) {
                if (context.reservedTerminalSlots > 0)
                    --context.reservedTerminalSlots;
                state.terminalSlotReserved = false;
            }

            const bool deliver = context.active && context.events.size() < context.maximumQueuedEvents;
            if (deliver) {
                context.events.push_back(ScriptInvocationEvent{
                    .kind = ScriptInvocationEventKind::Completed,
                    .invocation = state.id,
                    .context = context.id,
                    .providerGeneration = provider.generation,
                    .progress = state.progress,
                    .terminalResult = state.terminalResult,
                    .revision = state.revision,
                });
            }

            context.invocations.erase(state.id.value);
            provider.invocations.erase(state.id.value);
            if (const auto registry = state.registry.lock(); registry != nullptr)
                registry->activeInvocations.fetch_sub(1);
        }

        void ApplyCancellation(const std::shared_ptr<ScriptInvocationState> &state, ScriptInvocationCancellationReason reason) {
            if (reason == ScriptInvocationCancellationReason::None)
                return;
            state->callerCancellation.RequestCancellation();
            std::scoped_lock lock(state->provider->mutex, state->context->mutex);
            if (state->terminalResult.has_value())
                return;
            ApplyTerminalLocked(*state, ScriptInvocationStateKind::Cancelled, ScriptCallResult::Failure(CancellationError(reason)), reason);
        }

        ScriptInvocationCancellationReason ObserveAndMaybeCancel(const std::shared_ptr<ScriptInvocationState> &state) {
            ScriptInvocationCancellationReason reason = ScriptInvocationCancellationReason::None;
            {
                std::scoped_lock lock(state->provider->mutex, state->context->mutex);
                if (!state->terminalResult.has_value())
                    reason = PendingCancellationLocked(*state);
            }
            if (reason != ScriptInvocationCancellationReason::None)
                ApplyCancellation(state, reason);
            return reason;
        }

        Result<void> ValidateProgress(const ScriptInvocationProgress &progress, const ScriptValueLimits &limits) {
            if (progress.phase.empty() || progress.phase.size() > limits.maximumTypeIdentityBytes ||
                !IsValidUtf8ScalarSequence(progress.phase))
                return InvocationFailure<void>(ScriptInvocationInvalid, "Script invocation phase identity is malformed.");
            if (progress.totalUnits == 0 || progress.completedUnits > progress.totalUnits)
                return InvocationFailure<void>(ScriptInvocationInvalid, "Script invocation progress counters are malformed.");
            return Result<void>::Success();
        }

        Result<void> ValidateRequest(const ScriptInvocationRequest &request, const ScriptValueLimits &limits,
                                     const ScriptExportFunctionDescriptor *&function) {
            if (request.descriptors == nullptr || request.apiId.empty() || request.functionId.empty())
                return InvocationFailure<void>(ScriptInvocationInvalid, "Script invocation target is incomplete.");
            const auto *descriptor = request.descriptors->Find(request.apiId);
            if (descriptor == nullptr)
                return InvocationFailure<void>(ScriptInvocationInvalid, "Script invocation API identity is not declared.");
            function = nullptr;
            for (const auto &candidate : descriptor->functions) {
                if (candidate.id == request.functionId) {
                    function = &candidate;
                    break;
                }
            }
            if (function == nullptr)
                return InvocationFailure<void>(ScriptInvocationInvalid, "Script invocation function identity is not declared.");
            if (request.completionAffinity != ScriptInvocationCompletionAffinity::OwnerThreadSafePoint)
                return InvocationFailure<void>(ScriptInvocationInvalid, "Script invocation completion affinity is unsupported.");
            return ValidateScriptArguments(*function, request.arguments, *descriptor, limits);
        }

        Result<std::shared_ptr<ScriptInvocationState>> AdmitInvocation(const std::shared_ptr<ScriptInvocationRegistryState> &registry,
                                                                       const std::shared_ptr<ScriptInvocationContextState> &context,
                                                                       const std::shared_ptr<ScriptInvocationProviderState> &provider,
                                                                       ScriptInvocationRequest request,
                                                                       ScriptExportInvocationMode invocationMode) {
            std::shared_ptr<ScriptInvocationState> invocation;
            {
                std::lock_guard registryLock(registry->mutex);
                if (registry->shutdown)
                    return InvocationFailure<std::shared_ptr<ScriptInvocationState>>(ScriptInvocationShutdown);
                if (registry->activeInvocations.load() >= registry->limits.maximumInvocations)
                    return InvocationFailure<std::shared_ptr<ScriptInvocationState>>(ScriptInvocationCapacityExceeded);
                std::scoped_lock lock(provider->mutex, context->mutex);
                if (!provider->active || !context->active)
                    return InvocationFailure<std::shared_ptr<ScriptInvocationState>>(ScriptInvocationUnavailable);
                if (context->invocations.size() >= context->maximumInvocations ||
                    provider->invocations.size() >= provider->maximumInvocations)
                    return InvocationFailure<std::shared_ptr<ScriptInvocationState>>(ScriptInvocationCapacityExceeded);
                if (!HasUnreservedEventSlot(*context))
                    return InvocationFailure<
                        std::shared_ptr<ScriptInvocationState>>(ScriptInvocationCapacityExceeded,
                                                                "Script invocation terminal delivery capacity is full.");
                if (registry->nextInvocation == 0)
                    return InvocationFailure<std::shared_ptr<ScriptInvocationState>>(ScriptInvocationCapacityExceeded,
                                                                                     "Script invocation identity exhausted.");

                const ScriptInvocationId id{registry->nextInvocation++};
                std::optional<std::chrono::steady_clock::time_point> deadline;
                if (request.timeout.has_value())
                    deadline = std::chrono::steady_clock::now() + *request.timeout;
                ScriptInvocationExecutionContext execution{
                    .registry = registry,
                    .context = context,
                    .provider = provider,
                    .id = id,
                    .request = std::move(request),
                    .invocationMode = invocationMode,
                    .completionAffinity = ScriptInvocationCompletionAffinity::OwnerThreadSafePoint,
                    .deadline = deadline,
                    .valueLimits = registry->limits.value,
                };
                invocation = std::make_shared<ScriptInvocationState>(std::move(execution));
                context->invocations.emplace(id.value, invocation);
                provider->invocations.emplace(id.value, invocation);
                ++context->reservedTerminalSlots;
                registry->activeInvocations.fetch_add(1);
            }
            return Result<std::shared_ptr<ScriptInvocationState>>::Success(std::move(invocation));
        }

        Result<void> ValidateTerminalResult(const ScriptInvocationState &state, const ScriptCallResult &result) {
            const auto *descriptor = state.request.descriptors == nullptr ? nullptr : state.request.descriptors->Find(state.request.apiId);
            if (descriptor == nullptr)
                return InvocationFailure<void>(ScriptInvocationInvalid, "Script invocation descriptor generation was released.");
            const ScriptExportFunctionDescriptor *function = nullptr;
            for (const auto &candidate : descriptor->functions) {
                if (candidate.id == state.request.functionId) {
                    function = &candidate;
                    break;
                }
            }
            if (function == nullptr)
                return InvocationFailure<void>(ScriptInvocationInvalid, "Script invocation function declaration was released.");
            return ValidateScriptResult(*function, result, *descriptor, state.valueLimits);
        }

        ScriptInvocationSnapshot SnapshotLocked(const ScriptInvocationState &state) {
            return ScriptInvocationSnapshot{
                .invocation = state.id,
                .context = state.context->id,
                .providerGeneration = state.provider->generation,
                .apiId = state.request.apiId,
                .functionId = state.request.functionId,
                .invocationMode = state.invocationMode,
                .completionAffinity = state.completionAffinity,
                .state = state.state,
                .progress = state.progress,
                .cancellationReason = state.cancellationReason,
                .cancellationRequested = state.cancellationRequested,
                .terminalResult = state.terminalResult,
                .revision = state.revision,
            };
        }

        void RevokeHandlesForProvider(const std::vector<std::shared_ptr<ScriptInvocationContextState>> &contexts,
                                      std::uint64_t generation) noexcept {
            for (const auto &context : contexts) {
                std::lock_guard lock(context->mutex);
                context->handles.erase(std::remove_if(context->handles.begin(), context->handles.end(),
                                                      [generation](const ScriptHandle &handle) {
                    return handle.providerGeneration == generation;
                }),
                                       context->handles.end());
            }
        }
    }  // namespace Detail
}  // namespace Horo::Extensions
