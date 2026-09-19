#include "Horo/Extensions/BackendOperationRegistry.h"

#include "BackendOperationRegistryInternal.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <array>
#include <new>
#include <utility>

namespace Horo::Extensions {
    using namespace BackendOperationRegistryDetail;
    using namespace BackendOperationValidation;

    /** @copydoc BackendOperationProgress::Fraction */
    double BackendOperationProgress::Fraction() const noexcept {
        if (totalUnits == 0)
            return 0.0;
        return static_cast<double>(completedUnits) / static_cast<double>(totalUnits);
    }

    /** @copydoc BackendOperationSnapshot::IsTerminal */
    bool BackendOperationSnapshot::IsTerminal() const noexcept {
        using enum BackendOperationState;
        return state == Completed || state == Failed || state == Cancelled;
    }

    BackendOperationRegistration::BackendOperationRegistration(std::weak_ptr<BackendOperationRegistryState> registry,
                                                               std::shared_ptr<BackendOperationProviderState> provider) noexcept
        : registry_(std::move(registry)), provider_(std::move(provider)) {}

    /** @copydoc BackendOperationRegistration::~BackendOperationRegistration */
    BackendOperationRegistration::~BackendOperationRegistration() noexcept {
        Reset();
    }

    /** @copydoc BackendOperationRegistration::BackendOperationRegistration */
    BackendOperationRegistration::BackendOperationRegistration(BackendOperationRegistration &&other) noexcept
        : registry_(std::move(other.registry_)), provider_(std::move(other.provider_)) {}

    /** @copydoc BackendOperationRegistration::operator= */
    BackendOperationRegistration &BackendOperationRegistration::operator=(BackendOperationRegistration &&other) noexcept {
        if (this == &other)
            return *this;
        Reset();
        registry_ = std::move(other.registry_);
        provider_ = std::move(other.provider_);
        return *this;
    }

    /** @copydoc BackendOperationRegistration::Reset */
    void BackendOperationRegistration::Reset() noexcept {
        if (!provider_)
            return;
        std::array<std::shared_ptr<BackendOperationStateData>, BackendOperationRegistry::MaximumOperations> operations{};
        std::size_t operationCount = 0;
        if (const auto registry = registry_.lock()) {
            {
                std::scoped_lock lock{registry->mutex};
                provider_->registered.store(false);
                std::erase(registry->providers, provider_);
                for (const auto &operation : registry->operations) {
                    if (operation->provider == provider_) {
                        // Admission keeps this vector at or below MaximumOperations.
                        operations[operationCount++] = operation;
                    }
                }
            }
            for (std::size_t index = 0; index < operationCount; ++index)
                ForceCancel(operations[index], BackendOperationCancellationReason::Provider);
        } else {
            provider_->registered.store(false);
        }
        provider_.reset();
        registry_.reset();
    }

    /** @copydoc BackendOperationRegistration::IsRegistered */
    bool BackendOperationRegistration::IsRegistered() const noexcept {
        return provider_ != nullptr && provider_->registered.load();
    }

    BackendOperationHandle::BackendOperationHandle(std::shared_ptr<BackendOperationStateData> state) noexcept : state_(std::move(state)) {}

    /** @copydoc BackendOperationHandle::IsValid */
    bool BackendOperationHandle::IsValid() const noexcept {
        return state_ != nullptr;
    }

    /** @copydoc BackendOperationHandle::Id */
    BackendOperationId BackendOperationHandle::Id() const noexcept {
        if (!state_)
            return {};
        std::scoped_lock lock{state_->synchronization.Mutex()};
        return state_->snapshot.operation;
    }

    /** @copydoc BackendOperationHandle::Revision */
    std::optional<std::uint64_t> BackendOperationHandle::Revision() const noexcept {
        if (!state_)
            return std::nullopt;
        std::scoped_lock lock{state_->synchronization.Mutex()};
        return state_->snapshot.revision;
    }

    /** @copydoc BackendOperationHandle::Snapshot */
    std::optional<BackendOperationSnapshot> BackendOperationHandle::Snapshot() const {
        if (!state_)
            return std::nullopt;
        std::scoped_lock lock{state_->synchronization.Mutex()};
        return state_->snapshot;
    }

    /** @copydoc BackendOperationHandle::RequestCancellation */
    BackendOperationCancellationRequestResult BackendOperationHandle::RequestCancellation() const noexcept {
        using enum BackendOperationCancellationRequestResult;
        if (!state_)
            return InvalidHandle;
        std::scoped_lock lock{state_->synchronization.Mutex()};
        if (IsTerminal(*state_))
            return AlreadyTerminal;
        if (state_->snapshot.cancellationRequested)
            return AlreadyRequested;
        if (state_->parentCancellation.IsCancellationRequested()) {
            SetCancellationRequested(*state_, BackendOperationCancellationReason::Parent);
            return AlreadyRequested;
        }
        SetCancellationRequested(*state_, BackendOperationCancellationReason::Caller);
        return Requested;
    }

    BackendOperationController::BackendOperationController(std::shared_ptr<BackendOperationStateData> state) noexcept
        : state_(std::move(state)) {}

    /** @copydoc BackendOperationController::~BackendOperationController */
    BackendOperationController::~BackendOperationController() noexcept {
        Abandon();
    }

    /** @copydoc BackendOperationController::BackendOperationController */
    BackendOperationController::BackendOperationController(BackendOperationController &&other) noexcept
        : state_(std::exchange(other.state_, {})) {}

    /** @copydoc BackendOperationController::operator= */
    BackendOperationController &BackendOperationController::operator=(BackendOperationController &&other) noexcept {
        if (this == &other)
            return *this;
        Abandon();
        state_ = std::exchange(other.state_, {});
        return *this;
    }

    /** @copydoc BackendOperationController::Handle */
    BackendOperationHandle BackendOperationController::Handle() const noexcept {
        return BackendOperationHandle{state_};
    }

    /** @copydoc BackendOperationController::Cancellation */
    CancellationToken BackendOperationController::Cancellation() const noexcept {
        return state_ ? state_->cancellation.Token() : CancellationToken{};
    }

    /** @copydoc BackendOperationController::PublishProgress */
    BackendOperationTransitionResult BackendOperationController::PublishProgress(const BackendOperationPhaseId &phase,
                                                                                 const BackendOperationProgress progress) const {
        using enum BackendOperationTransitionResult;
        if (!state_)
            return AlreadyTerminal;
        bool terminalized = false;
        BackendOperationTransitionResult result = InvalidTransition;
        {
            std::scoped_lock lock{state_->synchronization.Mutex()};
            if (IsTerminal(*state_))
                return AlreadyTerminal;
            if (TerminalizePendingCancellationLocked(*state_)) {
                terminalized = true;
                result = CancellationWon;
            } else if (!ValidTypedId(phase.value) || !ValidProgress(progress) ||
                       (state_->snapshot.phase == phase && IsProgressRegression(state_->snapshot.progress, progress))) {
                result = InvalidTransition;
            } else {
                state_->snapshot.state = BackendOperationState::Running;
                state_->snapshot.progress = progress;
                state_->snapshot.phase = phase;
                ++state_->snapshot.revision;
                result = Applied;
            }
        }
        if (terminalized)
            RetireOperation(state_);
        return result;
    }

    /** @copydoc BackendOperationController::AddDiagnostic */
    Result<void> BackendOperationController::AddDiagnostic(Diagnostic diagnostic) const {
        if (!state_)
            return Result<void>::Failure(MakeError(ExtensionErrors::BackendOperationRegistryShutdown));
        bool terminalized = false;
        Result<void> result = Result<void>::Failure(MakeError(ExtensionErrors::BackendOperationPayloadInvalid));
        {
            std::scoped_lock lock{state_->synchronization.Mutex()};
            if (IsTerminal(*state_))
                return Result<void>::Failure(
                    MakeError(ExtensionErrors::BackendOperationRegistryShutdown, "Backend operation is already terminal."));
            if (TerminalizePendingCancellationLocked(*state_)) {
                terminalized = true;
                result = Result<void>::Failure(MakeError(ExtensionErrors::BackendOperationCancelled));
            } else {
                const auto &limits = state_->provider->descriptor;
                const bool validLengths = diagnostic.message.size() <= MaximumDiagnosticBytes &&
                                          diagnostic.location.source.size() <= MaximumIdentityBytes &&
                                          diagnostic.path.size() <= MaximumIdentityBytes;
                if (!ValidTypedId(diagnostic.code.Value()) || !validLengths ||
                    state_->snapshot.diagnostics.size() >= limits.maximumDiagnostics) {
                    result = Result<void>::Failure(MakeError(ExtensionErrors::BackendOperationPayloadInvalid));
                } else {
                    const std::size_t bytes = diagnostic.code.Value().size() + diagnostic.message.size() +
                                              diagnostic.location.source.size() + diagnostic.path.size();
                    if (bytes > limits.maximumDiagnosticBytes || state_->retainedDiagnosticBytes > limits.maximumDiagnosticBytes - bytes) {
                        result = Result<void>::Failure(MakeError(ExtensionErrors::BackendOperationPayloadInvalid));
                    } else {
                        try {
                            state_->snapshot.diagnostics.push_back(std::move(diagnostic));
                            state_->retainedDiagnosticBytes += bytes;
                            ++state_->snapshot.revision;
                            result = Result<void>::Success();
                        } catch (const std::bad_alloc &) {
                            result = Result<void>::Failure(MakeError(ExtensionErrors::BackendOperationPayloadInvalid));
                        }
                    }
                }
            }
        }
        if (terminalized)
            RetireOperation(state_);
        return result;
    }

    /** @copydoc BackendOperationController::ObserveCancellation */
    BackendOperationCancellationObservation BackendOperationController::ObserveCancellation() const noexcept {
        return CancelIfRequested(state_);
    }

    /** @copydoc BackendOperationController::Complete */
    BackendOperationTransitionResult BackendOperationController::Complete(std::optional<BackendOperationResultPayload> result) const {
        using enum BackendOperationTransitionResult;
        return ApplyTerminalTransition(state_, [&result](BackendOperationStateData &state) {
            const auto &limits = state.provider->descriptor;
            if (const bool validResult = !result.has_value() || (result->id == state.snapshot.result && !result->id.value.empty() &&
                                                                 result->bytes.size() <= limits.maximumResultBytes);
                !validResult)
                return InvalidTransition;
            state.snapshot.resultPayload = std::move(result);
            state.snapshot.progress = {.completedUnits = 1, .totalUnits = 1};
            TerminalizeLocked(state, BackendOperationState::Completed, std::nullopt);
            return Applied;
        });
    }

    /** @copydoc BackendOperationController::Fail */
    BackendOperationTransitionResult BackendOperationController::Fail(Error error) const {
        using enum BackendOperationTransitionResult;
        return ApplyTerminalTransition(state_, [&error](BackendOperationStateData &state) {
            TerminalizeLocked(state, BackendOperationState::Failed, std::move(error));
            return Applied;
        });
    }

    void BackendOperationController::Abandon() noexcept {
        if (!state_)
            return;
        bool terminalized = false;
        {
            std::scoped_lock lock{state_->synchronization.Mutex()};
            if (!IsTerminal(*state_)) {
                if (!TerminalizePendingCancellationLocked(*state_))
                    TerminalizeLocked(*state_, BackendOperationState::Failed, std::move(state_->abandonmentError));
                terminalized = true;
            }
        }
        if (terminalized)
            RetireOperation(state_);
        state_.reset();
    }

    BackendOperationRegistry::BackendOperationRegistry(BackendOperationRegistryConfig config)
        : state_(std::make_shared<BackendOperationRegistryState>()) {
        state_->config.maximumOperations = std::clamp(config.maximumOperations, std::size_t{1}, MaximumOperations);
        state_->providers.reserve(MaximumProviders);
        state_->operations.reserve(state_->config.maximumOperations);
    }

    /** @copydoc BackendOperationRegistry::~BackendOperationRegistry */
    BackendOperationRegistry::~BackendOperationRegistry() noexcept {
        BeginShutdown();
    }

    /** @copydoc BackendOperationRegistry::Register */
    Result<BackendOperationRegistration> BackendOperationRegistry::Register(BackendOperationProviderDescriptor descriptor) const {
        if (!ValidProviderDescriptor(descriptor))
            return Result<BackendOperationRegistration>::Failure(MakeError(ExtensionErrors::BackendOperationRegistryInvalid));
        if (!state_)
            return Result<BackendOperationRegistration>::Failure(MakeError(ExtensionErrors::BackendOperationRegistryShutdown));
        std::scoped_lock lock{state_->mutex};
        if (state_->shutdown)
            return Result<BackendOperationRegistration>::Failure(MakeError(ExtensionErrors::BackendOperationRegistryShutdown));
        if (std::ranges::any_of(state_->providers, [&](const auto &provider) {
            return provider->descriptor.provider == descriptor.provider;
        }))
            return Result<BackendOperationRegistration>::Failure(MakeError(ExtensionErrors::BackendOperationRegistryDuplicate));
        if (state_->providers.size() >= MaximumProviders)
            return Result<BackendOperationRegistration>::Failure(MakeError(ExtensionErrors::BackendOperationRegistryCapacityExceeded));
        try {
            auto provider = std::make_shared<BackendOperationProviderState>();
            provider->descriptor = std::move(descriptor);
            state_->providers.push_back(provider);
            return Result<BackendOperationRegistration>::Success(BackendOperationRegistration{state_, std::move(provider)});
        } catch (const std::bad_alloc &) {
            return Result<BackendOperationRegistration>::Failure(MakeError(ExtensionErrors::BackendOperationRegistryCapacityExceeded));
        }
    }

    /** @copydoc BackendOperationRegistry::Begin */
    Result<BackendOperationController> BackendOperationRegistry::Begin(const ApplicationCapabilityProviderIdentity &provider,
                                                                       const BackendOperationTypeId &type,
                                                                       BackendOperationDescriptor descriptor) {
        if (!ValidProvider(provider) || !ValidTypedId(type.value) || !ValidOperationDescriptor(descriptor))
            return Result<BackendOperationController>::Failure(MakeError(ExtensionErrors::BackendOperationRegistryInvalid));
        if (!state_)
            return Result<BackendOperationController>::Failure(MakeError(ExtensionErrors::BackendOperationRegistryShutdown));
        std::scoped_lock lock{state_->mutex};
        if (state_->shutdown)
            return Result<BackendOperationController>::Failure(MakeError(ExtensionErrors::BackendOperationRegistryShutdown));
        const auto selected = std::ranges::find_if(state_->providers, [&](const auto &candidate) {
            return candidate->descriptor.provider == provider && candidate->registered.load() && FindType(*candidate, type) != nullptr;
        });
        if (selected == state_->providers.end())
            return Result<BackendOperationController>::Failure(MakeError(ExtensionErrors::BackendOperationProviderUnavailable));
        if (state_->operations.size() >= state_->config.maximumOperations ||
            (*selected)->activeOperations.load(std::memory_order_relaxed) >= (*selected)->descriptor.maximumOperations)
            return Result<BackendOperationController>::Failure(MakeError(ExtensionErrors::BackendOperationRegistryCapacityExceeded));
        const auto id = AllocateOperationId(*state_);
        if (!id.has_value())
            return Result<BackendOperationController>::Failure(MakeError(ExtensionErrors::BackendOperationRegistryCapacityExceeded));
        const auto *operationType = FindType(**selected, type);
        BackendOperationSnapshot snapshot{
            .operation = *id,
            .provider = provider,
            .type = type,
            .result = operationType->result,
            .state = BackendOperationState::Queued,
            .phase = descriptor.initialPhase,
            .progress = {},
        };
        try {
            auto operation =
                std::make_shared<BackendOperationStateData>(state_, *selected, std::move(snapshot), descriptor.parentCancellation);
            operation->cancellationError = CancellationError();
            operation->abandonmentError = AbandonmentError();
            state_->operations.push_back(operation);
            (*selected)->activeOperations.fetch_add(1U, std::memory_order_relaxed);
            return Result<BackendOperationController>::Success(BackendOperationController{std::move(operation)});
        } catch (const std::bad_alloc &) {
            return Result<BackendOperationController>::Failure(MakeError(ExtensionErrors::BackendOperationRegistryCapacityExceeded));
        }
    }

    /** @copydoc BackendOperationRegistry::BeginShutdown */
    void BackendOperationRegistry::BeginShutdown() const noexcept {
        if (!state_)
            return;
        std::array<std::shared_ptr<BackendOperationStateData>, MaximumOperations> operations{};
        std::size_t operationCount = 0;
        {
            std::scoped_lock lock{state_->mutex};
            if (state_->shutdown)
                return;
            state_->shutdown = true;
            for (const auto &provider : state_->providers)
                provider->registered.store(false);
            for (const auto &operation : state_->operations) {
                // Admission keeps this vector at or below MaximumOperations.
                operations[operationCount++] = operation;
            }
            state_->providers.clear();
        }
        for (std::size_t index = 0; index < operationCount; ++index)
            ForceCancel(operations[index], BackendOperationCancellationReason::Shutdown);
    }

    /** @copydoc BackendOperationRegistry::IsShutdown */
    bool BackendOperationRegistry::IsShutdown() const noexcept {
        if (!state_)
            return true;
        std::scoped_lock lock{state_->mutex};
        return state_->shutdown;
    }
}  // namespace Horo::Extensions
