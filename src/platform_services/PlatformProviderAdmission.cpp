#include "Horo/PlatformServices/PlatformProviderAdmission.h"

#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <thread>
#include <utility>

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

        /** @brief Retires one revoked, unleased candidate on its owner thread; reports pending work. */
        [[nodiscard]] bool FinalizeCandidate(const std::shared_ptr<PlatformProviderCandidateState> &state,
                                             const std::shared_ptr<PlatformProviderRetirementState> &retirement) noexcept {
            {
                std::scoped_lock lock{state->mutex};
                if (state->retired || !state->revoked)
                    return false;
                if (state->leaseCount != 0 || state->retiring)
                    return true;
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
                } else if (status != HORO_EXTENSION_ERROR_BUSY) {
                    retirement->restartRequired.store(true, std::memory_order_release);
                }
            }
            return status == HORO_EXTENSION_ERROR_BUSY;
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
            for (std::size_t index = 0; index < count; ++index)
                busy = FinalizeCandidate(snapshot[index], retirement) || busy;
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

        /** @brief Reclaims drained generations and retains one new candidate generation within the fixed bound. */
        [[nodiscard]] bool RetainRetirement(std::mutex &mutex, std::vector<std::shared_ptr<PlatformProviderRetirementState>> &retirements,
                                            const std::shared_ptr<PlatformProviderRetirementState> &retirement) {
            std::scoped_lock lock{mutex};
            std::erase_if(retirements, [](const auto &state) {
                std::scoped_lock stateLock{state->mutex};
                return state.use_count() == 1 && state->candidates.empty();
            });
            if (retirements.size() >= Extensions::ApplicationCapabilityRegistry::MaximumProviders)
                return false;
            retirements.push_back(retirement);
            return true;
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
        if (!RetainRetirement(retirementsMutex_, retirements_, retirement))
            return Result<Extensions::ExtensionPlatformProviderPublication>::Failure(Invalid("Provider retirement capacity is full."));
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
}  // namespace Horo::PlatformServices
