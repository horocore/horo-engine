#include "ScriptInvocationInternal.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <utility>

namespace Horo::Extensions {
    using namespace Detail;
    using namespace ExtensionErrors;

    /** @copydoc ScriptInvocationController::PublishProgress */
    Result<ScriptInvocationProgressDisposition> ScriptInvocationController::PublishProgress(ScriptInvocationProgress progress) const {
        if (state_ == nullptr)
            return InvocationFailure<ScriptInvocationProgressDisposition>(ScriptInvocationInvalid, "Moved-from invocation controller.");
        auto valid = ValidateProgress(progress, state_->valueLimits);
        if (valid.HasError())
            return Result<ScriptInvocationProgressDisposition>::Failure(valid.ErrorValue());

        ScriptInvocationCancellationReason reason = ScriptInvocationCancellationReason::None;
        {
            std::scoped_lock lock(state_->provider->mutex, state_->context->mutex);
            if (state_->terminalResult.has_value())
                return Result<ScriptInvocationProgressDisposition>::Success(ScriptInvocationProgressDisposition::AlreadyTerminal);
            reason = PendingCancellationLocked(*state_);
            if (reason == ScriptInvocationCancellationReason::None) {
                if (progress.phase == state_->progress.phase && progress.completedUnits < state_->progress.completedUnits)
                    return InvocationFailure<ScriptInvocationProgressDisposition>(ScriptInvocationInvalid,
                                                                                  "Script invocation progress is not monotonic.");
                state_->progress = std::move(progress);
                state_->state = ScriptInvocationStateKind::Running;
                IncrementRevision(*state_);
                const bool canPublish = HasUnreservedEventSlot(*state_->context) &&
                                        state_->progressEventsPublished < state_->context->maximumProgressEventsPerInvocation;
                if (canPublish) {
                    ++state_->progressEventsPublished;
                    state_->context->events.push_back(ScriptInvocationEvent{
                        .kind = ScriptInvocationEventKind::Progress,
                        .invocation = state_->id,
                        .context = state_->context->id,
                        .providerGeneration = state_->provider->generation,
                        .progress = state_->progress,
                        .revision = state_->revision,
                    });
                    return Result<ScriptInvocationProgressDisposition>::Success(ScriptInvocationProgressDisposition::Published);
                }
                return Result<ScriptInvocationProgressDisposition>::Success(ScriptInvocationProgressDisposition::Coalesced);
            }
        }
        ApplyCancellation(state_, reason);
        return Result<ScriptInvocationProgressDisposition>::Success(ScriptInvocationProgressDisposition::CancellationWon);
    }

    /** @copydoc ScriptInvocationController::ObserveCancellation */
    Result<ScriptInvocationCancellationObservation> ScriptInvocationController::ObserveCancellation() const {
        if (state_ == nullptr)
            return InvocationFailure<ScriptInvocationCancellationObservation>(ScriptInvocationInvalid, "Moved-from invocation controller.");
        const auto reason = ObserveAndMaybeCancel(state_);
        std::scoped_lock lock(state_->provider->mutex, state_->context->mutex);
        if (reason != ScriptInvocationCancellationReason::None || state_->state == ScriptInvocationStateKind::Cancelled)
            return Result<ScriptInvocationCancellationObservation>::Success(ScriptInvocationCancellationObservation::Cancelled);
        if (state_->terminalResult.has_value())
            return Result<ScriptInvocationCancellationObservation>::Success(ScriptInvocationCancellationObservation::AlreadyTerminal);
        return Result<ScriptInvocationCancellationObservation>::Success(ScriptInvocationCancellationObservation::NotRequested);
    }

    /** @copydoc ScriptInvocationController::Complete */
    Result<ScriptInvocationTransitionResult> ScriptInvocationController::Complete(ScriptCallResult result) const {
        if (state_ == nullptr)
            return InvocationFailure<ScriptInvocationTransitionResult>(ScriptInvocationInvalid, "Moved-from invocation controller.");
        if (result.IsFailure())
            return InvocationFailure<ScriptInvocationTransitionResult>(ScriptCallResultInvalid,
                                                                       "A successful completion cannot carry a failure result.");
        auto valid = ValidateTerminalResult(*state_, result);
        if (valid.HasError())
            return Result<ScriptInvocationTransitionResult>::Failure(valid.ErrorValue());
        ScriptInvocationCancellationReason reason = ScriptInvocationCancellationReason::None;
        {
            std::scoped_lock lock(state_->provider->mutex, state_->context->mutex);
            if (state_->terminalResult.has_value())
                return Result<ScriptInvocationTransitionResult>::Success(ScriptInvocationTransitionResult::AlreadyTerminal);
            reason = PendingCancellationLocked(*state_);
            if (reason == ScriptInvocationCancellationReason::None) {
                ApplyTerminalLocked(*state_, ScriptInvocationStateKind::Completed, std::move(result),
                                    ScriptInvocationCancellationReason::None);
                return Result<ScriptInvocationTransitionResult>::Success(ScriptInvocationTransitionResult::Applied);
            }
        }
        ApplyCancellation(state_, reason);
        return Result<ScriptInvocationTransitionResult>::Success(ScriptInvocationTransitionResult::CancellationWon);
    }

    /** @copydoc ScriptInvocationController::Fail */
    Result<ScriptInvocationTransitionResult> ScriptInvocationController::Fail(ScriptError error) const {
        if (state_ == nullptr)
            return InvocationFailure<ScriptInvocationTransitionResult>(ScriptInvocationInvalid, "Moved-from invocation controller.");
        auto valid = ValidateScriptError(error, state_->valueLimits);
        if (valid.HasError())
            return Result<ScriptInvocationTransitionResult>::Failure(valid.ErrorValue());
        const bool errorCancelled = error.cancelled;
        ScriptInvocationCancellationReason reason = ScriptInvocationCancellationReason::None;
        {
            std::scoped_lock lock(state_->provider->mutex, state_->context->mutex);
            if (state_->terminalResult.has_value())
                return Result<ScriptInvocationTransitionResult>::Success(ScriptInvocationTransitionResult::AlreadyTerminal);
            reason = PendingCancellationLocked(*state_);
            if (reason == ScriptInvocationCancellationReason::None) {
                const auto stateKind = errorCancelled ? ScriptInvocationStateKind::Cancelled : ScriptInvocationStateKind::Failed;
                ApplyTerminalLocked(*state_, stateKind, ScriptCallResult::Failure(std::move(error)),
                                    errorCancelled ? ScriptInvocationCancellationReason::Caller : ScriptInvocationCancellationReason::None);
                return Result<ScriptInvocationTransitionResult>::Success(ScriptInvocationTransitionResult::Applied);
            }
        }
        ApplyCancellation(state_, reason);
        return Result<ScriptInvocationTransitionResult>::Success(ScriptInvocationTransitionResult::CancellationWon);
    }

    /** @copydoc ScriptInvocationController::Fail */
    Result<ScriptInvocationTransitionResult> ScriptInvocationController::Fail(const Error &error, bool retryable) const {
        if (state_ == nullptr)
            return InvocationFailure<ScriptInvocationTransitionResult>(ScriptInvocationInvalid, "Moved-from invocation controller.");
        auto converted = MakeScriptError(error, retryable, state_->valueLimits);
        if (converted.HasError())
            return Result<ScriptInvocationTransitionResult>::Failure(converted.ErrorValue());
        return Fail(std::move(converted).Value());
    }

    void ScriptInvocationController::Abandon() noexcept {
        if (state_ == nullptr)
            return;
        (void)ObserveAndMaybeCancel(state_);
        {
            std::scoped_lock lock(state_->provider->mutex, state_->context->mutex);
            if (!state_->terminalResult.has_value())
                ApplyTerminalLocked(*state_, ScriptInvocationStateKind::Failed,
                                    ScriptCallResult::Failure(InvocationScriptError(ScriptInvocationAbandoned)),
                                    ScriptInvocationCancellationReason::None);
        }
        state_.reset();
    }

    ScriptInvocationRegistry::ScriptInvocationRegistry(ScriptInvocationRegistryLimits limits) {
        limits.maximumContexts = NormalizeBound(limits.maximumContexts, MaximumContexts);
        limits.maximumProviders = NormalizeBound(limits.maximumProviders, MaximumProviders);
        limits.maximumInvocations = NormalizeBound(limits.maximumInvocations, MaximumInvocations);
        limits.maximumQueuedEvents = std::max<std::size_t>(1, limits.maximumQueuedEvents);
        limits.maximumHandlesPerContext = std::max<std::size_t>(1, limits.maximumHandlesPerContext);
        limits.maximumProgressEventsPerInvocation = std::min(limits.maximumProgressEventsPerInvocation, limits.maximumQueuedEvents);
        if (limits.maximumTimeout < std::chrono::milliseconds::zero())
            limits.maximumTimeout = std::chrono::milliseconds::zero();
        state_ = std::make_shared<ScriptInvocationRegistryState>(std::move(limits));
    }

    /** @copydoc ScriptInvocationRegistry::~ScriptInvocationRegistry */
    ScriptInvocationRegistry::~ScriptInvocationRegistry() noexcept {
        BeginShutdown();
    }

    /** @copydoc ScriptInvocationRegistry::RegisterProvider */
    Result<ScriptInvocationProviderRegistration> ScriptInvocationRegistry::RegisterProvider(ScriptInvocationProviderDescriptor descriptor) {
        if (descriptor.generation == 0)
            return InvocationFailure<ScriptInvocationProviderRegistration>(ScriptInvocationInvalid,
                                                                           "Script provider generation is invalid.");
        if (state_ == nullptr)
            return InvocationFailure<ScriptInvocationProviderRegistration>(ScriptInvocationShutdown);
        std::lock_guard lock(state_->mutex);
        if (state_->shutdown)
            return InvocationFailure<ScriptInvocationProviderRegistration>(ScriptInvocationShutdown);
        if (state_->providers.contains(descriptor.generation))
            return InvocationFailure<ScriptInvocationProviderRegistration>(ScriptInvocationProviderDuplicate);
        if (state_->providers.size() >= state_->limits.maximumProviders)
            return InvocationFailure<ScriptInvocationProviderRegistration>(ScriptInvocationCapacityExceeded);
        descriptor.maximumInvocations = NormalizeBound(descriptor.maximumInvocations, state_->limits.maximumInvocations);
        auto provider = std::make_shared<ScriptInvocationProviderState>();
        provider->registry = state_;
        provider->generation = descriptor.generation;
        provider->maximumInvocations = descriptor.maximumInvocations;
        state_->providers.emplace(descriptor.generation, provider);
        return Result<ScriptInvocationProviderRegistration>::Success(ScriptInvocationProviderRegistration{state_, std::move(provider)});
    }

    /** @copydoc ScriptInvocationRegistry::RegisterContext */
    Result<ScriptInvocationContextRegistration> ScriptInvocationRegistry::RegisterContext(ScriptInvocationContextDescriptor descriptor) {
        if (state_ == nullptr)
            return InvocationFailure<ScriptInvocationContextRegistration>(ScriptInvocationShutdown);
        std::lock_guard lock(state_->mutex);
        if (state_->shutdown)
            return InvocationFailure<ScriptInvocationContextRegistration>(ScriptInvocationShutdown);
        if (state_->contexts.size() >= state_->limits.maximumContexts)
            return InvocationFailure<ScriptInvocationContextRegistration>(ScriptInvocationCapacityExceeded);
        if (descriptor.maximumInvocations == 0 || descriptor.maximumHandles == 0)
            return InvocationFailure<ScriptInvocationContextRegistration>(ScriptInvocationInvalid,
                                                                          "Script context bounds must be non-zero.");
        const auto contextValue = AllocateScriptContextId();
        if (contextValue == 0)
            return InvocationFailure<ScriptInvocationContextRegistration>(ScriptInvocationCapacityExceeded,
                                                                          "Script context identity exhausted.");
        const ScriptContextId id{contextValue};
        descriptor.maximumInvocations = NormalizeBound(descriptor.maximumInvocations, state_->limits.maximumInvocations);
        descriptor.maximumHandles = NormalizeBound(descriptor.maximumHandles, state_->limits.maximumHandlesPerContext);
        auto context =
            std::make_shared<ScriptInvocationContextState>(state_, id, std::move(descriptor), std::this_thread::get_id(),
                                                           state_->limits.maximumQueuedEvents,
                                                           state_->limits.maximumProgressEventsPerInvocation, state_->limits.value);
        state_->contexts.emplace(id.value, context);
        return Result<ScriptInvocationContextRegistration>::Success(ScriptInvocationContextRegistration{state_, std::move(context)});
    }

    /** @copydoc ScriptInvocationRegistry::Begin */
    Result<ScriptInvocationController> ScriptInvocationRegistry::Begin(const ScriptInvocationContextRegistration &contextRegistration,
                                                                       const ScriptInvocationProviderRegistration &providerRegistration,
                                                                       ScriptInvocationRequest request) {
        if (state_ == nullptr)
            return InvocationFailure<ScriptInvocationController>(ScriptInvocationShutdown);
        const auto context = contextRegistration.context_;
        const auto provider = providerRegistration.provider_;
        if (context == nullptr || provider == nullptr || !SameRegistry(context->registry, state_) ||
            !SameRegistry(provider->registry, state_))
            return InvocationFailure<ScriptInvocationController>(ScriptInvocationUnavailable,
                                                                 "Script context or provider belongs to another registry.");
        if (ActiveDrainContext == context->id.value)
            return InvocationFailure<ScriptInvocationController>(ScriptInvocationReentrant);
        {
            std::lock_guard contextLock(context->mutex);
            if (context->ownerThread != std::this_thread::get_id())
                return InvocationFailure<ScriptInvocationController>(ScriptInvocationThreadViolation);
            if (!context->active)
                return InvocationFailure<ScriptInvocationController>(ScriptInvocationContextRevoked);
            if (context->cancellation.Token().IsCancellationRequested())
                return InvocationFailure<ScriptInvocationController>(ScriptInvocationCancelled);
        }
        {
            std::lock_guard providerLock(provider->mutex);
            if (!provider->active)
                return InvocationFailure<ScriptInvocationController>(ScriptInvocationProviderRevoked);
        }

        const ScriptExportFunctionDescriptor *function = nullptr;
        auto requestValidation = ValidateRequest(request, state_->limits.value, function);
        if (requestValidation.HasError())
            return Result<ScriptInvocationController>::Failure(requestValidation.ErrorValue());
        if (request.timeout.has_value() &&
            (*request.timeout < std::chrono::milliseconds::zero() || *request.timeout > state_->limits.maximumTimeout))
            return InvocationFailure<ScriptInvocationController>(ScriptInvocationInvalid,
                                                                 "Script invocation timeout is outside host bounds.");

        auto admitted = AdmitInvocation(state_, context, provider, std::move(request), function->invocation);
        if (admitted.HasError())
            return Result<ScriptInvocationController>::Failure(admitted.ErrorValue());
        auto invocation = std::move(admitted).Value();
        if (invocation->hasDeadline && std::chrono::steady_clock::now() >= invocation->deadline)
            ApplyCancellation(invocation, ScriptInvocationCancellationReason::Timeout);
        return Result<ScriptInvocationController>::Success(ScriptInvocationController{std::move(invocation)});
    }

    /** @copydoc ScriptInvocationRegistry::Drain */
    Result<std::vector<ScriptInvocationEvent>> ScriptInvocationRegistry::Drain(
        const ScriptInvocationContextRegistration &contextRegistration, std::size_t maximumEvents) {
        if (state_ == nullptr)
            return InvocationFailure<std::vector<ScriptInvocationEvent>>(ScriptInvocationShutdown);
        const auto context = contextRegistration.context_;
        if (context == nullptr || !SameRegistry(context->registry, state_))
            return InvocationFailure<std::vector<ScriptInvocationEvent>>(ScriptInvocationUnavailable);
        {
            std::lock_guard lock(context->mutex);
            if (context->ownerThread != std::this_thread::get_id())
                return InvocationFailure<std::vector<ScriptInvocationEvent>>(ScriptInvocationThreadViolation);
            if (!context->active)
                return InvocationFailure<std::vector<ScriptInvocationEvent>>(ScriptInvocationContextRevoked);
            if (ActiveDrainContext == context->id.value)
                return InvocationFailure<std::vector<ScriptInvocationEvent>>(ScriptInvocationReentrant);
        }
        ActiveDrainContext = context->id.value;

        struct DrainReset final {
            ~DrainReset() {
                ActiveDrainContext = 0;
            }
        } reset;

        std::vector<std::shared_ptr<ScriptInvocationState>> active;
        {
            std::lock_guard lock(context->mutex);
            active.reserve(context->invocations.size());
            for (const auto &[id, invocation] : context->invocations)
                active.push_back(invocation);
        }
        for (const auto &invocation : active)
            (void)ObserveAndMaybeCancel(invocation);

        std::vector<ScriptInvocationEvent> events;
        {
            std::lock_guard lock(context->mutex);
            const auto count = std::min(maximumEvents, context->events.size());
            events.reserve(count);
            for (std::size_t index = 0; index < count; ++index) {
                events.emplace_back(std::move(context->events.front()));
                context->events.pop_front();
            }
        }
        return Result<std::vector<ScriptInvocationEvent>>::Success(std::move(events));
    }

    /** @copydoc ScriptInvocationRegistry::IssueHandle */
    Result<ScriptHandle> ScriptInvocationRegistry::IssueHandle(const ScriptInvocationContextRegistration &contextRegistration,
                                                               const ScriptInvocationProviderRegistration &providerRegistration,
                                                               std::string type) {
        if (state_ == nullptr)
            return InvocationFailure<ScriptHandle>(ScriptInvocationShutdown);
        const auto context = contextRegistration.context_;
        const auto provider = providerRegistration.provider_;
        if (context == nullptr || provider == nullptr || !SameRegistry(context->registry, state_) ||
            !SameRegistry(provider->registry, state_))
            return InvocationFailure<ScriptHandle>(ScriptInvocationUnavailable);
        if (type.empty() || type.size() > state_->limits.value.maximumTypeIdentityBytes)
            return InvocationFailure<ScriptHandle>(ScriptHandleInvalid, "Script handle type identity is malformed.");
        if (context->ownerThread != std::this_thread::get_id())
            return InvocationFailure<ScriptHandle>(ScriptInvocationThreadViolation);
        std::scoped_lock lock(provider->mutex, context->mutex);
        if (!provider->active)
            return InvocationFailure<ScriptHandle>(ScriptHandleRevoked);
        if (!context->active)
            return InvocationFailure<ScriptHandle>(ScriptHandleRevoked);
        if (context->handles.size() >= context->maximumHandles)
            return InvocationFailure<ScriptHandle>(ScriptInvocationCapacityExceeded);
        if (context->nextHandleValue == 0 || context->nextHandleGeneration == 0)
            return InvocationFailure<ScriptHandle>(ScriptInvocationCapacityExceeded, "Script handle identity exhausted.");
        ScriptHandle handle{
            .context = context->id,
            .providerGeneration = provider->generation,
            .value = context->nextHandleValue++,
            .generation = context->nextHandleGeneration++,
            .type = std::move(type),
        };
        context->handles.push_back(handle);
        return Result<ScriptHandle>::Success(std::move(handle));
    }

    /** @copydoc ScriptInvocationRegistry::ReleaseHandle */
    Result<void> ScriptInvocationRegistry::ReleaseHandle(const ScriptHandle &handle) {
        if (state_ == nullptr)
            return InvocationFailure<void>(ScriptInvocationShutdown);
        if (!handle.IsValid())
            return InvocationFailure<void>(ScriptHandleInvalid);
        std::shared_ptr<ScriptInvocationContextState> context;
        std::shared_ptr<ScriptInvocationProviderState> provider;
        {
            std::lock_guard lock(state_->mutex);
            const auto contextFound = state_->contexts.find(handle.context.value);
            const auto providerFound = state_->providers.find(handle.providerGeneration);
            if (contextFound == state_->contexts.end() || providerFound == state_->providers.end())
                return InvocationFailure<void>(ScriptHandleRevoked);
            context = contextFound->second;
            provider = providerFound->second;
        }
        std::scoped_lock lock(provider->mutex, context->mutex);
        if (!provider->active || !context->active)
            return InvocationFailure<void>(ScriptHandleRevoked);
        const auto found = std::find(context->handles.begin(), context->handles.end(), handle);
        if (found == context->handles.end())
            return InvocationFailure<void>(ScriptHandleRevoked);
        context->handles.erase(found);
        return Result<void>::Success();
    }

    /** @copydoc ScriptInvocationRegistry::ValidateHandle */
    Result<void> ScriptInvocationRegistry::ValidateHandle(const ScriptHandle &handle) const {
        if (state_ == nullptr)
            return InvocationFailure<void>(ScriptInvocationShutdown);
        if (!handle.IsValid())
            return InvocationFailure<void>(ScriptHandleInvalid);
        std::shared_ptr<ScriptInvocationContextState> context;
        std::shared_ptr<ScriptInvocationProviderState> provider;
        {
            std::lock_guard lock(state_->mutex);
            const auto contextFound = state_->contexts.find(handle.context.value);
            const auto providerFound = state_->providers.find(handle.providerGeneration);
            if (contextFound == state_->contexts.end() || providerFound == state_->providers.end())
                return InvocationFailure<void>(ScriptHandleRevoked);
            context = contextFound->second;
            provider = providerFound->second;
        }
        std::scoped_lock lock(provider->mutex, context->mutex);
        if (!provider->active || !context->active)
            return InvocationFailure<void>(ScriptHandleRevoked);
        return std::find(context->handles.begin(), context->handles.end(), handle) == context->handles.end()
                   ? InvocationFailure<void>(ScriptHandleRevoked)
                   : Result<void>::Success();
    }

    void ScriptInvocationRegistry::ResetProviderState(const std::shared_ptr<ScriptInvocationRegistryState> &registry,
                                                      const std::shared_ptr<ScriptInvocationProviderState> &provider,
                                                      ScriptInvocationCancellationReason reason) noexcept {
        if (provider == nullptr)
            return;
        std::vector<std::shared_ptr<ScriptInvocationState>> active;
        {
            std::lock_guard lock(provider->mutex);
            if (!provider->active)
                return;
            provider->active = false;
            provider->cancellation.RequestCancellation();
            for (const auto &[id, weak] : provider->invocations) {
                if (auto invocation = weak.lock(); invocation != nullptr)
                    active.push_back(std::move(invocation));
            }
        }
        for (const auto &invocation : active)
            ApplyCancellation(invocation, reason);

        std::vector<std::shared_ptr<ScriptInvocationContextState>> contexts;
        {
            std::lock_guard lock(registry->mutex);
            for (const auto &[id, context] : registry->contexts)
                contexts.push_back(context);
            const auto found = registry->providers.find(provider->generation);
            if (found != registry->providers.end() && found->second.get() == provider.get())
                registry->providers.erase(found);
        }
        RevokeHandlesForProvider(contexts, provider->generation);
    }

    void ScriptInvocationRegistry::ResetContextState(const std::shared_ptr<ScriptInvocationRegistryState> &registry,
                                                     const std::shared_ptr<ScriptInvocationContextState> &context,
                                                     ScriptInvocationCancellationReason reason) noexcept {
        if (context == nullptr)
            return;
        std::vector<std::shared_ptr<ScriptInvocationState>> active;
        {
            std::lock_guard lock(context->mutex);
            if (!context->active)
                return;
            context->active = false;
            context->cancellation.RequestCancellation();
            context->events.clear();
            for (const auto &[id, invocation] : context->invocations)
                active.push_back(invocation);
            context->handles.clear();
        }
        for (const auto &invocation : active)
            ApplyCancellation(invocation, reason);
        {
            std::lock_guard lock(registry->mutex);
            const auto found = registry->contexts.find(context->id.value);
            if (found != registry->contexts.end() && found->second.get() == context.get())
                registry->contexts.erase(found);
        }
    }

    /** @copydoc ScriptInvocationRegistry::BeginShutdown */
    void ScriptInvocationRegistry::BeginShutdown() noexcept {
        if (state_ == nullptr)
            return;
        std::vector<std::shared_ptr<ScriptInvocationProviderState>> providers;
        std::vector<std::shared_ptr<ScriptInvocationContextState>> contexts;
        {
            std::lock_guard lock(state_->mutex);
            if (state_->shutdown)
                return;
            state_->shutdown = true;
            for (const auto &[id, provider] : state_->providers)
                providers.push_back(provider);
            for (const auto &[id, context] : state_->contexts)
                contexts.push_back(context);
        }
        for (const auto &provider : providers)
            ResetProviderState(state_, provider, ScriptInvocationCancellationReason::Shutdown);
        for (const auto &context : contexts)
            ResetContextState(state_, context, ScriptInvocationCancellationReason::Shutdown);
        std::lock_guard lock(state_->mutex);
        state_->providers.clear();
        state_->contexts.clear();
    }

    /** @copydoc ScriptInvocationRegistry::IsShutdown */
    bool ScriptInvocationRegistry::IsShutdown() const noexcept {
        if (state_ == nullptr)
            return true;
        std::lock_guard lock(state_->mutex);
        return state_->shutdown;
    }
}  // namespace Horo::Extensions
