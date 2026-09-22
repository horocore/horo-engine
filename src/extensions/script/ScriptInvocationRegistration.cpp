#include "ScriptInvocationInternal.h"

#include <mutex>
#include <utility>

namespace Horo::Extensions {
    using namespace Detail;
    using namespace ExtensionErrors;

    /** @copydoc ScriptInvocationProgress::Fraction */
    double ScriptInvocationProgress::Fraction() const noexcept {
        if (totalUnits == 0)
            return 0.0;
        return static_cast<double>(completedUnits) / static_cast<double>(totalUnits);
    }

    /** @copydoc ScriptInvocationSnapshot::IsTerminal */
    bool ScriptInvocationSnapshot::IsTerminal() const noexcept {
        using enum ScriptInvocationStateKind;
        return state == Completed || state == Failed || state == Cancelled;
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
    void ScriptInvocationProviderRegistration::Reset() const noexcept {
        if (provider_ == nullptr)
            return;
        if (const auto registry = registry_.lock(); registry != nullptr) {
            ScriptInvocationRegistry::ResetProviderState(registry, provider_, ScriptInvocationCancellationReason::Provider);
        } else {
            std::lock_guard lock(provider_->Mutex());
            provider_->active = false;
            provider_->cancellation.RequestCancellation();
        }
    }

    /** @copydoc ScriptInvocationProviderRegistration::IsRegistered */
    bool ScriptInvocationProviderRegistration::IsRegistered() const noexcept {
        if (provider_ == nullptr)
            return false;
        std::lock_guard lock(provider_->Mutex());
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
    void ScriptInvocationContextRegistration::Reset() const noexcept {
        if (context_ == nullptr)
            return;
        if (const auto registry = registry_.lock(); registry != nullptr) {
            ScriptInvocationRegistry::ResetContextState(registry, context_, ScriptInvocationCancellationReason::Context);
        } else {
            std::lock_guard lock(context_->Mutex());
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
        std::lock_guard lock(context_->Mutex());
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
        std::scoped_lock lock(state_->provider->Mutex(), state_->context->Mutex());
        return SnapshotLocked(*state_);
    }

    /** @copydoc ScriptInvocationHandle::Revision */
    std::optional<std::uint64_t> ScriptInvocationHandle::Revision() const noexcept {
        if (state_ == nullptr)
            return std::nullopt;
        (void)ObserveAndMaybeCancel(state_);
        std::scoped_lock lock(state_->provider->Mutex(), state_->context->Mutex());
        return state_->revision;
    }

    /** @copydoc ScriptInvocationHandle::RequestCancellation */
    ScriptInvocationCancellationRequestResult ScriptInvocationHandle::RequestCancellation() const noexcept {
        using enum ScriptInvocationCancellationRequestResult;
        if (state_ == nullptr)
            return InvalidHandle;
        {
            std::scoped_lock lock(state_->provider->Mutex(), state_->context->Mutex());
            if (state_->terminalResult.has_value())
                return AlreadyTerminal;
            if (state_->callerRequested)
                return AlreadyRequested;
            state_->callerRequested = true;
            state_->callerCancellation.RequestCancellation();
        }
        ApplyCancellation(state_, ScriptInvocationCancellationReason::Caller);
        return Requested;
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

}  // namespace Horo::Extensions
