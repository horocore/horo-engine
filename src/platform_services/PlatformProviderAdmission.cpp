#include "Horo/PlatformServices/PlatformProviderAdmission.h"

#include "Horo/Extensions/ExtensionErrors.h"
#include "Horo/PlatformServices/PlatformRequestErrors.h"
#include "Horo/PlatformServices/PlatformServiceErrors.h"
#include "Horo/PlatformServices/PlatformServicesFrontend.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::PlatformServices {
    namespace {
        constexpr std::string_view CapabilityId = "platform.services.provider";
        constexpr std::string_view FactoryServiceId = "platform.services.provider.factory";
        constexpr std::size_t MaximumCandidates = 64;

        [[nodiscard]] bool CanonicalId(const std::string_view value) noexcept {
            if (value.empty() || value.size() > 128 || value.front() < 'a' || value.front() > 'z' || value.back() == '.')
                return false;
            return std::ranges::all_of(value, [](const char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-';
            }) && value.find("..") == std::string_view::npos;
        }

        [[nodiscard]] constexpr std::uint32_t PlatformBit(const Extensions::ExtensionHostPlatform platform) noexcept {
            return 1U << static_cast<std::uint8_t>(platform);
        }

        [[nodiscard]] constexpr std::uint32_t ProfileBit(const PlatformServicesHostProfile profile) noexcept {
            return 1U << static_cast<std::uint8_t>(profile);
        }

        [[nodiscard]] bool ValidClaim(const Extensions::ExtensionPlatformProviderCandidate &candidate,
                                      const Extensions::ExtensionHostPlatform platform,
                                      const PlatformServicesHostProfile profile) noexcept {
            constexpr std::uint32_t ServiceMask = (1U << static_cast<std::size_t>(PlatformServiceKind::Count)) - 1U;
            return platform < Extensions::ExtensionHostPlatform::Count && profile <= PlatformServicesHostProfile::Certification &&
                   CanonicalId(candidate.extensionId) && CanonicalId(candidate.moduleId) && CanonicalId(candidate.providerKey) &&
                   candidate.providerId != 0 && candidate.platformMask != 0 && (candidate.platformMask & ~0x7U) == 0 &&
                   (candidate.platformMask & PlatformBit(platform)) != 0 && candidate.profileMask != 0 &&
                   (candidate.profileMask & ~0xfU) == 0 && (candidate.profileMask & ProfileBit(profile)) != 0 &&
                   candidate.serviceMask != 0 && (candidate.serviceMask & ~ServiceMask) == 0 &&
                   candidate.interfaceMajor == PlatformServicesBackendInterfaceMajor &&
                   candidate.interfaceMinor == PlatformServicesBackendInterfaceMinor && candidate.contractMajor != 0 &&
                   candidate.contractMajor <= std::numeric_limits<std::uint16_t>::max() &&
                   candidate.contractMinor <= std::numeric_limits<std::uint16_t>::max() &&
                   candidate.contractPatch <= std::numeric_limits<std::uint16_t>::max() && candidate.permissions.size() <= 32 &&
                   candidate.createCandidate != nullptr && candidate.retireCandidate != nullptr && candidate.destroyCandidate != nullptr &&
                   candidate.moduleCodeLease != nullptr;
        }

        [[nodiscard]] Error Invalid(const std::string_view reason) {
            return MakeError(Extensions::ExtensionErrors::ContributionRejected, std::string{reason});
        }

        [[nodiscard]] PlatformProviderContributionDescriptor Describe(const Extensions::ExtensionPlatformProviderCandidate &candidate,
                                                                      const std::uint64_t generation) {
            PlatformProviderContributionDescriptor descriptor;
            descriptor.owner = {candidate.moduleId, candidate.providerKey, generation};
            descriptor.providerKey = candidate.providerKey;
            descriptor.provider = {candidate.providerId};
            for (std::size_t index = 0; index < descriptor.platforms.size(); ++index)
                descriptor.platforms[index] = (candidate.platformMask & (1U << index)) != 0;
            descriptor.profiles = static_cast<PlatformServicesHostProfileMask>(candidate.profileMask);
            for (std::size_t index = 0; index < descriptor.services.size(); ++index)
                descriptor.services[index] = (candidate.serviceMask & (1U << index)) != 0;
            for (const auto &permission : candidate.permissions)
                descriptor.permissions.push_back({permission});
            descriptor.interfaceVersion = {static_cast<std::uint16_t>(candidate.interfaceMajor),
                                           static_cast<std::uint16_t>(candidate.interfaceMinor)};
            descriptor.contractVersion = {static_cast<std::uint16_t>(candidate.contractMajor),
                                          static_cast<std::uint16_t>(candidate.contractMinor),
                                          static_cast<std::uint16_t>(candidate.contractPatch)};
            return descriptor;
        }
    }  // namespace

    struct PlatformProviderCandidateState final {
        std::mutex mutex;  // Protects admission, lease count and native retirement state; callbacks run outside it.
        std::weak_ptr<PlatformProviderRetirementState> retirement;
        std::shared_ptr<void> moduleCodeLease;
        PlatformProviderContributionDescriptor descriptor;
        PlatformProviderId provider;
        void *candidate{};
        HoroPlatformProviderRetireFunc retire{};
        HoroPlatformProviderDestroyFunc destroy{};
        HoroPlatformProviderOperations operations{};
        std::thread::id ownerThread;
        std::size_t leaseCount{};
        bool backendActive{};
        bool revoked{};
        bool retiring{};
        bool retired{};
    };

    struct PlatformProviderRetirementState final {
        std::mutex mutex;  // Protects bounded candidate list and publication closure; never held during native calls.
        std::vector<std::shared_ptr<PlatformProviderCandidateState>> candidates;
        std::shared_ptr<PlatformProviderRetirementState> quarantine;
        std::thread::id ownerThread;
        std::atomic_bool closed{};
        std::atomic_bool restartRequired{};
    };

    namespace {
        [[nodiscard]] PlatformProviderRetirementDisposition FinalizeCandidates(
            const std::shared_ptr<PlatformProviderRetirementState> &retirement) noexcept;

        void ReleaseLease(const std::shared_ptr<PlatformProviderCandidateState> &state, const bool backend) noexcept {
            if (state == nullptr)
                return;
            std::shared_ptr<PlatformProviderRetirementState> retirement;
            {
                std::scoped_lock lock{state->mutex};
                if (backend) {
                    state->backendActive = false;
                    state->revoked = true;
                }
                if (state->leaseCount != 0)
                    --state->leaseCount;
                retirement = state->retirement.lock();
            }
            if (retirement != nullptr && retirement->ownerThread == std::this_thread::get_id()) {
                // The explicit owner-thread pass performs native callbacks without holding the lease mutex.
                // A caller may also retry later when the module reports BUSY.
                static_cast<void>(FinalizeCandidates(retirement));
            }
        }

        [[nodiscard]] PlatformProviderRetirementDisposition FinalizeCandidates(
            const std::shared_ptr<PlatformProviderRetirementState> &retirement) noexcept {
            if (retirement->ownerThread != std::this_thread::get_id())
                return PlatformProviderRetirementDisposition::OwnerThreadRequired;
            if (retirement->restartRequired.load(std::memory_order_acquire))
                return PlatformProviderRetirementDisposition::RestartRequired;
            std::array<std::shared_ptr<PlatformProviderCandidateState>, MaximumCandidates> snapshot;
            std::size_t count{};
            {
                std::scoped_lock lock{retirement->mutex};
                count = retirement->candidates.size();
                std::copy(retirement->candidates.begin(), retirement->candidates.end(), snapshot.begin());
            }
            bool busy = false;
            for (std::size_t index = 0; index < count; ++index) {
                const auto &state = snapshot[index];
                {
                    std::scoped_lock lock{state->mutex};
                    if (state->retired)
                        continue;
                    if (!state->revoked)
                        continue;
                    if (state->leaseCount != 0 || state->retiring) {
                        busy = true;
                        continue;
                    }
                    state->retiring = true;
                }
                HoroExtensionStatus status = HORO_EXTENSION_ERROR_BUSY;
                try {
                    status = state->retire(state->candidate);
                    if (status == HORO_EXTENSION_SUCCESS)
                        state->destroy(state->candidate);
                } catch (...) {
                    status = HORO_EXTENSION_ERROR_INIT_FAILED;
                }
                {
                    std::scoped_lock lock{state->mutex};
                    state->retiring = false;
                    if (status == HORO_EXTENSION_SUCCESS) {
                        state->retired = true;
                        state->candidate = nullptr;
                    } else if (status == HORO_EXTENSION_ERROR_BUSY) {
                        busy = true;
                    } else {
                        retirement->restartRequired.store(true, std::memory_order_release);
                    }
                }
            }
            {
                std::scoped_lock lock{retirement->mutex};
                std::erase_if(retirement->candidates, [](const auto &state) {
                    return state->retired;
                });
                busy = busy || !retirement->candidates.empty();
                if (retirement->candidates.empty())
                    retirement->quarantine.reset();
            }
            if (retirement->restartRequired.load(std::memory_order_acquire))
                return PlatformProviderRetirementDisposition::RestartRequired;
            return busy ? PlatformProviderRetirementDisposition::Busy : PlatformProviderRetirementDisposition::Complete;
        }

        void QuarantinePending(const std::shared_ptr<PlatformProviderRetirementState> &retirement) noexcept {
            std::scoped_lock lock{retirement->mutex};
            if (retirement->candidates.empty())
                return;
            // Self-retain only while a candidate still needs native retirement; finalization breaks the cycle.
            retirement->quarantine = retirement;
        }
    }  // namespace

    PlatformProviderRequestLease::PlatformProviderRequestLease(std::shared_ptr<PlatformProviderCandidateState> state) noexcept
        : state_(std::move(state)) {}

    PlatformProviderRequestLease::~PlatformProviderRequestLease() {
        ReleaseLease(state_, false);
    }

    PlatformProviderRequestLease::PlatformProviderRequestLease(PlatformProviderRequestLease &&other) noexcept
        : state_(std::exchange(other.state_, {})) {}

    PlatformProviderRequestLease &PlatformProviderRequestLease::operator=(PlatformProviderRequestLease &&other) noexcept {
        if (this != &other) {
            ReleaseLease(state_, false);
            state_ = std::exchange(other.state_, {});
        }
        return *this;
    }

    PlatformProviderCandidateLease::PlatformProviderCandidateLease(std::shared_ptr<PlatformProviderCandidateState> state) noexcept
        : state_(std::move(state)) {}

    PlatformProviderCandidateLease::~PlatformProviderCandidateLease() {
        ReleaseLease(state_, true);
    }

    PlatformProviderCandidateLease::PlatformProviderCandidateLease(PlatformProviderCandidateLease &&other) noexcept
        : state_(std::exchange(other.state_, {})) {}

    PlatformProviderCandidateLease &PlatformProviderCandidateLease::operator=(PlatformProviderCandidateLease &&other) noexcept {
        if (this != &other) {
            ReleaseLease(state_, true);
            state_ = std::exchange(other.state_, {});
        }
        return *this;
    }

    /** @copydoc PlatformProviderCandidateLease::AcquireRequestLease */
    Result<PlatformProviderRequestLease> PlatformProviderCandidateLease::AcquireRequestLease() const {
        if (state_ == nullptr)
            return Result<PlatformProviderRequestLease>::Failure(Invalid("Provider candidate lease is empty."));
        std::scoped_lock lock{state_->mutex};
        if (!state_->backendActive || state_->revoked)
            return Result<PlatformProviderRequestLease>::Failure(Invalid("Provider generation is revoked."));
        ++state_->leaseCount;
        return Result<PlatformProviderRequestLease>::Success(PlatformProviderRequestLease{state_});
    }

    /** @copydoc PlatformProviderCandidateLease::Provider */
    PlatformProviderId PlatformProviderCandidateLease::Provider() const noexcept {
        return state_ ? state_->provider : PlatformProviderId{};
    }

    /** @copydoc PlatformProviderCandidateLease::Descriptor */
    const PlatformProviderContributionDescriptor &PlatformProviderCandidateLease::Descriptor() const noexcept {
        return state_->descriptor;
    }

    class PlatformProviderFactory final {
    public:
        PlatformProviderFactory(Extensions::ExtensionPlatformProviderCandidate candidate, PlatformProviderContributionDescriptor descriptor,
                                std::shared_ptr<PlatformProviderRetirementState> retirement)
            : candidate_(std::move(candidate)), descriptor_(std::move(descriptor)), retirement_(std::move(retirement)) {}

        [[nodiscard]] Result<PlatformProviderCandidateLease> Create(const std::uint8_t &,
                                                                    const Extensions::BackendServiceCallContext &context) {
            if (retirement_->closed.load(std::memory_order_acquire) || context.IsCancellationRequested())
                return Result<PlatformProviderCandidateLease>::Failure(Invalid("Provider factory admission is closed."));
            {
                std::scoped_lock lock{retirement_->mutex};
                if (retirement_->candidates.size() >= MaximumCandidates)
                    return Result<PlatformProviderCandidateLease>::Failure(Invalid("Provider candidate capacity is full."));
            }
            auto state = std::make_shared<PlatformProviderCandidateState>();
            state->retirement = retirement_;
            state->moduleCodeLease = candidate_.moduleCodeLease;
            state->descriptor = descriptor_;
            state->provider = PlatformProviderId{candidate_.providerId};
            state->retire = candidate_.retireCandidate;
            state->destroy = candidate_.destroyCandidate;
            state->operations = candidate_.operations;
            state->ownerThread = std::this_thread::get_id();
            void *nativeCandidate = nullptr;
            HoroExtensionStatus status = HORO_EXTENSION_ERROR_INIT_FAILED;
            try {
                status = candidate_.createCandidate(candidate_.factoryContext, &nativeCandidate);
            } catch (...) {
            }
            state->candidate = nativeCandidate;
            if (nativeCandidate != nullptr) {
                std::scoped_lock lock{retirement_->mutex};
                state->backendActive = status == HORO_EXTENSION_SUCCESS && !retirement_->closed.load(std::memory_order_acquire);
                state->leaseCount = state->backendActive ? 1U : 0U;
                state->revoked = !state->backendActive;
                retirement_->candidates.push_back(state);
            }
            if (!state->backendActive) {
                static_cast<void>(FinalizeCandidates(retirement_));
                return Result<PlatformProviderCandidateLease>::Failure(Invalid("Provider candidate creation failed."));
            }
            return Result<PlatformProviderCandidateLease>::Success(PlatformProviderCandidateLease{std::move(state)});
        }

        void Shutdown() noexcept {
            retirement_->closed.store(true, std::memory_order_release);
        }

    private:
        Extensions::ExtensionPlatformProviderCandidate candidate_;
        PlatformProviderContributionDescriptor descriptor_;
        std::shared_ptr<PlatformProviderRetirementState> retirement_;
    };

    class PlatformProviderPublication final : public Extensions::IExtensionPlatformProviderPublication {
    public:
        PlatformProviderPublication(Extensions::BackendServiceRegistration factory, Extensions::ExtensionCapabilityAdmission admission,
                                    std::shared_ptr<PlatformProviderRetirementState> retirement)
            : factory_(std::move(factory)), admission_(std::move(admission)), retirement_(std::move(retirement)) {}

        ~PlatformProviderPublication() override {
            Revoke();
        }

        void Publish(Extensions::ApplicationCapabilityProviderRegistration capability) noexcept {
            capability_.emplace(std::move(capability));
        }

        void Revoke() noexcept override {
            if (revoked_.exchange(true, std::memory_order_acq_rel))
                return;
            if (capability_)
                capability_->Reset();
            admission_.Revoke();
            retirement_->closed.store(true, std::memory_order_release);
            {
                std::scoped_lock lock{retirement_->mutex};
                for (const auto &state : retirement_->candidates) {
                    std::scoped_lock candidateLock{state->mutex};
                    state->revoked = true;
                    state->backendActive = false;
                }
            }
            const auto disposition = factory_.Reset();
            if (disposition == Extensions::BackendServiceRetirementDisposition::RestartRequired)
                retirement_->restartRequired.store(true, std::memory_order_release);
            static_cast<void>(FinalizeCandidates(retirement_));
        }

    private:
        std::optional<Extensions::ApplicationCapabilityProviderRegistration> capability_;
        Extensions::BackendServiceRegistration factory_;
        Extensions::ExtensionCapabilityAdmission admission_;
        std::shared_ptr<PlatformProviderRetirementState> retirement_;
        std::atomic_bool revoked_{};
    };

    PlatformProviderAdmission::PlatformProviderAdmission(Extensions::ApplicationCapabilityRegistry &capabilities,
                                                         Extensions::BackendServiceRegistry &services,
                                                         Extensions::ExtensionAdmissionPolicy policy,
                                                         const Extensions::ExtensionHostPlatform platform,
                                                         const PlatformServicesHostProfile profile)
        : capabilities_(capabilities), services_(services), policy_(std::move(policy)), platform_(platform), profile_(profile),
          retirements_{} {
        retirements_.reserve(Extensions::ApplicationCapabilityRegistry::MaximumProviders);
    }

    PlatformProviderAdmission::~PlatformProviderAdmission() {
        // The service registry owns any deferred factory shutdown after a foreign-thread revoke.
        static_cast<void>(services_.FinalizeRetiredOnOwnerThread());
        std::array<std::shared_ptr<PlatformProviderRetirementState>, Extensions::ApplicationCapabilityRegistry::MaximumProviders> snapshot;
        std::size_t count{};
        {
            std::scoped_lock lock{retirementsMutex_};
            for (const auto &state : retirements_)
                snapshot[count++] = state;
        }
        for (std::size_t index = 0; index < count; ++index) {
            snapshot[index]->closed.store(true, std::memory_order_release);
            static_cast<void>(FinalizeCandidates(snapshot[index]));
            QuarantinePending(snapshot[index]);
        }
    }

    /** @copydoc PlatformProviderAdmission::Commit */
    Result<Extensions::ExtensionPlatformProviderPublication> PlatformProviderAdmission::Commit(
        Extensions::ExtensionPlatformProviderCandidate candidate) {
        if (nextGeneration_ == 0 || !ValidClaim(candidate, platform_, profile_))
            return Result<Extensions::ExtensionPlatformProviderPublication>::Failure(
                Invalid("Provider identity, platform, profile, service, version or factory claim is invalid."));
        PlatformProviderContributionDescriptor descriptor = Describe(candidate, nextGeneration_);
        Extensions::ExtensionAdmissionRequest request;
        request.extensionId = candidate.extensionId;
        request.moduleId = candidate.moduleId;
        request.activationGeneration = nextGeneration_;
        Extensions::ExtensionCapabilityRequest permissionRequest;
        permissionRequest.capability.value = CapabilityId;
        for (const auto &permission : candidate.permissions)
            permissionRequest.requiredPermissions.push_back({permission});
        request.capabilities.push_back(std::move(permissionRequest));
        auto admission = Extensions::ExtensionCapabilityAdmission::Evaluate(request, policy_);
        if (admission.HasError())
            return Result<Extensions::ExtensionPlatformProviderPublication>::Failure(admission.ErrorValue());

        const Extensions::ApplicationCapabilityVersion version = descriptor.contractVersion;
        const Extensions::ApplicationCapabilityProviderIdentity identity = descriptor.owner;
        auto moduleCodeLease = candidate.moduleCodeLease;
        auto retirement = std::make_shared<PlatformProviderRetirementState>();
        retirement->ownerThread = std::this_thread::get_id();
        retirement->candidates.reserve(MaximumCandidates);
        {
            std::scoped_lock lock{retirementsMutex_};
            std::erase_if(retirements_, [](const auto &state) {
                std::scoped_lock stateLock{state->mutex};
                return state.use_count() == 1 && state->candidates.empty();
            });
            if (retirements_.size() >= Extensions::ApplicationCapabilityRegistry::MaximumProviders)
                return Result<Extensions::ExtensionPlatformProviderPublication>::Failure(Invalid("Provider retirement capacity is full."));
            retirements_.push_back(retirement);
        }
        Extensions::BackendServiceDescriptor factoryDescriptor{.serviceId = {std::string{FactoryServiceId}},
                                                               .contractId = {std::string{FactoryServiceId}},
                                                               .capability = {std::string{CapabilityId}},
                                                               .version = version,
                                                               .provider = identity,
                                                               .threadRule = Extensions::BackendServiceThreadRule::ProviderOwnerThread};
        auto factory = std::make_unique<PlatformProviderFactory>(std::move(candidate), std::move(descriptor), retirement);
        auto factoryRegistration = services_.Register(std::move(factoryDescriptor), std::move(factory),
                                                      Extensions::BackendServiceCodeLease::Retain(std::move(moduleCodeLease)));
        if (factoryRegistration.HasError())
            return Result<Extensions::ExtensionPlatformProviderPublication>::Failure(factoryRegistration.ErrorValue());
        // Allocate the publication before the final visibility edge. Its destructor rolls back the staged factory.
        auto concretePublication =
            std::make_unique<PlatformProviderPublication>(std::move(factoryRegistration).Value(), std::move(admission).Value(), retirement);
        auto *typedPublication = concretePublication.get();
        Extensions::ExtensionPlatformProviderPublication publication{concretePublication.release()};
        // A service staged under this generation cannot be resolved without the matching capability lease.
        auto capabilityRegistration = capabilities_.Register({{std::string{CapabilityId}}, version, identity});
        if (capabilityRegistration.HasError())
            return Result<Extensions::ExtensionPlatformProviderPublication>::Failure(capabilityRegistration.ErrorValue());
        typedPublication->Publish(std::move(capabilityRegistration).Value());
        ++nextGeneration_;
        return Result<Extensions::ExtensionPlatformProviderPublication>::Success(std::move(publication));
    }

    /** @copydoc PlatformProviderAdmission::Create */
    Result<PlatformProviderCandidateLease> PlatformProviderAdmission::Create(const Extensions::ExtensionCapabilityHandle &authority,
                                                                             const Extensions::ApplicationCapabilityVersionRange &versions,
                                                                             const std::string_view consumerExtensionId,
                                                                             const std::string_view consumerModuleId,
                                                                             const std::uint64_t consumerGeneration) const {
        auto provider = capabilities_.Resolve(authority, versions, consumerExtensionId, consumerModuleId, consumerGeneration);
        if (provider.HasError())
            return Result<PlatformProviderCandidateLease>::Failure(provider.ErrorValue());
        auto factory = services_.Resolve<PlatformProviderFactory>(std::move(provider).Value(), {std::string{FactoryServiceId}},
                                                                  {std::string{FactoryServiceId}});
        if (factory.HasError())
            return Result<PlatformProviderCandidateLease>::Failure(factory.ErrorValue());
        return std::move(factory).Value().Invoke(&PlatformProviderFactory::Create, std::uint8_t{0});
    }

    /** @copydoc PlatformProviderAdmission::CreateExact */
    Result<PlatformProviderCandidateLease> PlatformProviderAdmission::CreateExact(
        const Extensions::ApplicationCapabilityProviderIdentity &identity, const Extensions::ExtensionCapabilityHandle &authority,
        const Extensions::ApplicationCapabilityVersionRange &versions, const std::string_view consumerExtensionId,
        const std::string_view consumerModuleId, const std::uint64_t consumerGeneration) const {
        auto provider =
            capabilities_.ResolveExact(authority, versions, identity, consumerExtensionId, consumerModuleId, consumerGeneration);
        if (provider.HasError())
            return Result<PlatformProviderCandidateLease>::Failure(provider.ErrorValue());
        auto factory = services_.Resolve<PlatformProviderFactory>(std::move(provider).Value(), {std::string{FactoryServiceId}},
                                                                  {std::string{FactoryServiceId}});
        if (factory.HasError())
            return Result<PlatformProviderCandidateLease>::Failure(factory.ErrorValue());
        return std::move(factory).Value().Invoke(&PlatformProviderFactory::Create, std::uint8_t{0});
    }

    /** @copydoc PlatformProviderAdmission::FinalizeOnOwnerThread */
    PlatformProviderRetirementDisposition PlatformProviderAdmission::FinalizeOnOwnerThread() noexcept {
        const auto factoryRetirement = services_.FinalizeRetiredOnOwnerThread();
        if (factoryRetirement == Extensions::BackendServiceRetirementDisposition::RestartRequired)
            return PlatformProviderRetirementDisposition::RestartRequired;
        std::array<std::shared_ptr<PlatformProviderRetirementState>, Extensions::ApplicationCapabilityRegistry::MaximumProviders> snapshot;
        std::size_t count{};
        {
            std::scoped_lock lock{retirementsMutex_};
            for (const auto &state : retirements_)
                snapshot[count++] = state;
        }
        PlatformProviderRetirementDisposition aggregate = PlatformProviderRetirementDisposition::Complete;
        if (factoryRetirement == Extensions::BackendServiceRetirementDisposition::DeferredUntilCallExit)
            aggregate = PlatformProviderRetirementDisposition::Busy;
        else if (factoryRetirement == Extensions::BackendServiceRetirementDisposition::OwnerThreadFinalizationRequired)
            aggregate = PlatformProviderRetirementDisposition::OwnerThreadRequired;
        for (std::size_t index = 0; index < count; ++index) {
            const auto result = FinalizeCandidates(snapshot[index]);
            if (result == PlatformProviderRetirementDisposition::RestartRequired)
                return result;
            if (result == PlatformProviderRetirementDisposition::OwnerThreadRequired)
                aggregate = result;
            else if (result == PlatformProviderRetirementDisposition::Busy && aggregate == PlatformProviderRetirementDisposition::Complete)
                aggregate = result;
        }
        return aggregate;
    }

    namespace PlatformProviderLifecycleErrors {
        namespace {
            const ErrorDomainId Domain{"horo.platform.lifecycle"};
        }

        const ErrorCodeDescriptor InvalidSelection{Domain,
                                                   ErrorCode{"platform.lifecycle.invalid_selection"},
                                                   ErrorSeverity::Error,
                                                   "The exact configured platform provider is unavailable.",
                                                   "Install and admit the selected provider generation.",
                                                   false,
                                                   false};
        const ErrorCodeDescriptor UnsupportedProfile{Domain,
                                                     ErrorCode{"platform.lifecycle.unsupported_profile"},
                                                     ErrorSeverity::Error,
                                                     "The selected provider has no versioned operation profile.",
                                                     "Use a provider implementing the version-2 operation profile.",
                                                     false,
                                                     false};
        const ErrorCodeDescriptor InitializationFailed{Domain,
                                                       ErrorCode{"platform.lifecycle.initialization_failed"},
                                                       ErrorSeverity::Error,
                                                       "Platform provider initialization failed.",
                                                       "Inspect the selected provider and its service/session/ingress stage.",
                                                       false,
                                                       false};
        const ErrorCodeDescriptor InvalidOperation{Domain,
                                                   ErrorCode{"platform.lifecycle.invalid_operation"},
                                                   ErrorSeverity::Error,
                                                   "Platform provider operation is invalid or unavailable.",
                                                   "Use an advertised service and a bounded Horo operation.",
                                                   false,
                                                   false};
        const ErrorCodeDescriptor DrainBusy{Domain,
                                            ErrorCode{"platform.lifecycle.drain_busy"},
                                            ErrorSeverity::Error,
                                            "Platform provider work or callbacks are still draining.",
                                            "Retry shutdown on the provider owner lane; keep the code lease alive.",
                                            false,
                                            true};
        const ErrorCodeDescriptor ShutdownFailed{Domain,
                                                 ErrorCode{"platform.lifecycle.shutdown_failed"},
                                                 ErrorSeverity::Error,
                                                 "Platform provider shutdown failed.",
                                                 "Retain the provider generation and inspect its shutdown stage.",
                                                 false,
                                                 false};
    }  // namespace PlatformProviderLifecycleErrors

    struct PlatformProviderLifecycleState final {
        static constexpr std::size_t MaximumRequests = 64;
        static constexpr std::size_t MaximumPayloadBytes = 4096;

        struct Completion final {
            std::uint64_t requestId{};
            std::uint64_t requestGeneration{};
            std::uint64_t sessionRevision{};
            std::uint32_t service{};
            std::uint32_t operation{};
            std::uint32_t resultCode{};
            std::uint32_t size{};
            std::array<std::byte, MaximumPayloadBytes> payload{};
        };

        struct InFlight final {
            PlatformRequestId id;
            PlatformRequestGeneration generation;
            std::uint64_t sessionRevision{};
            PlatformServiceKind service;
            std::uint32_t operation{};
            PlatformProviderRequestLease lease;
        };

        explicit PlatformProviderLifecycleState(const PlatformRequestGeneration generation)
            : requests({.activeCapacity = MaximumRequests,
                        .terminalCapacity = MaximumRequests,
                        .observerCapacity = MaximumRequests,
                        .generation = generation}) {
            inFlight.reserve(MaximumRequests);
        }

        std::mutex mutex;  // Protects callback ingress and session evidence; native calls run without this lock.
        std::array<Completion, MaximumRequests> completions{};
        std::size_t completionHead{};
        std::size_t completionCount{};
        PlatformProviderSessionObservation session{};
        bool callbackOpen{true};
        HoroPlatformProviderSink sink{};
        HoroPlatformProviderOperations operations{};
        void *candidate{};
        std::optional<PlatformProviderCandidateLease> lease;
        PlatformRequestStore requests;
        std::vector<InFlight> inFlight;
        std::uint32_t availableServices{};
        bool servicesAttempted{};
        bool sessionAttempted{};
        bool ingressAttempted{};
        bool admissionClosed{};
        bool closing{};
        bool ingressClosed{};
        bool drained{};
        bool sessionStopped{};
        bool servicesStopped{};
        bool closed{};
        std::shared_ptr<PlatformProviderLifecycleState> quarantine;  // BUSY teardown retains the callback context and code.
    };

    namespace {
        [[nodiscard]] HoroExtensionStatus ObserveSession(void *context, const std::uint64_t revision, const std::uint32_t phase) noexcept {
            auto &state = *static_cast<PlatformProviderLifecycleState *>(context);
            std::scoped_lock lock{state.mutex};
            if (!state.callbackOpen || revision == 0 || revision <= state.session.revision || phase > 4)
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            state.session = {.revision = revision, .phase = phase};
            return HORO_EXTENSION_SUCCESS;
        }

        [[nodiscard]] HoroExtensionStatus ReceiveCompletion(void *context, const HoroPlatformProviderCompletion *completion) noexcept {
            if (completion == nullptr || completion->structSize != sizeof(HoroPlatformProviderCompletion) || completion->requestId == 0 ||
                completion->requestGeneration == 0 || completion->sessionRevision == 0 ||
                completion->service >= static_cast<std::uint32_t>(PlatformServiceKind::Count) ||
                completion->payloadSize > PlatformProviderLifecycleState::MaximumPayloadBytes ||
                (completion->payloadSize != 0 && completion->payload == nullptr))
                return HORO_EXTENSION_ERROR_INVALID_ARGS;
            auto &state = *static_cast<PlatformProviderLifecycleState *>(context);
            std::scoped_lock lock{state.mutex};
            if (!state.callbackOpen || state.completionCount == state.completions.size())
                return HORO_EXTENSION_ERROR_OUTPUT_REJECTED;
            auto &slot = state.completions[(state.completionHead + state.completionCount) % state.completions.size()];
            slot.requestId = completion->requestId;
            slot.requestGeneration = completion->requestGeneration;
            slot.sessionRevision = completion->sessionRevision;
            slot.service = completion->service;
            slot.operation = completion->operation;
            slot.resultCode = completion->resultCode;
            slot.size = completion->resultCode == HORO_PLATFORM_PROVIDER_SUCCESS ? completion->payloadSize : 0;
            if (slot.size != 0)
                std::memcpy(slot.payload.data(), completion->payload, slot.size);
            ++state.completionCount;
            return HORO_EXTENSION_SUCCESS;
        }

        template <typename Callback, typename... Arguments>
        [[nodiscard]] HoroExtensionStatus InvokeProvider(Callback callback, Arguments... arguments) noexcept {
            try {
                return callback(arguments...);
            } catch (...) {  // NOSONAR: no C++ exception may cross a provider ABI boundary.
                return HORO_EXTENSION_ERROR_INIT_FAILED;
            }
        }

        [[nodiscard]] PlatformProviderFailureCategory ProviderCategory(const std::uint32_t code) noexcept {
            switch (code) {  // NOSONAR(cpp:S6177) C11 ABI enumerators are unscoped; using enum adds no scope here.
                case HORO_PLATFORM_PROVIDER_OFFLINE:
                    return PlatformProviderFailureCategory::Offline;
                case HORO_PLATFORM_PROVIDER_NOT_SIGNED_IN:
                    return PlatformProviderFailureCategory::NotSignedIn;
                case HORO_PLATFORM_PROVIDER_FORBIDDEN:
                    return PlatformProviderFailureCategory::Forbidden;
                case HORO_PLATFORM_PROVIDER_RATE_LIMITED:
                    return PlatformProviderFailureCategory::RateLimited;
                case HORO_PLATFORM_PROVIDER_PRECONDITION_FAILED:
                    return PlatformProviderFailureCategory::PreconditionFailed;
                case HORO_PLATFORM_PROVIDER_QUOTA_EXCEEDED:
                    return PlatformProviderFailureCategory::QuotaExceeded;
                case HORO_PLATFORM_PROVIDER_INVALID_RESPONSE:
                    return PlatformProviderFailureCategory::InvalidResponse;
                case HORO_PLATFORM_PROVIDER_TRANSIENT_FAILURE:
                    return PlatformProviderFailureCategory::TransientFailure;
                case HORO_PLATFORM_PROVIDER_PERMANENT_FAILURE:
                    return PlatformProviderFailureCategory::PermanentFailure;
                default:
                    return PlatformProviderFailureCategory::Unknown;
            }
        }

        [[nodiscard]] std::uint32_t RequiredMask(const PlatformProjectConfiguration &configuration) noexcept {
            std::uint32_t mask{};
            const auto services = configuration.ServiceRequirements();
            for (std::size_t index = 0; index < services.size(); ++index)
                if (services[index] == PlatformServiceRequirement::Required)
                    mask |= 1U << index;
            return mask;
        }

        [[nodiscard]] std::uint32_t EnabledMask(const PlatformProjectConfiguration &configuration) noexcept {
            std::uint32_t mask{};
            const auto services = configuration.ServiceRequirements();
            for (std::size_t index = 0; index < services.size(); ++index)
                if (services[index] != PlatformServiceRequirement::Disabled)
                    mask |= 1U << index;
            return mask;
        }

        [[nodiscard]] std::uint32_t ClaimedMask(const PlatformProviderContributionDescriptor &descriptor) noexcept {
            std::uint32_t mask{};
            for (std::size_t index = 0; index < descriptor.services.size(); ++index)
                if (descriptor.services[index])
                    mask |= 1U << index;
            return mask;
        }
    }  // namespace

    PlatformProviderLifecycleHost::PlatformProviderLifecycleHost(std::shared_ptr<PlatformProviderLifecycleState> state) noexcept
        : state_(std::move(state)) {}

    PlatformProviderLifecycleHost::~PlatformProviderLifecycleHost() {
        try {
            static_cast<void>(Close());
        } catch (...) {  // NOSONAR: retain callback context and native code if typed teardown cannot be represented.
            if (state_)
                state_->quarantine = state_;
        }
    }

    /** @copydoc PlatformProviderLifecycleHost::Start */
    Result<std::unique_ptr<PlatformProviderLifecycleHost>> PlatformProviderLifecycleHost::Start(
        const PlatformProjectConfiguration &configuration, PlatformProviderAdmission &admission,
        const Extensions::ApplicationCapabilityProviderIdentity &identity, const Extensions::ExtensionCapabilityHandle &authority,
        const Extensions::ApplicationCapabilityVersionRange &versions, const std::string_view consumerExtensionId,
        const std::string_view consumerModuleId, const std::uint64_t consumerGeneration) {
        using HostResult = Result<std::unique_ptr<PlatformProviderLifecycleHost>>;
        if (configuration.UsesNullProvider() || !configuration.SelectedProvider() || !configuration.SelectedModule() ||
            identity.moduleId != configuration.SelectedModule()->value || identity.providerId != configuration.SelectedProviderKey())
            return HostResult::Failure(MakeError(PlatformProviderLifecycleErrors::InvalidSelection));
        auto created = admission.CreateExact(identity, authority, versions, consumerExtensionId, consumerModuleId, consumerGeneration);
        if (created.HasError())
            return HostResult::Failure(MakeError(PlatformProviderLifecycleErrors::InvalidSelection));
        auto candidate = std::move(created).Value();
        const auto &descriptor = candidate.Descriptor();
        const auto requiredMask = RequiredMask(configuration);
        if (descriptor.provider != *configuration.SelectedProvider() || descriptor.owner != identity ||
            (requiredMask & ~ClaimedMask(descriptor)) != 0)
            return HostResult::Failure(MakeError(PlatformProviderLifecycleErrors::InvalidSelection));
        auto &native = *candidate.state_;
        if (native.operations.version != HORO_PLATFORM_SERVICES_PROVIDER_OPERATIONS_VERSION ||
            native.operations.structSize != sizeof(HoroPlatformProviderOperations))
            return HostResult::Failure(MakeError(PlatformProviderLifecycleErrors::UnsupportedProfile));
        auto state = std::make_shared<PlatformProviderLifecycleState>(PlatformRequestGeneration{identity.generation});
        state->operations = native.operations;
        state->candidate = native.candidate;
        state->lease.emplace(std::move(candidate));
        state->sink = {.structSize = sizeof(HoroPlatformProviderSink),
                       .context = state.get(),
                       .sessionChanged = ObserveSession,
                       .complete = ReceiveCompletion};
        auto host = std::unique_ptr<PlatformProviderLifecycleHost>(new PlatformProviderLifecycleHost(state));
        state->servicesAttempted = true;
        std::uint32_t availableMask{};
        if (InvokeProvider(state->operations.initializeServices, state->candidate, requiredMask, &availableMask) !=
                HORO_EXTENSION_SUCCESS ||
            (availableMask & ~ClaimedMask(descriptor)) != 0 || (requiredMask & ~availableMask) != 0) {
            static_cast<void>(host->Close());
            return HostResult::Failure(MakeError(PlatformProviderLifecycleErrors::InitializationFailed));
        }
        state->availableServices = availableMask & EnabledMask(configuration);
        state->sessionAttempted = true;
        if (InvokeProvider(state->operations.beginSession, state->candidate, &state->sink) != HORO_EXTENSION_SUCCESS) {
            static_cast<void>(host->Close());
            return HostResult::Failure(MakeError(PlatformProviderLifecycleErrors::InitializationFailed));
        }
        state->ingressAttempted = true;
        if (InvokeProvider(state->operations.openIngress, state->candidate, &state->sink) != HORO_EXTENSION_SUCCESS) {
            static_cast<void>(host->Close());
            return HostResult::Failure(MakeError(PlatformProviderLifecycleErrors::InitializationFailed));
        }
        return HostResult::Success(std::move(host));
    }

    /** @copydoc PlatformProviderLifecycleHost::UnlockAchievement */
    Result<PlatformProviderLifecycleHost::RequestHandle> PlatformProviderLifecycleHost::UnlockAchievement(const AchievementId achievement) {
        if (!achievement.IsValid())
            return Result<RequestHandle>::Failure(MakeError(PlatformProviderLifecycleErrors::InvalidOperation));
        std::array<std::byte, sizeof(std::uint64_t)> payload{};
        for (std::size_t index = 0; index < payload.size(); ++index)
            payload[index] = std::byte{static_cast<std::uint8_t>(achievement.value >> (8U * index))};
        return Submit(PlatformServiceKind::Achievements, HORO_PLATFORM_OPERATION_ACHIEVEMENT_UNLOCK, payload);
    }

    /** @brief Sends one validated Horo operation through the selected native candidate. */
    Result<PlatformProviderLifecycleHost::RequestHandle> PlatformProviderLifecycleHost::Submit(const PlatformServiceKind service,
                                                                                               const std::uint32_t operation,
                                                                                               const std::span<const std::byte> payload) {
        using SubmitResult = Result<RequestHandle>;
        auto &state = *state_;
        const auto serviceIndex = static_cast<std::uint32_t>(service);
        if (state.closing || state.closed || !state.lease)
            return SubmitResult::Failure(MakeError(FrontendErrors::Unavailable));
        std::uint64_t sessionRevision{};
        {
            std::scoped_lock lock{state.mutex};
            if (state.session.phase != static_cast<std::uint32_t>(PlatformSessionPhase::Active))
                return SubmitResult::Failure(MakeError(PlatformSessionErrors::NoSubject));
            sessionRevision = state.session.revision;
        }
        if (service != PlatformServiceKind::Achievements || operation != HORO_PLATFORM_OPERATION_ACHIEVEMENT_UNLOCK ||
            payload.size() != sizeof(std::uint64_t) || (state.availableServices & (1U << serviceIndex)) == 0)
            return SubmitResult::Failure(MakeError(PlatformProviderLifecycleErrors::InvalidOperation));
        if (state.inFlight.size() == PlatformProviderLifecycleState::MaximumRequests)
            return SubmitResult::Failure(MakeError(RequestErrors::CapacityExceeded));
        auto nativeLease = state.lease->AcquireRequestLease();
        if (nativeLease.HasError())
            return SubmitResult::Failure(nativeLease.ErrorValue());
        auto admitted = state.requests.Admit<void>();
        if (admitted.HasError())
            return SubmitResult::Failure(admitted.ErrorValue());
        auto handle = std::move(admitted).Value();
        state.inFlight.push_back({handle.Id(), handle.Generation(), sessionRevision, service, operation, std::move(nativeLease).Value()});
        static_cast<void>(state.requests.MarkRunning(handle));
        const HoroPlatformProviderOperation input{.structSize = sizeof(HoroPlatformProviderOperation),
                                                  .requestId = handle.Id().value,
                                                  .requestGeneration = handle.Generation().value,
                                                  .sessionRevision = sessionRevision,
                                                  .service = serviceIndex,
                                                  .operation = operation,
                                                  .payload = reinterpret_cast<const std::uint8_t *>(payload.data()),
                                                  .payloadSize = static_cast<std::uint32_t>(payload.size())};
        const HoroExtensionStatus submission = InvokeProvider(state.operations.submit, state.candidate, &input);
        if (submission != HORO_EXTENSION_SUCCESS) {
            if (submission == HORO_EXTENSION_ERROR_CANCELLED) {
                static_cast<void>(state.requests.RequestCancel(handle));
                static_cast<void>(state.requests.CompleteCancelled(handle, MakeError(RequestErrors::Cancelled)));
            } else {
                static_cast<void>(
                    state.requests.CompleteFailure(handle, MakePlatformProviderError(PlatformProviderFailureCategory::Unknown,
                                                                                     handle.Id().value, handle.Generation().value)));
            }
            // An adapter may have started native work before reporting failure; its lease retires on callback or drain.
        }
        return SubmitResult::Success(std::move(handle));
    }

    /** @copydoc PlatformProviderLifecycleHost::DispatchCompletions */
    std::size_t PlatformProviderLifecycleHost::DispatchCompletions(const std::size_t maximum) {
        auto &state = *state_;
        std::size_t processed{};
        while (processed < maximum) {
            PlatformProviderLifecycleState::Completion completion;
            {
                std::scoped_lock lock{state.mutex};
                if (state.completionCount == 0)
                    break;
                completion = state.completions[state.completionHead];
                state.completionHead = (state.completionHead + 1) % state.completions.size();
                --state.completionCount;
            }
            const auto found = std::ranges::find_if(state.inFlight, [&](const auto &entry) {
                return entry.id.value == completion.requestId && entry.generation.value == completion.requestGeneration &&
                       static_cast<std::uint32_t>(entry.service) == completion.service && entry.operation == completion.operation;
            });
            if (found != state.inFlight.end()) {
                RequestHandle handle{found->id, found->generation};
                const auto currentSessionRevision = [&state] {
                    std::scoped_lock lock{state.mutex};
                    return state.session.revision;
                }();
                if (completion.sessionRevision != found->sessionRevision || currentSessionRevision != found->sessionRevision)
                    static_cast<void>(state.requests.CompleteFailure(handle, MakeError(PlatformSessionErrors::StaleSession)));
                else if (completion.resultCode == HORO_PLATFORM_PROVIDER_SUCCESS)
                    static_cast<void>(state.requests.CompleteSuccess(handle));
                else if (completion.resultCode == HORO_PLATFORM_PROVIDER_CANCELLED) {
                    static_cast<void>(state.requests.RequestCancel(handle));
                    static_cast<void>(state.requests.CompleteCancelled(handle, MakeError(RequestErrors::Cancelled)));
                } else if (completion.resultCode == HORO_PLATFORM_PROVIDER_TIMED_OUT)
                    static_cast<void>(state.requests.CompleteTimedOut(handle, MakeError(RequestErrors::TimedOut)));
                else if (completion.resultCode == HORO_PLATFORM_PROVIDER_CAPABILITY_UNAVAILABLE)
                    static_cast<void>(state.requests.CompleteFailure(handle, MakeError(BackendErrors::ServiceUnavailable)));
                else
                    static_cast<void>(
                        state.requests.CompleteFailure(handle, MakePlatformProviderError(ProviderCategory(completion.resultCode),
                                                                                         handle.Id().value, handle.Generation().value)));
                state.inFlight.erase(found);
            }
            ++processed;
        }
        static_cast<void>(state.requests.DispatchCompletions(maximum));
        return processed;
    }

    /** @copydoc PlatformProviderLifecycleHost::Query */
    Result<PlatformRequestSnapshot<void>> PlatformProviderLifecycleHost::Query(const RequestHandle &request) const {
        return state_->requests.Query(request);
    }

    /** @copydoc PlatformProviderLifecycleHost::OnComplete */
    Result<PlatformRequestSubscription> PlatformProviderLifecycleHost::OnComplete(
        const RequestHandle &request, std::function<void(const PlatformRequestSnapshot<void> &)> observer) {
        return state_->requests.OnComplete(request, std::move(observer));
    }

    /** @copydoc PlatformProviderLifecycleHost::Session */
    PlatformProviderSessionObservation PlatformProviderLifecycleHost::Session() const noexcept {
        std::scoped_lock lock{state_->mutex};
        return state_->session;
    }

    /** @copydoc PlatformProviderLifecycleHost::Close */
    Result<void> PlatformProviderLifecycleHost::Close() {
        auto &state = *state_;
        if (state.closed)
            return Result<void>::Success();
        state.closing = true;
        if (!state.admissionClosed) {
            if (state.servicesAttempted && InvokeProvider(state.operations.closeAdmission, state.candidate) != HORO_EXTENSION_SUCCESS) {
                state.quarantine = state_;
                return Result<void>::Failure(MakeError(PlatformProviderLifecycleErrors::ShutdownFailed));
            }
            state.admissionClosed = true;
        }
        if (!state.ingressClosed) {
            {
                std::scoped_lock lock{state.mutex};
                state.callbackOpen = false;
                state.completionCount = 0;
            }
            if (state.ingressAttempted && InvokeProvider(state.operations.closeIngress, state.candidate) != HORO_EXTENSION_SUCCESS) {
                state.quarantine = state_;
                return Result<void>::Failure(MakeError(PlatformProviderLifecycleErrors::ShutdownFailed));
            }
            state.ingressClosed = true;
        }
        for (const auto &request : state.inFlight) {
            static_cast<void>(state.requests.RequestCancel(request.id, request.generation));
            static_cast<void>(InvokeProvider(state.operations.cancel, state.candidate, request.id.value, request.generation.value));
        }
        if (!state.drained && (state.sessionAttempted || state.ingressAttempted)) {
            const auto status = InvokeProvider(state.operations.drain, state.candidate);
            if (status != HORO_EXTENSION_SUCCESS) {
                state.quarantine = state_;
                return Result<void>::Failure(MakeError(status == HORO_EXTENSION_ERROR_BUSY
                                                           ? PlatformProviderLifecycleErrors::DrainBusy
                                                           : PlatformProviderLifecycleErrors::ShutdownFailed));
            }
            state.drained = true;
        }
        state.inFlight.clear();
        state.requests.Shutdown();
        if (!state.sessionStopped && state.sessionAttempted) {
            if (InvokeProvider(state.operations.stopSession, state.candidate) != HORO_EXTENSION_SUCCESS) {
                state.quarantine = state_;
                return Result<void>::Failure(MakeError(PlatformProviderLifecycleErrors::ShutdownFailed));
            }
            state.sessionStopped = true;
        }
        if (!state.servicesStopped && state.servicesAttempted) {
            if (InvokeProvider(state.operations.shutdownServices, state.candidate) != HORO_EXTENSION_SUCCESS) {
                state.quarantine = state_;
                return Result<void>::Failure(MakeError(PlatformProviderLifecycleErrors::ShutdownFailed));
            }
            state.servicesStopped = true;
        }
        state.lease.reset();
        state.closed = true;
        state.quarantine.reset();
        return Result<void>::Success();
    }
}  // namespace Horo::PlatformServices
