#include "Horo/Packages/NativePackageModuleHost.h"

#include "Horo/Packages/PackageLifecycleErrors.h"
#include "PackageModuleCandidateInternal.h"

#include <algorithm>

namespace Horo::Packages {
    namespace {
        /** @brief Keeps exact verified resources and owning-host registrations together through native callback drain. */
        struct NativeRoleState final {
            VerifiedPackageModule module;
            std::vector<std::shared_ptr<IPackageModuleActivation>> dependencies;
            std::unique_ptr<Detail::PackageActivationComposition> composition;
            std::optional<Error> failure;
            bool retired{};
        };

        /** @brief Actual native registration receipt retained by the package retirement authority. */
        class NativeRoleReceipt final : public IPackageModuleActivation {
        public:
            explicit NativeRoleReceipt(std::shared_ptr<NativeRoleState> state) : state_(std::move(state)) {}

            bool IsActive() const noexcept override {
                return !state_->retired && !state_->failure && state_->module.Activation().IsUsable() && state_->composition->Snapshot();
            }

            std::optional<Error> Diagnostic() const override {
                return state_->failure;
            }

            void Retire() noexcept override {
                if (state_->retired)
                    return;
                state_->retired = true;
                state_->composition->Retire();
            }

            bool Finalize() noexcept override {
                if (!state_->retired)
                    return false;
                try {
                    if (state_->composition && !state_->composition->Finalize())
                        return false;
                    state_->composition.reset();
                    state_->dependencies.clear();
                    return true;
                } catch (...) {
                    return false;
                }
            }

        private:
            std::shared_ptr<NativeRoleState> state_;
        };
    }  // namespace

    struct NativePackageModuleHost::Impl final {
        PackageLifecycleConfiguration configuration;
        std::vector<std::weak_ptr<NativeRoleState>> roles;
    };

    /** @copydoc NativePackageModuleHost::NativePackageModuleHost */
    NativePackageModuleHost::NativePackageModuleHost(PackageLifecycleConfiguration configuration) : impl_(std::make_unique<Impl>()) {
        configuration.moduleHosts.clear();
        configuration.moduleRetirements.reset();
        impl_->configuration = std::move(configuration);
        impl_->roles.reserve(64);
    }

    NativePackageModuleHost::~NativePackageModuleHost() = default;

    /** @copydoc NativePackageModuleHost::Identity */
    std::string_view NativePackageModuleHost::Identity() const noexcept {
        return "extensions.native";
    }

    /** @copydoc NativePackageModuleHost::Supports */
    bool NativePackageModuleHost::Supports(const Extensions::ExtensionModuleRole role) const noexcept {
        using enum Extensions::ExtensionModuleRole;
        return role == BackendCapability || role == HeadlessTooling ||
               (role == EditorPresentation && impl_->configuration.profile == Extensions::ExtensionHostProfile::Interactive &&
                static_cast<bool>(impl_->configuration.editorActivityHost));
    }

    /** @copydoc NativePackageModuleHost::Activate */
    Result<std::unique_ptr<IPackageModuleActivation>> NativePackageModuleHost::Activate(
        const VerifiedPackageModule &module, const std::span<const std::shared_ptr<IPackageModuleActivation>> dependencies,
        const CancellationToken &cancellation) {
        std::erase_if(impl_->roles, [](const auto &role) {
            return role.expired();
        });
        if ((!Supports(module.Identity().role) && module.Identity().role != Extensions::ExtensionModuleRole::ScriptProvider) ||
            !module.Activation().IsUsable() || impl_->roles.size() >= 64)
            return Result<std::unique_ptr<IPackageModuleActivation>>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
        auto composition =
            std::make_unique<Detail::PackageActivationComposition>(impl_->configuration, module.Install(), module.Identity().generation);
        auto state =
            std::make_shared<NativeRoleState>(NativeRoleState{module, {dependencies.begin(), dependencies.end()}, std::move(composition)});
        auto receipt = std::make_unique<NativeRoleReceipt>(state);
        impl_->roles.push_back(state);
        const auto candidate = Detail::PackageModuleCandidateFactory::NativeCandidate(module);
        auto activationFailure = MakeError(PackageLifecycleErrors::InvalidCandidate);
        try {
            auto prepared = state->composition->Prepare(std::span{&candidate, 1}, impl_->configuration, cancellation);
            if (prepared.HasError())
                state->failure = prepared.ErrorValue();
        } catch (...) {
            state->failure = std::move(activationFailure);
        }
        return Result<std::unique_ptr<IPackageModuleActivation>>::Success(std::move(receipt));
    }

    /** @copydoc NativePackageModuleHost::Importers */
    std::shared_ptr<const Assets::AssetImporterCatalogSnapshot> NativePackageModuleHost::Importers(const HoroPackageId &package,
                                                                                                   const std::string_view module,
                                                                                                   const std::uint64_t generation) const {
        for (const auto &weak : impl_->roles)
            if (auto state = weak.lock(); state && !state->retired && !state->failure && state->module.Identity().package == package &&
                                          state->module.Module().id == module && state->module.Identity().generation == generation) {
                const auto snapshot = state->composition->Snapshot();
                return snapshot ? snapshot->importers : nullptr;
            }
        return {};
    }
}  // namespace Horo::Packages
