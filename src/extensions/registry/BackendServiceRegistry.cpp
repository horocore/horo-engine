#include "Horo/Extensions/BackendServiceRegistry.h"

#include "../ExtensionAuthorityIdentityValidation.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <condition_variable>
#include <exception>
#include <format>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace Horo::Extensions {
    enum class BackendServiceProviderLifecycle : std::uint8_t {
        Active,
        Retiring,
        AwaitingOwnerFinalization,
        Finalizing,
        RetainedRestartRequired,
        Shutdown,
    };

    struct BackendServiceProviderState final {
        BackendServiceDescriptor descriptor;
        Detail::BackendServiceOwnedImplementation implementation{BackendServiceCodeLease::Retain(std::shared_ptr<const void>{}), {}};
        const void *typeTag{};
        void (*shutdown)(void *) noexcept {};
        std::thread::id ownerThread;
        CancellationSource cancellation;
        std::mutex mutex;
        std::condition_variable drained;
        std::size_t activeCalls{};
        std::atomic_bool registered{true};
        BackendServiceProviderLifecycle lifecycle{BackendServiceProviderLifecycle::Active};
        std::weak_ptr<BackendServiceRegistryState> registry;
        std::uint64_t registrationSequence{};
        bool retirementQueued{};
        bool retirementAttached{};  // Protected by mutex; binding precedes the first executable call.
    };

    struct BackendServiceRegistryState final {
        std::mutex mutex;
        std::vector<std::shared_ptr<BackendServiceProviderState>> providers;
        std::vector<std::shared_ptr<BackendServiceProviderState>> retiredProviders;
        BackendServiceRegistryConfig config;
        std::timed_mutex retirementMutex;
        std::atomic_bool restartRequired{};
        std::atomic_bool stagingShutdown{};
        std::uint64_t nextRegistrationSequence{};
        std::shared_ptr<BackendServiceRegistryState> restartQuarantine;
        bool quarantined{};
        bool shutdown{};
    };

    namespace {
        struct ExecutingProviderStack final {
            std::array<BackendServiceProviderState *, BackendServiceRegistry::MaximumServices> providers{};
            std::size_t size{};
        };

        thread_local ExecutingProviderStack ExecutingProviders;

        [[nodiscard]] bool IsExecuting(const BackendServiceProviderState *provider) noexcept {
            const auto end = ExecutingProviders.providers.begin() + static_cast<std::ptrdiff_t>(ExecutingProviders.size);
            return std::ranges::find(ExecutingProviders.providers.begin(), end, provider) != end;
        }

        [[nodiscard]] bool ValidDescriptor(const BackendServiceDescriptor &descriptor) noexcept {
            const std::array valid{
                Detail::IsCanonicalExtensionAuthorityId(descriptor.serviceId.value),
                Detail::IsCanonicalExtensionAuthorityId(descriptor.contractId.value),
                Detail::IsCanonicalExtensionAuthorityId(descriptor.capability.value),
                Detail::IsCanonicalExtensionAuthorityId(descriptor.provider.moduleId),
                Detail::IsCanonicalExtensionAuthorityId(descriptor.provider.providerId),
                descriptor.version != ApplicationCapabilityVersion{},
                descriptor.provider.generation != 0U,
                static_cast<std::uint8_t>(descriptor.threadRule) <=
                    static_cast<std::uint8_t>(BackendServiceThreadRule::ProviderOwnerThread),
            };
            return std::ranges::find(valid, false) == valid.end();
        }

        [[nodiscard]] std::optional<ApplicationCapabilityVersion> ParseProviderVersion(const std::string_view encoded) noexcept {
            std::array<std::uint32_t, 3> components{};
            std::string_view remaining = encoded;
            for (std::size_t index = 0; index < components.size(); ++index) {
                const std::size_t separator = remaining.find('.');
                if ((index + 1U < components.size()) != (separator != std::string_view::npos))
                    return std::nullopt;
                const std::string_view digits = remaining.substr(0, separator);
                const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), components[index]);
                if (digits.empty() || (digits.size() > 1U && digits.front() == '0') || error != std::errc{} ||
                    end != digits.data() + digits.size() || components[index] > std::numeric_limits<std::uint16_t>::max())
                    return std::nullopt;
                if (separator != std::string_view::npos)
                    remaining.remove_prefix(separator + 1U);
            }
            return ApplicationCapabilityVersion{static_cast<std::uint16_t>(components[0]), static_cast<std::uint16_t>(components[1]),
                                                static_cast<std::uint16_t>(components[2])};
        }

        [[nodiscard]] bool CanFinalizeHere(const BackendServiceProviderState &provider) noexcept {
            const std::array allowed{provider.descriptor.threadRule == BackendServiceThreadRule::AnyThread,
                                     provider.ownerThread == std::this_thread::get_id()};
            return std::ranges::find(allowed, true) != allowed.end();
        }

        /** @brief Runs provider shutdown outside its state lock, then releases service ownership before its executable-code lease. */
        [[nodiscard]] BackendServiceRetirementDisposition FinalizeProvider(const std::shared_ptr<BackendServiceRegistryState> &registry,
                                                                           const std::shared_ptr<BackendServiceProviderState> &provider,
                                                                           std::unique_lock<std::mutex> &lock) noexcept {
            provider->lifecycle = BackendServiceProviderLifecycle::Finalizing;
            std::shared_ptr<void> service = provider->implementation.service;
            const auto shutdown = provider->shutdown;
            lock.unlock();
            shutdown(service.get());
            lock.lock();
            if (registry->restartRequired.load(std::memory_order_acquire)) {
                provider->lifecycle = BackendServiceProviderLifecycle::RetainedRestartRequired;
                lock.unlock();
                provider->drained.notify_all();
                return BackendServiceRetirementDisposition::RestartRequired;
            }
            service.reset();
            Detail::BackendServiceOwnedImplementation retired = std::move(provider->implementation);
            provider->lifecycle = BackendServiceProviderLifecycle::Shutdown;
            lock.unlock();
            provider->drained.notify_all();
            retired.service.reset();
            return BackendServiceRetirementDisposition::ShutdownComplete;
        }

        [[nodiscard]] BackendServiceRetirementDisposition StopProvider(const std::shared_ptr<BackendServiceRegistryState> &registry,
                                                                       const std::shared_ptr<BackendServiceProviderState> &provider,
                                                                       const std::chrono::steady_clock::time_point deadline,
                                                                       const bool waitForDrain) noexcept {
            std::unique_lock lock{provider->mutex};

            struct EarlyDisposition final {
                bool applies;
                BackendServiceRetirementDisposition disposition;
            };

            const std::array earlyDispositions{
                EarlyDisposition{provider->lifecycle == BackendServiceProviderLifecycle::Shutdown,
                                 BackendServiceRetirementDisposition::ShutdownComplete},
                EarlyDisposition{provider->lifecycle == BackendServiceProviderLifecycle::RetainedRestartRequired,
                                 BackendServiceRetirementDisposition::RestartRequired},
                EarlyDisposition{IsExecuting(provider.get()), BackendServiceRetirementDisposition::DeferredUntilCallExit},
            };
            if (const auto early = std::ranges::find(earlyDispositions, true, &EarlyDisposition::applies); early != earlyDispositions.end())
                return early->disposition;
            if (provider->activeCalls != 0U) {
                const std::array deadlines{std::chrono::steady_clock::now(), deadline};
                if (!provider->drained.wait_until(lock, deadlines[static_cast<std::size_t>(waitForDrain)], [&provider] {
                    return provider->activeCalls == 0U;
                })) {
                    constexpr std::array lifecycles{BackendServiceProviderLifecycle::Retiring,
                                                    BackendServiceProviderLifecycle::RetainedRestartRequired};
                    constexpr std::array dispositions{BackendServiceRetirementDisposition::DeferredUntilCallExit,
                                                      BackendServiceRetirementDisposition::RestartRequired};
                    if (waitForDrain)
                        registry->restartRequired.store(true, std::memory_order_release);
                    provider->lifecycle = lifecycles[static_cast<std::size_t>(waitForDrain)];
                    return dispositions[static_cast<std::size_t>(waitForDrain)];
                }
            }
            if (!CanFinalizeHere(*provider)) {
                provider->lifecycle = BackendServiceProviderLifecycle::AwaitingOwnerFinalization;
                return BackendServiceRetirementDisposition::OwnerThreadFinalizationRequired;
            }
            return FinalizeProvider(registry, provider, lock);
        }

        void RequestRetirement(const std::shared_ptr<BackendServiceProviderState> &provider) noexcept {
            std::scoped_lock lock{provider->mutex};
            provider->registered.store(false, std::memory_order_release);
            provider->cancellation.RequestCancellation();
            if (provider->lifecycle == BackendServiceProviderLifecycle::Active)
                provider->lifecycle = BackendServiceProviderLifecycle::Retiring;
        }

        void EnqueueRetirement(BackendServiceRegistryState &registry, const std::shared_ptr<BackendServiceProviderState> &provider) {
            if (provider->retirementQueued)
                return;
            const auto position = std::ranges::lower_bound(registry.retiredProviders, provider->registrationSequence, std::greater{},
                                                           &BackendServiceProviderState::registrationSequence);
            registry.retiredProviders.insert(position, provider);
            provider->retirementQueued = true;
        }

        [[nodiscard]] BackendServiceRetirementDisposition ProgressRetirements(const std::shared_ptr<BackendServiceRegistryState> &registry,
                                                                              const std::chrono::steady_clock::time_point deadline,
                                                                              const bool waitForDrain) noexcept {
            using enum BackendServiceRetirementDisposition;
            if (registry->stagingShutdown.load(std::memory_order_acquire))
                return DeferredUntilCallExit;
            std::unique_lock coordinator{registry->retirementMutex, std::defer_lock};
            if (const std::array coordinatorDeadlines{std::chrono::steady_clock::now(), deadline};
                !coordinator.try_lock_until(coordinatorDeadlines[static_cast<std::size_t>(waitForDrain)])) {
                if (waitForDrain)
                    registry->restartRequired.store(true, std::memory_order_release);
                constexpr std::array dispositions{DeferredUntilCallExit, RestartRequired};
                return dispositions[static_cast<std::size_t>(waitForDrain)];
            }
            constexpr std::array initialDispositions{ShutdownComplete, RestartRequired};
            auto aggregate = initialDispositions[static_cast<std::size_t>(registry->restartRequired.load(std::memory_order_acquire))];
            for (;;) {
                std::shared_ptr<BackendServiceProviderState> provider;
                {
                    std::scoped_lock lock{registry->mutex};
                    if (registry->retiredProviders.empty())
                        return aggregate;
                    provider = registry->retiredProviders.front();
                }
                const auto disposition = StopProvider(registry, provider, deadline, waitForDrain);
                aggregate = std::max(aggregate, disposition);
                if (disposition != ShutdownComplete)
                    return aggregate;
                std::scoped_lock lock{registry->mutex};
                std::erase(registry->retiredProviders, provider);
            }
        }

        [[nodiscard]] BackendServiceRetirementDisposition RetireProvider(
            const std::shared_ptr<BackendServiceRegistryState> &registry,
            const std::shared_ptr<BackendServiceProviderState> &provider) noexcept {
            {
                std::scoped_lock lock{registry->mutex};
                std::erase(registry->providers, provider);
            }
            RequestRetirement(provider);
            {
                std::scoped_lock lock{registry->mutex};
                EnqueueRetirement(*registry, provider);
            }
            return ProgressRetirements(registry, std::chrono::steady_clock::now() + registry->config.drainDeadline, true);
        }

        void QuarantineForRestart(const std::shared_ptr<BackendServiceRegistryState> &registry) noexcept {
            static std::mutex mutex;
            static const BackendServiceRegistryState *quarantinedRegistry{};
            std::scoped_lock lock{mutex};
            if (registry->quarantined)
                return;
            if (quarantinedRegistry != nullptr)
                std::terminate();
            registry->restartQuarantine = registry;
            registry->quarantined = true;
            quarantinedRegistry = registry.get();
        }

    }  // namespace

    BackendServiceCallContext::BackendServiceCallContext(const BackendServiceDescriptor &provider, CancellationToken caller,
                                                         CancellationToken providerCancellation) noexcept
        : provider_(&provider), caller_(std::move(caller)), providerCancellation_(std::move(providerCancellation)) {}

    /** @copydoc BackendServiceCallContext::Provider */
    const BackendServiceDescriptor &BackendServiceCallContext::Provider() const noexcept {
        return *provider_;
    }

    /** @copydoc BackendServiceCallContext::IsCancellationRequested */
    bool BackendServiceCallContext::IsCancellationRequested() const noexcept {
        const std::array cancelled{caller_.IsCancellationRequested(), providerCancellation_.IsCancellationRequested()};
        return std::ranges::find(cancelled, true) != cancelled.end();
    }

    BackendServiceCallAdmission::BackendServiceCallAdmission(std::shared_ptr<BackendServiceProviderState> provider,
                                                             BackendServiceCallContext context) noexcept
        : provider_(std::move(provider)), context_(std::move(context)) {
        ExecutingProviders.providers[ExecutingProviders.size++] = provider_.get();
    }

    BackendServiceCallAdmission::~BackendServiceCallAdmission() {
        Reset();
    }

    /** @copydoc BackendServiceCallAdmission::Context */
    const BackendServiceCallContext &BackendServiceCallAdmission::Context() const noexcept {
        return context_;
    }

    void BackendServiceCallAdmission::Reset() noexcept {
        if (provider_ == nullptr)
            return;
        const std::shared_ptr<BackendServiceRegistryState> registry = provider_->registry.lock();
        {
            std::scoped_lock lock{provider_->mutex};
            --provider_->activeCalls;
        }
        provider_->drained.notify_all();
        --ExecutingProviders.size;
        ExecutingProviders.providers[ExecutingProviders.size] = nullptr;
        provider_.reset();
        if (registry != nullptr)
            static_cast<void>(ProgressRetirements(registry, std::chrono::steady_clock::now(), false));
    }

    namespace Detail {
        Result<BackendServiceCallAdmission> BeginBackendServiceCall(const std::shared_ptr<BackendServiceProviderState> &provider,
                                                                    const CancellationToken &caller) {
            std::scoped_lock lock{provider->mutex};
            const std::array cancellationFacts{caller.IsCancellationRequested(), provider->cancellation.Token().IsCancellationRequested()};
            const std::array failures{
                !provider->registered.load(std::memory_order_acquire),
                !CanFinalizeHere(*provider),
                std::ranges::find(cancellationFacts, true) != cancellationFacts.end(),
                ExecutingProviders.size >= ExecutingProviders.providers.size(),
            };
            const std::array errors{
                &ExtensionErrors::BackendServiceUnavailable,
                &ExtensionErrors::BackendServiceThreadViolation,
                &ExtensionErrors::BackendServiceCancelled,
                &ExtensionErrors::BackendServiceCapacityExceeded,
            };
            if (const auto failure = std::ranges::find(failures, true); failure != failures.end()) {
                const auto index = static_cast<std::size_t>(failure - failures.begin());
                if (errors[index] == &ExtensionErrors::BackendServiceCancelled)
                    return Result<BackendServiceCallAdmission>::Failure(BackendServiceCancellationError(provider->descriptor));
                return Result<BackendServiceCallAdmission>::Failure(MakeError(*errors[index]));
            }
            ++provider->activeCalls;
            return Result<BackendServiceCallAdmission>::Success(
                BackendServiceCallAdmission{provider,
                                            BackendServiceCallContext{provider->descriptor, caller, provider->cancellation.Token()}});
        }

        void *BackendServiceObject(const std::shared_ptr<BackendServiceProviderState> &provider) noexcept {
            return provider->implementation.service.get();
        }

        Error AttributeBackendServiceError(const BackendServiceDescriptor &provider, Error cause) {
            const std::string detail = std::format("Backend service failed: {}@{} ({}).", provider.provider.providerId,
                                                   provider.provider.generation, provider.serviceId.value);
            return cause.code.Value().empty() ? MakeError(ExtensionErrors::BackendServiceInvocationFailed, detail)
                                              : WrapError(ExtensionErrors::BackendServiceInvocationFailed, std::move(cause), detail);
        }

        Error BackendServiceCancellationError(const BackendServiceDescriptor &provider) {
            return MakeError(ExtensionErrors::BackendServiceCancelled,
                             std::format("Backend service call cancelled: {}@{} ({}).", provider.provider.providerId,
                                         provider.provider.generation, provider.serviceId.value));
        }

        Error BackendServiceCancellationError(const ApplicationCapabilityProviderDescriptor &provider) {
            return MakeError(ExtensionErrors::BackendServiceCancelled,
                             std::format("Backend service call cancelled: {}@{}.", provider.provider.providerId,
                                         provider.provider.generation));
        }

        Error BackendServiceAuthorityUnavailableError() {
            return MakeError(ExtensionErrors::BackendServiceUnavailable,
                             "The application capability/provider authority is unavailable or already consumed.");
        }

        Error BackendServiceConsumedCallError() {
            return MakeError(ExtensionErrors::BackendServiceUnavailable, "The one-shot backend service call was already consumed.");
        }

        Error AttributeBackendServiceImportError(const ResolvedExtensionServiceImport &binding, Error error, const std::string_view reason,
                                                 const ApplicationCapabilityProviderDescriptor *provider) {
            const std::string providerModule = binding.ProviderModuleId().empty() ? "<unavailable>" : binding.ProviderModuleId();
            std::string detail =
                std::format("Extension import '{}': consumer='{}/{}', service='{}', declared-provider-module='{}', declared-version='{}'.",
                            binding.ImportId(), binding.ConsumerExtensionId(), binding.ConsumerModuleId(), binding.ServiceId(),
                            providerModule, binding.ProviderVersion());
            if (provider != nullptr) {
                detail += std::format(" Admitted provider='{} -> {}@{}', version={}.{}.{}.", provider->provider.moduleId,
                                      provider->provider.providerId, provider->provider.generation, provider->version.major,
                                      provider->version.minor, provider->version.patch);
            }
            if (!reason.empty())
                detail += " " + std::string{reason};
            if (!error.message.empty())
                detail += " " + error.message;
            error.message = std::move(detail);
            return error;
        }

        Error BackendServiceImportError(const ResolvedExtensionServiceImport &binding, const ErrorCodeDescriptor &descriptor,
                                        const std::string_view reason, const ApplicationCapabilityProviderDescriptor *provider) {
            return AttributeBackendServiceImportError(binding, MakeError(descriptor), reason, provider);
        }
    }  // namespace Detail

    BackendServiceRegistration::BackendServiceRegistration(std::shared_ptr<BackendServiceRegistryState> registry,
                                                           std::shared_ptr<BackendServiceProviderState> provider) noexcept
        : registry_(std::move(registry)), provider_(std::move(provider)) {}

    BackendServiceRegistration::~BackendServiceRegistration() {
        static_cast<void>(Reset());
    }

    /** @copydoc BackendServiceRegistration::Reset */
    BackendServiceRetirementDisposition BackendServiceRegistration::Reset() noexcept {
        if (provider_ == nullptr)
            return BackendServiceRetirementDisposition::ShutdownComplete;
        const BackendServiceRetirementDisposition disposition = RetireProvider(registry_, provider_);
        provider_.reset();
        registry_.reset();
        return disposition;
    }

    /** @copydoc BackendServiceRegistration::IsRegistered */
    bool BackendServiceRegistration::IsRegistered() const noexcept {
        return provider_ != nullptr && provider_->registered.load(std::memory_order_acquire);
    }

    namespace {
        /** @brief Preserves the original service code owner while reporting its extension-level lifetime. */
        struct RetirementBackendCode final {
            BackendServiceCodeLease previous;
            std::shared_ptr<ExtensionExecutableLease> retirement;
        };

        /** @brief Connects actual backend cancellation/drain/quarantine to package retirement. */
        class BackendRetirementPublication final : public IExtensionRetirementContribution {
        public:
            BackendRetirementPublication(BackendServiceRegistration registration, std::shared_ptr<ExtensionRetirement> retirement)
                : registration_(std::move(registration)), retirement_(std::move(retirement)) {}

            void Revoke() noexcept override {
                if (registration_.Reset() == BackendServiceRetirementDisposition::RestartRequired)
                    retirement_->RequireRestart();
            }

        private:
            BackendServiceRegistration registration_;
            std::shared_ptr<ExtensionRetirement> retirement_;
        };
    }  // namespace

    /** @copydoc BackendServiceRegistration::AttachRetirement */
    bool BackendServiceRegistration::AttachRetirement(const std::shared_ptr<ExtensionRetirement> &retirement) const {
        if (!retirement || !IsRegistered())
            return false;
        if (std::this_thread::get_id() != provider_->ownerThread)
            return false;
        const auto &descriptor = provider_->descriptor;
        std::shared_ptr<void> service;
        BackendServiceCodeLease previous = BackendServiceCodeLease::Retain(std::shared_ptr<const void>{});
        {
            std::scoped_lock lock{provider_->mutex};
            if (provider_->retirementAttached || provider_->activeCalls != 0 ||
                provider_->lifecycle != BackendServiceProviderLifecycle::Active)
                return false;
            service = provider_->implementation.service;
            previous = provider_->implementation.codeLease;
        }
        auto lease = retirement->Acquire(descriptor.provider.moduleId, ExtensionLeaseKind::HostService, descriptor.serviceId.value,
                                         std::move(service));
        if (!lease)
            return false;
        auto code = std::make_shared<RetirementBackendCode>(std::move(previous), std::move(lease));
        auto publication = std::make_shared<BackendRetirementPublication>(BackendServiceRegistration{registry_, provider_}, retirement);
        {
            std::scoped_lock lock{provider_->mutex};
            if (provider_->activeCalls != 0 || provider_->lifecycle != BackendServiceProviderLifecycle::Active)
                return false;
            provider_->implementation.codeLease = BackendServiceCodeLease::Retain(std::move(code));
            provider_->retirementAttached = true;
        }
        return retirement->RegisterContribution(descriptor.provider.moduleId, std::move(publication));
    }

    BackendServiceImportBinding::BackendServiceImportBinding(std::shared_ptr<BackendServiceProviderState> provider,
                                                             ApplicationCapabilityProviderLease authority,
                                                             ResolvedExtensionServiceImport binding) noexcept
        : provider_(std::move(provider)), authority_(std::move(authority)), import_(std::move(binding)) {}

    BackendServiceRegistry::BackendServiceRegistry(BackendServiceRegistryConfig config)
        : state_(std::make_shared<BackendServiceRegistryState>()) {
        if (config.drainDeadline <= std::chrono::milliseconds::zero())
            config.drainDeadline = std::chrono::milliseconds{1};
        state_->config = config;
        state_->providers.reserve(MaximumServices);
        state_->retiredProviders.reserve(MaximumServices);
    }

    BackendServiceRegistry::~BackendServiceRegistry() {
        if (BeginShutdown() != BackendServiceRetirementDisposition::ShutdownComplete)
            QuarantineForRestart(state_);
    }

    /** @copydoc BackendServiceRegistry::Register */
    Result<BackendServiceRegistration> BackendServiceRegistry::RegisterErased(BackendServiceDescriptor descriptor,
                                                                              Detail::BackendServiceOwnedImplementation implementation,
                                                                              const void *typeTag, const ShutdownFunction shutdown) {
        if (const std::array invalid{!ValidDescriptor(descriptor), implementation.service == nullptr,
                                     implementation.codeLease.owner_ == nullptr, typeTag == nullptr, shutdown == nullptr};
            std::ranges::find(invalid, true) != invalid.end())
            return Result<BackendServiceRegistration>::Failure(MakeError(ExtensionErrors::BackendServiceInvalid));
        std::scoped_lock lock{state_->mutex};
        const auto duplicate = std::ranges::find_if(state_->providers, [&descriptor](const auto &provider) {
            return std::tie(provider->descriptor.serviceId, provider->descriptor.version) ==
                   std::tie(descriptor.serviceId, descriptor.version);
        });
        const auto retiredDuplicate = std::ranges::find_if(state_->retiredProviders, [&descriptor](const auto &provider) {
            return std::tie(provider->descriptor.serviceId, provider->descriptor.version) ==
                   std::tie(descriptor.serviceId, descriptor.version);
        });
        const std::array duplicateFacts{duplicate != state_->providers.end(), retiredDuplicate != state_->retiredProviders.end()};
        const std::array failures{state_->shutdown, std::ranges::find(duplicateFacts, true) != duplicateFacts.end(),
                                  state_->providers.size() + state_->retiredProviders.size() >= MaximumServices};
        constexpr std::array errors{&ExtensionErrors::BackendServiceShutdown, &ExtensionErrors::BackendServiceDuplicate,
                                    &ExtensionErrors::BackendServiceCapacityExceeded};
        if (const auto failure = std::ranges::find(failures, true); failure != failures.end())
            return Result<BackendServiceRegistration>::Failure(MakeError(*errors[static_cast<std::size_t>(failure - failures.begin())]));

        auto provider = std::make_shared<BackendServiceProviderState>();
        provider->descriptor = std::move(descriptor);
        provider->implementation = std::move(implementation);
        provider->typeTag = typeTag;
        provider->shutdown = shutdown;
        provider->ownerThread = std::this_thread::get_id();
        provider->registry = state_;
        provider->registrationSequence = ++state_->nextRegistrationSequence;
        state_->providers.push_back(provider);
        return Result<BackendServiceRegistration>::Success(BackendServiceRegistration{state_, std::move(provider)});
    }

    /** @copydoc BackendServiceRegistry::BindImport */
    Result<BackendServiceImportBinding> BackendServiceRegistry::BindImport(ApplicationCapabilityProviderLease authority,
                                                                           const ResolvedExtensionServiceImport &binding) const {
        if (binding.Status() != ExtensionServiceImportStatus::Bound) {
            const ErrorCodeDescriptor &error = binding.Status() == ExtensionServiceImportStatus::Unavailable
                                                   ? ExtensionErrors::BackendServiceUnavailable
                                                   : ExtensionErrors::BackendServiceContractMismatch;
            return Result<BackendServiceImportBinding>::Failure(
                Detail::BackendServiceImportError(binding, error, "The declared import did not resolve to a provider."));
        }
        if (!authority.IsUsable()) {
            return Result<BackendServiceImportBinding>::Failure(
                Detail::BackendServiceImportError(binding, ExtensionErrors::BackendServiceUnavailable,
                                                  "The consumer admission or capability publication is no longer active."));
        }

        const ApplicationCapabilityProviderDescriptor &provider = authority.Descriptor();
        if (const ExtensionActivationIdentity &consumer = authority.Consumer();
            consumer.ExtensionId() != binding.ConsumerExtensionId() || consumer.ModuleId() != binding.ConsumerModuleId()) {
            return Result<BackendServiceImportBinding>::Failure(
                Detail::BackendServiceImportError(binding, ExtensionErrors::BackendServiceContractMismatch,
                                                  std::format("Admitted consumer is '{}/{}' at generation {}.", consumer.ExtensionId(),
                                                              consumer.ModuleId(), consumer.Generation()),
                                                  &provider));
        }

        if (provider.provider.moduleId != binding.ProviderModuleId()) {
            return Result<BackendServiceImportBinding>::Failure(
                Detail::BackendServiceImportError(binding, ExtensionErrors::BackendServiceContractMismatch,
                                                  std::format("Admitted provider mapping is '{} -> {}' at generation {}.",
                                                              provider.provider.moduleId, provider.provider.providerId,
                                                              provider.provider.generation),
                                                  &provider));
        }
        if (const std::optional<ApplicationCapabilityVersion> declaredVersion = ParseProviderVersion(binding.ProviderVersion());
            !declaredVersion.has_value() || *declaredVersion != provider.version) {
            return Result<BackendServiceImportBinding>::Failure(
                Detail::BackendServiceImportError(binding, ExtensionErrors::BackendServiceContractMismatch,
                                                  std::format("Admitted provider version is {}.{}.{} at generation {}.",
                                                              provider.version.major, provider.version.minor, provider.version.patch,
                                                              provider.provider.generation),
                                                  &provider));
        }

        auto resolved =
            ResolveErased(provider, BackendServiceId{binding.ServiceId()}, BackendServiceContractId{binding.ContractId()}, nullptr);
        if (resolved.HasError()) {
            return Result<BackendServiceImportBinding>::Failure(
                Detail::AttributeBackendServiceImportError(binding, resolved.ErrorValue(), "Live provider binding failed.", &provider));
        }
        return Result<BackendServiceImportBinding>::Success(
            BackendServiceImportBinding{std::move(resolved).Value(), std::move(authority), binding});
    }

    /** @copydoc BackendServiceRegistry::Resolve */
    Result<std::shared_ptr<BackendServiceProviderState>> BackendServiceRegistry::ResolveErased(
        const ApplicationCapabilityProviderDescriptor &authority, const BackendServiceId &serviceId,
        const BackendServiceContractId &contractId, const void *typeTag) const {
        if (!Detail::IsCanonicalExtensionAuthorityId(serviceId.value) || !Detail::IsCanonicalExtensionAuthorityId(contractId.value))
            return Result<std::shared_ptr<BackendServiceProviderState>>::Failure(MakeError(ExtensionErrors::BackendServiceInvalid));
        std::scoped_lock lock{state_->mutex};
        if (state_->shutdown)
            return Result<std::shared_ptr<BackendServiceProviderState>>::Failure(MakeError(ExtensionErrors::BackendServiceShutdown));
        const auto service = std::ranges::find(state_->providers, serviceId, [](const auto &provider) {
            return provider->descriptor.serviceId;
        });
        const auto authorityMatch = std::ranges::find_if(state_->providers, [&serviceId, &authority](const auto &provider) {
            return std::tie(provider->descriptor.serviceId, provider->descriptor.capability, provider->descriptor.version,
                            provider->descriptor.provider) ==
                   std::tie(serviceId, authority.capability, authority.version, authority.provider);
        });
        if (service == state_->providers.end())
            return Result<std::shared_ptr<BackendServiceProviderState>>::Failure(MakeError(ExtensionErrors::BackendServiceUnavailable));
        if (authorityMatch == state_->providers.end() || (*authorityMatch)->descriptor.contractId != contractId)
            return Result<std::shared_ptr<BackendServiceProviderState>>::Failure(
                MakeError(ExtensionErrors::BackendServiceContractMismatch));
        if (typeTag != nullptr && (*authorityMatch)->typeTag != typeTag)
            return Result<std::shared_ptr<BackendServiceProviderState>>::Failure(MakeError(ExtensionErrors::BackendServiceTypeMismatch));
        return Result<std::shared_ptr<BackendServiceProviderState>>::Success(*authorityMatch);
    }

    Result<void> BackendServiceRegistry::ValidateImportedBinding(const BackendServiceImportBinding &binding, const void *typeTag) const {
        if (binding.provider_ == nullptr || typeTag == nullptr) {
            return Result<void>::Failure(Detail::BackendServiceImportError(binding.import_, ExtensionErrors::BackendServiceInvalid,
                                                                           "The host-created import binding is malformed or consumed."));
        }
        if (!binding.authority_.IsUsable()) {
            return Result<void>::Failure(
                Detail::BackendServiceImportError(binding.import_, ExtensionErrors::BackendServiceUnavailable,
                                                  "The bound consumer admission or capability publication was revoked.",
                                                  &binding.authority_.Descriptor()));
        }

        std::scoped_lock lock{state_->mutex};
        if (state_->shutdown) {
            return Result<void>::Failure(Detail::BackendServiceImportError(binding.import_, ExtensionErrors::BackendServiceShutdown,
                                                                           "The backend service registry is shutting down.",
                                                                           &binding.authority_.Descriptor()));
        }
        if (const bool live = binding.provider_->registered.load(std::memory_order_acquire) &&
                              std::ranges::find(state_->providers, binding.provider_) != state_->providers.end();
            !live) {
            return Result<void>::Failure(
                Detail::BackendServiceImportError(binding.import_, ExtensionErrors::BackendServiceUnavailable,
                                                  "The exact provider generation selected by the binding is no longer published.",
                                                  &binding.authority_.Descriptor()));
        }
        if (binding.provider_->typeTag != typeTag) {
            return Result<void>::Failure(
                Detail::BackendServiceImportError(binding.import_, ExtensionErrors::BackendServiceTypeMismatch,
                                                  "The bound provider does not implement the requested C++ adapter type.",
                                                  &binding.authority_.Descriptor()));
        }
        return Result<void>::Success();
    }

    /** @copydoc BackendServiceRegistry::BeginShutdown */
    BackendServiceRetirementDisposition BackendServiceRegistry::BeginShutdown() noexcept {
        std::vector<std::shared_ptr<BackendServiceProviderState>> providers;
        bool stagesShutdown{};
        {
            std::scoped_lock lock{state_->mutex};
            if (!state_->shutdown) {
                state_->stagingShutdown.store(true, std::memory_order_release);
                state_->shutdown = true;
                providers = std::move(state_->providers);
                stagesShutdown = true;
            }
        }
        const auto deadline = std::chrono::steady_clock::now() + state_->config.drainDeadline;
        for (auto iterator = providers.rbegin(); iterator != providers.rend(); ++iterator) {
            RequestRetirement(*iterator);
            std::scoped_lock lock{state_->mutex};
            EnqueueRetirement(*state_, *iterator);
        }
        if (stagesShutdown)
            state_->stagingShutdown.store(false, std::memory_order_release);
        return ProgressRetirements(state_, deadline, true);
    }

    /** @copydoc BackendServiceRegistry::FinalizeRetiredOnOwnerThread */
    BackendServiceRetirementDisposition BackendServiceRegistry::FinalizeRetiredOnOwnerThread() noexcept {
        return ProgressRetirements(state_, std::chrono::steady_clock::now(), false);
    }

    /** @copydoc BackendServiceRegistry::IsShutdown */
    bool BackendServiceRegistry::IsShutdown() const noexcept {
        std::scoped_lock lock{state_->mutex};
        return state_->shutdown;
    }
}  // namespace Horo::Extensions
