#include "Horo/Extensions/ApplicationCapabilityRegistry.h"

#include "../ExtensionAuthorityIdentityValidation.h"
#include "Horo/Extensions/ExtensionErrors.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <ranges>
#include <tuple>
#include <utility>
#include <vector>

namespace Horo::Extensions {
    struct ApplicationCapabilityProviderState final {
        ApplicationCapabilityProviderDescriptor descriptor;
        std::atomic_bool active{true};
    };

    struct ApplicationCapabilityRegistryState final {
        // Registration and resolution are bounded control-path operations. The mutex
        // makes publication removal linearizable with concurrent callback admission.
        std::mutex mutex;
        std::vector<std::shared_ptr<ApplicationCapabilityProviderState>> providers;
        bool shutdown{};
    };

    namespace {
        [[nodiscard]] bool IsValid(const ApplicationCapabilityProviderDescriptor &descriptor) noexcept {
            return Detail::IsCanonicalExtensionAuthorityId(descriptor.capability.value) &&
                   Detail::IsCanonicalExtensionAuthorityId(descriptor.provider.moduleId) &&
                   Detail::IsCanonicalExtensionAuthorityId(descriptor.provider.providerId) &&
                   descriptor.version != ApplicationCapabilityVersion{} && descriptor.provider.generation != 0;
        }

        [[nodiscard]] bool IsValid(const ApplicationCapabilityVersionRange &versions) noexcept {
            return versions.minimum != ApplicationCapabilityVersion{} && versions.maximum != ApplicationCapabilityVersion{} &&
                   versions.minimum <= versions.maximum;
        }

        void RemoveProvider(const std::shared_ptr<ApplicationCapabilityRegistryState> &registry,
                            const std::shared_ptr<ApplicationCapabilityProviderState> &provider) noexcept {
            std::scoped_lock lock{registry->mutex};
            provider->active.store(false, std::memory_order_release);
            std::erase(registry->providers, provider);
        }
    }  // namespace

    ApplicationCapabilityProviderRegistration::ApplicationCapabilityProviderRegistration(
        std::weak_ptr<ApplicationCapabilityRegistryState> registry, std::shared_ptr<ApplicationCapabilityProviderState> provider) noexcept
        : registry_(std::move(registry)), provider_(std::move(provider)) {}

    ApplicationCapabilityProviderRegistration::~ApplicationCapabilityProviderRegistration() {
        Reset();
    }

    ApplicationCapabilityProviderRegistration::ApplicationCapabilityProviderRegistration(
        ApplicationCapabilityProviderRegistration &&other) noexcept
        : registry_(std::move(other.registry_)), provider_(std::move(other.provider_)) {}

    ApplicationCapabilityProviderRegistration &ApplicationCapabilityProviderRegistration::operator=(
        ApplicationCapabilityProviderRegistration &&other) noexcept {
        if (this == &other)
            return *this;
        Reset();
        registry_ = std::move(other.registry_);
        provider_ = std::move(other.provider_);
        return *this;
    }

    /** @copydoc ApplicationCapabilityProviderRegistration::Reset */
    void ApplicationCapabilityProviderRegistration::Reset() noexcept {
        if (provider_ == nullptr)
            return;
        if (auto registry = registry_.lock())
            RemoveProvider(registry, provider_);
        else
            provider_->active.store(false, std::memory_order_release);
        provider_.reset();
        registry_.reset();
    }

    /** @copydoc ApplicationCapabilityProviderRegistration::IsRegistered */
    bool ApplicationCapabilityProviderRegistration::IsRegistered() const noexcept {
        return provider_ != nullptr && provider_->active.load(std::memory_order_acquire);
    }

    ApplicationCapabilityProviderLease::ApplicationCapabilityProviderLease(
        std::shared_ptr<const ApplicationCapabilityProviderState> provider, ExtensionCapabilityUseLease admission) noexcept
        : provider_(std::move(provider)), admission_(std::move(admission)) {}

    /** @copydoc ApplicationCapabilityProviderLease::Descriptor */
    const ApplicationCapabilityProviderDescriptor &ApplicationCapabilityProviderLease::Descriptor() const noexcept {
        return provider_->descriptor;
    }

    /** @copydoc ApplicationCapabilityProviderLease::Consumer */
    const ExtensionActivationIdentity &ApplicationCapabilityProviderLease::Consumer() const noexcept {
        return admission_.Activation();
    }

    /** @copydoc ApplicationCapabilityProviderLease::IsUsable */
    bool ApplicationCapabilityProviderLease::IsUsable() const noexcept {
        return provider_ != nullptr && provider_->active.load(std::memory_order_acquire) && admission_.IsUsable();
    }

    ApplicationCapabilityRegistry::ApplicationCapabilityRegistry() : state_(std::make_shared<ApplicationCapabilityRegistryState>()) {
        state_->providers.reserve(MaximumProviders);
    }

    ApplicationCapabilityRegistry::~ApplicationCapabilityRegistry() {
        BeginShutdown();
    }

    ApplicationCapabilityRegistryState &ApplicationCapabilityRegistry::MutableState() noexcept {
        return *state_;
    }

    /** @copydoc ApplicationCapabilityRegistry::Register */
    Result<ApplicationCapabilityProviderRegistration> ApplicationCapabilityRegistry::Register(
        ApplicationCapabilityProviderDescriptor descriptor) {
        if (!IsValid(descriptor)) {
            return Result<ApplicationCapabilityProviderRegistration>::Failure(
                MakeError(ExtensionErrors::CapabilityRegistryInvalid, "Capability provider descriptor is malformed."));
        }
        if (state_ == nullptr)
            return Result<ApplicationCapabilityProviderRegistration>::Failure(MakeError(ExtensionErrors::CapabilityRegistryShutdown));
        ApplicationCapabilityRegistryState &state = MutableState();
        std::scoped_lock lock{state.mutex};
        if (state.shutdown)
            return Result<ApplicationCapabilityProviderRegistration>::Failure(MakeError(ExtensionErrors::CapabilityRegistryShutdown));
        if (const auto duplicate = std::ranges::find_if(state.providers,
                                                        [&descriptor](const auto &provider) {
            return provider->descriptor.capability == descriptor.capability && provider->descriptor.version == descriptor.version;
        });
            duplicate != state.providers.end()) {
            return Result<ApplicationCapabilityProviderRegistration>::Failure(
                MakeError(ExtensionErrors::CapabilityRegistryDuplicate,
                          "Capability and contract version already have a provider: " + descriptor.capability.value));
        }
        if (state.providers.size() >= MaximumProviders) {
            return Result<ApplicationCapabilityProviderRegistration>::Failure(
                MakeError(ExtensionErrors::CapabilityRegistryCapacityExceeded));
        }
        auto provider = std::make_shared<ApplicationCapabilityProviderState>();
        provider->descriptor = std::move(descriptor);
        state.providers.push_back(provider);
        std::ranges::sort(state.providers, [](const auto &left, const auto &right) {
            return std::tie(left->descriptor.capability.value, left->descriptor.version) <
                   std::tie(right->descriptor.capability.value, right->descriptor.version);
        });
        return Result<ApplicationCapabilityProviderRegistration>::Success(
            ApplicationCapabilityProviderRegistration{state_, std::move(provider)});
    }

    /** @copydoc ApplicationCapabilityRegistry::Resolve */
    Result<ApplicationCapabilityProviderLease> ApplicationCapabilityRegistry::Resolve(const ExtensionCapabilityHandle &authority,
                                                                                      const ApplicationCapabilityVersionRange &versions,
                                                                                      const std::string_view extensionId,
                                                                                      const std::string_view moduleId,
                                                                                      const std::uint64_t activationGeneration) const {
        if (!IsValid(versions)) {
            return Result<ApplicationCapabilityProviderLease>::Failure(
                MakeError(ExtensionErrors::CapabilityRegistryInvalid, "Capability version range is malformed."));
        }
        if (state_ == nullptr)
            return Result<ApplicationCapabilityProviderLease>::Failure(MakeError(ExtensionErrors::CapabilityRegistryShutdown));
        std::scoped_lock lock{state_->mutex};
        if (state_->shutdown)
            return Result<ApplicationCapabilityProviderLease>::Failure(MakeError(ExtensionErrors::CapabilityRegistryShutdown));

        std::shared_ptr<ApplicationCapabilityProviderState> selected;
        bool capabilityExists = false;
        for (const auto &provider : state_->providers) {
            if (provider->descriptor.capability != authority.Capability())
                continue;
            capabilityExists = true;
            if (provider->descriptor.version < versions.minimum || provider->descriptor.version > versions.maximum)
                continue;
            if (selected == nullptr || selected->descriptor.version < provider->descriptor.version)
                selected = provider;
        }
        if (selected == nullptr) {
            const auto &error = capabilityExists ? ExtensionErrors::CapabilityVersionIncompatible : ExtensionErrors::CapabilityUnavailable;
            return Result<ApplicationCapabilityProviderLease>::Failure(MakeError(error));
        }
        auto admitted = authority.AcquireUse(extensionId, moduleId, activationGeneration);
        if (admitted.HasError())
            return Result<ApplicationCapabilityProviderLease>::Failure(admitted.ErrorValue());
        return Result<ApplicationCapabilityProviderLease>::Success(
            ApplicationCapabilityProviderLease{std::move(selected), std::move(admitted).Value()});
    }

    /** @copydoc ApplicationCapabilityRegistry::ResolveExact */
    Result<ApplicationCapabilityProviderLease> ApplicationCapabilityRegistry::ResolveExact(
        const ExtensionCapabilityHandle &authority, const ApplicationCapabilityVersionRange &versions,
        const ApplicationCapabilityProviderIdentity &identity, const std::string_view extensionId, const std::string_view moduleId,
        const std::uint64_t activationGeneration) const {
        if (!IsValid(versions) || identity.moduleId.empty() || identity.providerId.empty() || identity.generation == 0)
            return Result<ApplicationCapabilityProviderLease>::Failure(MakeError(ExtensionErrors::CapabilityRegistryInvalid));
        if (state_ == nullptr)
            return Result<ApplicationCapabilityProviderLease>::Failure(MakeError(ExtensionErrors::CapabilityRegistryShutdown));
        std::scoped_lock lock{state_->mutex};
        if (state_->shutdown)
            return Result<ApplicationCapabilityProviderLease>::Failure(MakeError(ExtensionErrors::CapabilityRegistryShutdown));
        const auto found = std::ranges::find_if(state_->providers, [&](const auto &provider) {
            const auto &descriptor = provider->descriptor;
            return descriptor.capability == authority.Capability() && descriptor.provider == identity &&
                   descriptor.version >= versions.minimum && descriptor.version <= versions.maximum;
        });
        if (found == state_->providers.end())
            return Result<ApplicationCapabilityProviderLease>::Failure(MakeError(ExtensionErrors::CapabilityUnavailable));
        auto admitted = authority.AcquireUse(extensionId, moduleId, activationGeneration);
        if (admitted.HasError())
            return Result<ApplicationCapabilityProviderLease>::Failure(admitted.ErrorValue());
        return Result<ApplicationCapabilityProviderLease>::Success(ApplicationCapabilityProviderLease{*found, std::move(admitted).Value()});
    }

    /** @copydoc ApplicationCapabilityRegistry::BeginShutdown */
    void ApplicationCapabilityRegistry::BeginShutdown() noexcept {
        if (state_ == nullptr)
            return;
        ApplicationCapabilityRegistryState &state = MutableState();
        std::scoped_lock lock{state.mutex};
        state.shutdown = true;
        for (const auto &provider : state.providers)
            provider->active.store(false, std::memory_order_release);
        state.providers.clear();
    }

    /** @copydoc ApplicationCapabilityRegistry::IsShutdown */
    bool ApplicationCapabilityRegistry::IsShutdown() const noexcept {
        if (state_ == nullptr)
            return true;
        std::scoped_lock lock{state_->mutex};
        return state_->shutdown;
    }
}  // namespace Horo::Extensions
