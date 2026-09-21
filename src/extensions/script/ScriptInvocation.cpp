#include "Horo/Extensions/ScriptInvocation.h"

#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/Foundation/Utf8.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace Horo::Extensions {
    namespace {
        using namespace ExtensionErrors;

        template <typename T> [[nodiscard]] Result<T> InvocationFailure(const ErrorCodeDescriptor &descriptor, std::string message = {}) {
            return Result<T>::Failure(MakeError(descriptor, std::move(message)));
        }

        [[nodiscard]] ScriptError InvocationScriptError(const ErrorCodeDescriptor &descriptor, std::string message = {},
                                                        bool cancelled = false) {
            return ScriptError{
                .domain = descriptor.domain.Value(),
                .code = descriptor.code.Value(),
                .message = message.empty() ? std::string{descriptor.summary} : std::move(message),
                .retryable = descriptor.retryable,
                .cancelled = cancelled,
            };
        }

        [[nodiscard]] std::size_t NormalizeBound(std::size_t value, std::size_t maximum) noexcept {
            return std::min(value == 0 ? std::size_t{1} : value, maximum);
        }

        [[nodiscard]] bool SameRegistry(const std::weak_ptr<ScriptInvocationRegistryState> &weak,
                                        const std::shared_ptr<ScriptInvocationRegistryState> &registry) noexcept {
            const auto owner = weak.lock();
            return owner != nullptr && owner.get() == registry.get();
        }

        std::atomic<std::uint64_t> NextScriptContextId{1};

        [[nodiscard]] std::uint64_t AllocateScriptContextId() noexcept {
            auto current = NextScriptContextId.load(std::memory_order_relaxed);
            while (current != 0) {
                const auto next = current == std::numeric_limits<std::uint64_t>::max() ? 0 : current + 1;
                if (NextScriptContextId.compare_exchange_weak(current, next, std::memory_order_relaxed))
                    return current;
            }
            return 0;
        }

        thread_local std::uint64_t ActiveDrainContext{};
    }  // namespace

    struct ScriptInvocationRegistryState;

    struct ScriptInvocationProviderState final {
        std::weak_ptr<ScriptInvocationRegistryState> registry;
        mutable std::mutex mutex;
        std::uint64_t generation{};
        std::size_t maximumInvocations{};
        bool active{true};
        CancellationSource cancellation;
        std::unordered_map<std::uint64_t, std::weak_ptr<ScriptInvocationState>> invocations;
    };

    struct ScriptInvocationContextState final {
        std::weak_ptr<ScriptInvocationRegistryState> registry;
        mutable std::mutex mutex;
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

        ScriptInvocationContextState(std::weak_ptr<ScriptInvocationRegistryState> registryIn, ScriptContextId context,
                                     ScriptInvocationContextDescriptor descriptor, std::thread::id owner, std::size_t maximumQueued,
                                     std::size_t maximumProgress, ScriptValueLimits valueLimitsIn)
            : registry(std::move(registryIn)), id(context), ownerThread(owner), maximumInvocations(descriptor.maximumInvocations),
              maximumHandles(descriptor.maximumHandles), maximumQueuedEvents(maximumQueued),
              maximumProgressEventsPerInvocation(maximumProgress), valueLimits(std::move(valueLimitsIn)),
              cancellation(descriptor.parentCancellation) {}
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

        ScriptInvocationState(std::weak_ptr<ScriptInvocationRegistryState> registryIn,
                              std::shared_ptr<ScriptInvocationContextState> contextIn,
                              std::shared_ptr<ScriptInvocationProviderState> providerIn, ScriptInvocationId invocation,
                              ScriptInvocationRequest requestIn, ScriptExportInvocationMode mode,
                              ScriptInvocationCompletionAffinity affinity, std::optional<std::chrono::steady_clock::time_point> deadlineIn,
                              ScriptValueLimits valueLimitsIn)
            : registry(std::move(registryIn)), context(std::move(contextIn)), provider(std::move(providerIn)), id(invocation),
              request(std::move(requestIn)), invocationMode(mode), completionAffinity(affinity), hasDeadline(deadlineIn.has_value()),
              deadline(deadlineIn.value_or(std::chrono::steady_clock::time_point{})), valueLimits(std::move(valueLimitsIn)),
              callerCancellation(context->cancellation.Token()) {}
    };

    struct ScriptInvocationRegistryState final {
        explicit ScriptInvocationRegistryState(ScriptInvocationRegistryLimits limitsIn) : limits(std::move(limitsIn)) {}

        mutable std::mutex mutex;
        ScriptInvocationRegistryLimits limits;
        bool shutdown{};
        std::uint64_t nextInvocation{1};
        std::unordered_map<std::uint64_t, std::shared_ptr<ScriptInvocationProviderState>> providers;
        std::unordered_map<std::uint64_t, std::shared_ptr<ScriptInvocationContextState>> contexts;
        std::atomic<std::size_t> activeInvocations{};
    };

    namespace {
        [[nodiscard]] ScriptInvocationCancellationReason PendingCancellationLocked(const ScriptInvocationState &state) noexcept {
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

        [[nodiscard]] bool HasUnreservedEventSlot(const ScriptInvocationContextState &context) noexcept {
            if (context.events.size() >= context.maximumQueuedEvents)
                return false;
            return context.reservedTerminalSlots < context.maximumQueuedEvents - context.events.size();
        }

        [[nodiscard]] ScriptError CancellationError(ScriptInvocationCancellationReason reason) {
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

        [[nodiscard]] ScriptInvocationCancellationReason ObserveAndMaybeCancel(const std::shared_ptr<ScriptInvocationState> &state) {
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

        [[nodiscard]] Result<void> ValidateProgress(const ScriptInvocationProgress &progress, const ScriptValueLimits &limits) {
            if (progress.phase.empty() || progress.phase.size() > limits.maximumTypeIdentityBytes ||
                !IsValidUtf8ScalarSequence(progress.phase))
                return InvocationFailure<void>(ScriptInvocationInvalid, "Script invocation phase identity is malformed.");
            if (progress.totalUnits == 0 || progress.completedUnits > progress.totalUnits)
                return InvocationFailure<void>(ScriptInvocationInvalid, "Script invocation progress counters are malformed.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateRequest(const ScriptInvocationRequest &request, const ScriptValueLimits &limits,
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

        [[nodiscard]] Result<void> ValidateTerminalResult(const ScriptInvocationState &state, const ScriptCallResult &result) {
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

        [[nodiscard]] ScriptInvocationSnapshot SnapshotLocked(const ScriptInvocationState &state) {
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
    }  // namespace

    /** @copydoc ScriptInvocationProgress::Fraction */
    double ScriptInvocationProgress::Fraction() const noexcept {
        if (totalUnits == 0)
            return 0.0;
        return static_cast<double>(completedUnits) / static_cast<double>(totalUnits);
    }

    /** @copydoc ScriptInvocationSnapshot::IsTerminal */
    bool ScriptInvocationSnapshot::IsTerminal() const noexcept {
        return state == ScriptInvocationStateKind::Completed || state == ScriptInvocationStateKind::Failed ||
               state == ScriptInvocationStateKind::Cancelled;
    }

    ScriptInvocationProviderRegistration::ScriptInvocationProviderRegistration(
        std::weak_ptr<ScriptInvocationRegistryState> registry, std::shared_ptr<ScriptInvocationProviderState> provider) noexcept
        : registry_(std::move(registry)), provider_(std::move(provider)) {}

    /** @copydoc ScriptInvocationProviderRegistration::~ScriptInvocationProviderRegistration */
    ScriptInvocationProviderRegistration::~ScriptInvocationProviderRegistration() noexcept {
        Reset();
    }

    /** @copydoc ScriptInvocationProviderRegistration::ScriptInvocationProviderRegistration */
    ScriptInvocationProviderRegistration::ScriptInvocationProviderRegistration(ScriptInvocationProviderRegistration &&other) noexcept
        : registry_(std::move(other.registry_)), provider_(std::move(other.provider_)) {}

    /** @copydoc ScriptInvocationProviderRegistration::operator= */
    ScriptInvocationProviderRegistration &ScriptInvocationProviderRegistration::operator=(
        ScriptInvocationProviderRegistration &&other) noexcept {
        if (this != &other) {
            Reset();
            registry_ = std::move(other.registry_);
            provider_ = std::move(other.provider_);
        }
        return *this;
    }

    /** @copydoc ScriptInvocationProviderRegistration::Reset */
    void ScriptInvocationProviderRegistration::Reset() noexcept {
        if (provider_ == nullptr)
            return;
        if (const auto registry = registry_.lock(); registry != nullptr) {
            ScriptInvocationRegistry::ResetProviderState(registry, provider_, ScriptInvocationCancellationReason::Provider);
        } else {
            std::lock_guard lock(provider_->mutex);
            provider_->active = false;
            provider_->cancellation.RequestCancellation();
        }
    }

    /** @copydoc ScriptInvocationProviderRegistration::IsRegistered */
    bool ScriptInvocationProviderRegistration::IsRegistered() const noexcept {
        if (provider_ == nullptr)
            return false;
        std::lock_guard lock(provider_->mutex);
        return provider_->active;
    }

    /** @copydoc ScriptInvocationProviderRegistration::Generation */
    std::uint64_t ScriptInvocationProviderRegistration::Generation() const noexcept {
        return provider_ == nullptr ? 0 : provider_->generation;
    }

    ScriptInvocationContextRegistration::ScriptInvocationContextRegistration(std::weak_ptr<ScriptInvocationRegistryState> registry,
                                                                             std::shared_ptr<ScriptInvocationContextState> context) noexcept
        : registry_(std::move(registry)), context_(std::move(context)) {}

    /** @copydoc ScriptInvocationContextRegistration::~ScriptInvocationContextRegistration */
    ScriptInvocationContextRegistration::~ScriptInvocationContextRegistration() noexcept {
        Reset();
    }

    /** @copydoc ScriptInvocationContextRegistration::ScriptInvocationContextRegistration */
    ScriptInvocationContextRegistration::ScriptInvocationContextRegistration(ScriptInvocationContextRegistration &&other) noexcept
        : registry_(std::move(other.registry_)), context_(std::move(other.context_)) {}

    /** @copydoc ScriptInvocationContextRegistration::operator= */
    ScriptInvocationContextRegistration &ScriptInvocationContextRegistration::operator=(
        ScriptInvocationContextRegistration &&other) noexcept {
        if (this != &other) {
            Reset();
            registry_ = std::move(other.registry_);
            context_ = std::move(other.context_);
        }
        return *this;
    }

    /** @copydoc ScriptInvocationContextRegistration::Reset */
    void ScriptInvocationContextRegistration::Reset() noexcept {
        if (context_ == nullptr)
            return;
        if (const auto registry = registry_.lock(); registry != nullptr) {
            ScriptInvocationRegistry::ResetContextState(registry, context_, ScriptInvocationCancellationReason::Context);
        } else {
            std::lock_guard lock(context_->mutex);
            context_->active = false;
            context_->cancellation.RequestCancellation();
            context_->events.clear();
            context_->handles.clear();
        }
    }

    /** @copydoc ScriptInvocationContextRegistration::IsRegistered */
    bool ScriptInvocationContextRegistration::IsRegistered() const noexcept {
        if (context_ == nullptr)
            return false;
        std::lock_guard lock(context_->mutex);
        return context_->active;
    }

    /** @copydoc ScriptInvocationContextRegistration::Id */
    ScriptContextId ScriptInvocationContextRegistration::Id() const noexcept {
        return context_ == nullptr ? ScriptContextId{} : context_->id;
    }

    ScriptInvocationHandle::ScriptInvocationHandle(std::shared_ptr<ScriptInvocationState> state) noexcept : state_(std::move(state)) {}

    /** @copydoc ScriptInvocationHandle::IsValid */
    bool ScriptInvocationHandle::IsValid() const noexcept {
        return state_ != nullptr && state_->id.IsValid();
    }

    /** @copydoc ScriptInvocationHandle::Id */
    ScriptInvocationId ScriptInvocationHandle::Id() const noexcept {
        return state_ == nullptr ? ScriptInvocationId{} : state_->id;
    }

    /** @copydoc ScriptInvocationHandle::Snapshot */
    std::optional<ScriptInvocationSnapshot> ScriptInvocationHandle::Snapshot() const {
        if (state_ == nullptr)
            return std::nullopt;
        (void)ObserveAndMaybeCancel(state_);
        std::scoped_lock lock(state_->provider->mutex, state_->context->mutex);
        return SnapshotLocked(*state_);
    }

    /** @copydoc ScriptInvocationHandle::Revision */
    std::optional<std::uint64_t> ScriptInvocationHandle::Revision() const noexcept {
        if (state_ == nullptr)
            return std::nullopt;
        (void)ObserveAndMaybeCancel(state_);
        std::scoped_lock lock(state_->provider->mutex, state_->context->mutex);
        return state_->revision;
    }

    /** @copydoc ScriptInvocationHandle::RequestCancellation */
    ScriptInvocationCancellationRequestResult ScriptInvocationHandle::RequestCancellation() const noexcept {
        if (state_ == nullptr)
            return ScriptInvocationCancellationRequestResult::InvalidHandle;
        {
            std::scoped_lock lock(state_->provider->mutex, state_->context->mutex);
            if (state_->terminalResult.has_value())
                return ScriptInvocationCancellationRequestResult::AlreadyTerminal;
            if (state_->callerRequested)
                return ScriptInvocationCancellationRequestResult::AlreadyRequested;
            state_->callerRequested = true;
            state_->callerCancellation.RequestCancellation();
        }
        ApplyCancellation(state_, ScriptInvocationCancellationReason::Caller);
        return ScriptInvocationCancellationRequestResult::Requested;
    }

    ScriptInvocationController::ScriptInvocationController(std::shared_ptr<ScriptInvocationState> state) noexcept
        : state_(std::move(state)) {}

    /** @copydoc ScriptInvocationController::~ScriptInvocationController */
    ScriptInvocationController::~ScriptInvocationController() noexcept {
        Abandon();
    }

    /** @copydoc ScriptInvocationController::ScriptInvocationController */
    ScriptInvocationController::ScriptInvocationController(ScriptInvocationController &&other) noexcept : state_(std::move(other.state_)) {}

    /** @copydoc ScriptInvocationController::operator= */
    ScriptInvocationController &ScriptInvocationController::operator=(ScriptInvocationController &&other) noexcept {
        if (this != &other) {
            Abandon();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    /** @copydoc ScriptInvocationController::Handle */
    ScriptInvocationHandle ScriptInvocationController::Handle() const noexcept {
        return ScriptInvocationHandle{state_};
    }

    /** @copydoc ScriptInvocationController::Request */
    const ScriptInvocationRequest *ScriptInvocationController::Request() const noexcept {
        return state_ == nullptr ? nullptr : &state_->request;
    }

    /** @copydoc ScriptInvocationController::Cancellation */
    CancellationToken ScriptInvocationController::Cancellation() const noexcept {
        return state_ == nullptr ? CancellationToken{} : state_->callerCancellation.Token();
    }

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

        std::shared_ptr<ScriptInvocationState> invocation;
        {
            std::lock_guard registryLock(state_->mutex);
            if (state_->shutdown)
                return InvocationFailure<ScriptInvocationController>(ScriptInvocationShutdown);
            if (state_->activeInvocations.load() >= state_->limits.maximumInvocations)
                return InvocationFailure<ScriptInvocationController>(ScriptInvocationCapacityExceeded);
            std::scoped_lock lock(provider->mutex, context->mutex);
            if (!provider->active || !context->active)
                return InvocationFailure<ScriptInvocationController>(ScriptInvocationUnavailable);
            if (context->invocations.size() >= context->maximumInvocations || provider->invocations.size() >= provider->maximumInvocations)
                return InvocationFailure<ScriptInvocationController>(ScriptInvocationCapacityExceeded);
            if (!HasUnreservedEventSlot(*context))
                return InvocationFailure<ScriptInvocationController>(ScriptInvocationCapacityExceeded,
                                                                     "Script invocation terminal delivery capacity is full.");
            if (state_->nextInvocation == 0)
                return InvocationFailure<ScriptInvocationController>(ScriptInvocationCapacityExceeded,
                                                                     "Script invocation identity exhausted.");
            const ScriptInvocationId id{state_->nextInvocation++};
            std::optional<std::chrono::steady_clock::time_point> deadline;
            if (request.timeout.has_value())
                deadline = std::chrono::steady_clock::now() + *request.timeout;
            invocation = std::make_shared<ScriptInvocationState>(state_, context, provider, id, std::move(request), function->invocation,
                                                                 ScriptInvocationCompletionAffinity::OwnerThreadSafePoint, deadline,
                                                                 state_->limits.value);
            context->invocations.emplace(id.value, invocation);
            provider->invocations.emplace(id.value, invocation);
            ++context->reservedTerminalSlots;
            state_->activeInvocations.fetch_add(1);
        }
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
