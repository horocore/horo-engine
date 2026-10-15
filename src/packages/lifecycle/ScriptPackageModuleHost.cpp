#include "Horo/Packages/ScriptPackageModuleHost.h"

#include "Horo/Packages/PackageLifecycleErrors.h"

#include <algorithm>
#include <limits>

namespace Horo::Packages {
    namespace {
        /** @brief Pins native code and exact verified module through actual provider registration teardown. */
        struct ScriptRoleState final {
            VerifiedPackageModule module;
            std::shared_ptr<Extensions::ScriptInvocationRegistry> registry;
            std::unique_ptr<IPackageModuleActivation> native;
            std::optional<Extensions::ScriptInvocationProviderRegistration> provider;
            std::optional<Error> failure;
            bool retired{};
        };

        /** @brief Withdraws script admission before finalizing its native resource owner. */
        class ScriptRoleReceipt final : public IPackageModuleActivation {
        public:
            explicit ScriptRoleReceipt(std::shared_ptr<ScriptRoleState> state) : state_(std::move(state)) {}

            bool IsActive() const noexcept override {
                return !state_->retired && !state_->failure && state_->provider && state_->provider->IsRegistered() &&
                       state_->native->IsActive();
            }

            std::optional<Error> Diagnostic() const override {
                return state_->failure;
            }

            void Retire() noexcept override {
                state_->retired = true;
                if (state_->provider)
                    state_->provider->Reset();
                if (state_->native)
                    state_->native->Retire();
            }

            bool Finalize() noexcept override {
                if (!state_->retired || (state_->native && !state_->native->Finalize()))
                    return false;
                state_->provider.reset();
                state_->native.reset();
                return true;
            }

        private:
            std::shared_ptr<ScriptRoleState> state_;
        };

        /** @brief Matches immutable package role identity without consulting ambient registries. */
        bool Matches(const ScriptRoleState &state, const HoroPackageId &package, const std::string_view module,
                     const std::uint64_t generation) {
            return !state.retired && !state.failure && state.provider && state.module.Identity().package == package &&
                   state.module.Module().id == module && state.module.Identity().generation == generation;
        }
    }  // namespace

    struct ScriptPackageModuleHost::Impl final {
        std::vector<PackageScriptRegistryRealm> realms;
        std::shared_ptr<NativePackageModuleHost> native;
        std::vector<std::weak_ptr<ScriptRoleState>> roles;
        bool valid{};
    };

    /** @copydoc ScriptPackageModuleHost::ScriptPackageModuleHost */
    ScriptPackageModuleHost::ScriptPackageModuleHost(std::vector<PackageScriptRegistryRealm> realms,
                                                     std::shared_ptr<NativePackageModuleHost> native)
        : impl_(std::make_unique<Impl>()) {
        impl_->realms = std::move(realms);
        impl_->native = std::move(native);
        impl_->roles.reserve(64);
        impl_->valid = !impl_->realms.empty() && impl_->realms.size() <= 64;
        for (std::size_t index = 0; index < impl_->realms.size(); ++index) {
            const auto &realm = impl_->realms[index];
            if (!realm.registry || realm.module.empty() || realm.module.size() > 256)
                impl_->valid = false;
            for (std::size_t prior = 0; prior < index; ++prior)
                if ((realm.package == impl_->realms[prior].package && realm.module == impl_->realms[prior].module) ||
                    realm.registry == impl_->realms[prior].registry)
                    impl_->valid = false;
        }
    }

    ScriptPackageModuleHost::~ScriptPackageModuleHost() = default;

    /** @copydoc ScriptPackageModuleHost::Identity */
    std::string_view ScriptPackageModuleHost::Identity() const noexcept {
        return "script.invocation";
    }

    /** @copydoc ScriptPackageModuleHost::Supports */
    bool ScriptPackageModuleHost::Supports(const Extensions::ExtensionModuleRole role) const noexcept {
        return role == Extensions::ExtensionModuleRole::ScriptProvider && impl_->valid && impl_->native;
    }

    /** @copydoc ScriptPackageModuleHost::Activate */
    Result<std::unique_ptr<IPackageModuleActivation>> ScriptPackageModuleHost::Activate(
        const VerifiedPackageModule &module, const std::span<const std::shared_ptr<IPackageModuleActivation>> dependencies,
        const CancellationToken &cancellation) {
        std::erase_if(impl_->roles, [](const auto &role) {
            return role.expired();
        });
        if (!Supports(module.Identity().role) || impl_->roles.size() >= 64)
            return Result<std::unique_ptr<IPackageModuleActivation>>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
        const auto realm = std::ranges::find_if(impl_->realms, [&module](const auto &value) {
            return value.package == module.Identity().package && value.module == module.Module().id;
        });
        if (realm == impl_->realms.end())
            return Result<std::unique_ptr<IPackageModuleActivation>>::Failure(
                MakeError(PackageLifecycleErrors::InvalidCandidate, "The script module has no exact injected registry realm."));
        auto state = std::make_shared<ScriptRoleState>(ScriptRoleState{module, realm->registry, {}});
        auto receipt = std::make_unique<ScriptRoleReceipt>(state);
        impl_->roles.push_back(state);
        auto activationFailure = MakeError(PackageLifecycleErrors::InvalidCandidate);
        try {
            auto native = impl_->native->Activate(module, dependencies, cancellation);
            if (native.HasError())
                return native;
            state->native = std::move(native).Value();
            if (!state->native->IsActive())
                state->failure = state->native->Diagnostic().value_or(activationFailure);
            else {
                auto provider = realm->registry->RegisterProvider({module.Identity().generation, 64});
                if (provider.HasError())
                    state->failure = provider.ErrorValue();
                else
                    state->provider.emplace(std::move(provider).Value());
            }
        } catch (...) {
            // Diagnostic allocation failure must also return the already-created rollback owner.
            state->failure = std::move(activationFailure);
        }
        return Result<std::unique_ptr<IPackageModuleActivation>>::Success(std::move(receipt));
    }

    /** @copydoc ScriptPackageModuleHost::Provider */
    Extensions::ScriptInvocationProviderLease ScriptPackageModuleHost::Provider(const HoroPackageId &package, const std::string_view module,
                                                                                const std::uint64_t generation) const {
        for (const auto &weak : impl_->roles)
            if (auto state = weak.lock(); state && Matches(*state, package, module, generation))
                return state->provider->Lease();
        return {};
    }

    /** @copydoc ScriptPackageModuleHost::Capability */
    Result<Extensions::ExtensionCapabilityHandle> ScriptPackageModuleHost::Capability(
        const HoroPackageId &package, const std::string_view module, const std::uint64_t generation,
        const Extensions::ExtensionCapabilityId &capability) const {
        for (const auto &weak : impl_->roles)
            if (auto state = weak.lock(); state && Matches(*state, package, module, generation))
                for (const auto &grant : state->module.Capabilities())
                    if (grant.Capability() == capability)
                        return Result<Extensions::ExtensionCapabilityHandle>::Success(grant);
        return Result<Extensions::ExtensionCapabilityHandle>::Failure(MakeError(PackageLifecycleErrors::TrustRequired));
    }

    /** @copydoc ScriptPackageModuleHost::Begin */
    Result<Extensions::ScriptInvocationController> ScriptPackageModuleHost::Begin(const HoroPackageId &package,
                                                                                  const std::string_view module,
                                                                                  const std::uint64_t generation,
                                                                                  const Extensions::ScriptCapabilityBinding &binding,
                                                                                  Extensions::ScriptInvocationRequest request) const {
        for (const auto &weak : impl_->roles)
            if (auto state = weak.lock(); state && Matches(*state, package, module, generation))
                return binding.Begin(*state->provider, std::move(request));
        return Result<Extensions::ScriptInvocationController>::Failure(MakeError(PackageLifecycleErrors::InvalidLifecycle));
    }
}  // namespace Horo::Packages
