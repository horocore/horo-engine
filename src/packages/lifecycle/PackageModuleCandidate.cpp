#include "Horo/Packages/PackageLifecycleErrors.h"
#include "PackageModuleCandidateInternal.h"

#include <algorithm>
#include <array>

namespace Horo::Packages::Detail {
    /** @copydoc PackageModuleCandidateFactory::NativeCandidate */
    PackageActivationCandidate PackageModuleCandidateFactory::NativeCandidate(const VerifiedPackageModule &module) {
        const auto &state = *module.state_;
        auto plan = state.plan;
        const auto selected = std::ranges::find(plan.moduleIds, module.Module().id);
        const auto entry = plan.selectedEntries[static_cast<std::size_t>(selected - plan.moduleIds.begin())];
        plan.moduleIds = {module.Module().id};
        plan.selectedEntries = {entry};
        std::erase_if(plan.contributions, [&module](const auto &value) {
            return value.owningModule != module.Module().id;
        });
        return {state.install,       state.packageIndex, state.descriptor, *state.manifest, std::move(plan), state.trust, {},
                {module.Module().id}};
    }

    namespace {
        /** @brief Compatibility-bound metadata and artifact identity; no executable callback has run. */
        struct PreparedRole final {
            std::size_t packageIndex;
            std::shared_ptr<const Extensions::ExtensionManifest> manifest;
            std::size_t moduleIndex;
            Extensions::ExtensionModulePlan plan;
            PackagePath artifact;
        };

        struct CompatibleRole final {
            Extensions::ExtensionModulePlan plan;
            Extensions::ExtensionModuleCompatibility compatibility;
        };

        /** @brief Resolves the exact selected module and its host tuple before artifact or trust admission. */
        Result<CompatibleRole> CompatiblePlan(const Extensions::ExtensionManifest &manifest,
                                              const Extensions::ExtensionModuleManifest &module,
                                              const PackageLifecycleConfiguration &configuration) {
            std::vector<std::string_view> capabilities;
            for (const auto &capability : configuration.capabilities)
                capabilities.push_back(capability);
            const auto environment = PackageHostEnvironment(configuration, capabilities);
            const std::array selected{module.id};
            auto plan = Extensions::ResolveExtensionModules(manifest, environment, selected);
            if (plan.HasError())
                return Result<CompatibleRole>::Failure(plan.ErrorValue());
            if (std::ranges::find(plan.Value().moduleIds, module.id) == plan.Value().moduleIds.end())
                return Result<CompatibleRole>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
            auto compatible = Extensions::EvaluateExtensionModuleCompatibility(manifest, module, environment);
            if (!compatible.IsCompatible())
                return Result<CompatibleRole>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate, compatible.requirement));
            return Result<CompatibleRole>::Success({std::move(plan).Value(), std::move(compatible)});
        }

        /** @brief Validates the immutable descriptor, role, compatibility and declared contribution-root artifact. */
        Result<PreparedRole> PrepareRole(const VerifiedPackageInstallRecord &install, const EnabledPackageModule &request,
                                         const PackageLifecycleConfiguration &configuration) {
            const auto &packages = install.Graph()->packages;
            const auto package = std::ranges::find(packages, request.package, [](const auto &value) {
                return value.lock.package;
            });
            if (package == packages.end())
                return Result<PreparedRole>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
            auto declared = ReadDeclaredPackageDescriptor(*package, request.descriptor);
            if (declared.HasError())
                return Result<PreparedRole>::Failure(declared.ErrorValue());
            auto manifest = std::make_shared<const Extensions::ExtensionManifest>(std::move(declared.Value().manifest));
            const auto module = std::ranges::find(manifest->modules, request.module, &Extensions::ExtensionModuleManifest::id);
            if (module == manifest->modules.end() || std::ranges::find(module->roles, request.role) == module->roles.end())
                return Result<PreparedRole>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
            auto plan = CompatiblePlan(*manifest, *module, configuration);
            if (plan.HasError())
                return Result<PreparedRole>::Failure(plan.ErrorValue());
            auto artifact =
                PackagePath::Parse(declared.Value().root + '/' + SelectedPackageNativeEntry(plan.Value().compatibility.selectedEntry));
            if (artifact.HasError())
                return Result<PreparedRole>::Failure(artifact.ErrorValue());
            const auto entries = package->archive->Manifest().Entries();
            const auto file = std::ranges::find(entries, artifact.Value().Value(), [](const auto &item) {
                return item.id.path.Value();
            });
            if (file == entries.end() || !file->contributionRoot || file->contributionRoot->Value() != declared.Value().root)
                return Result<PreparedRole>::Failure(MakeError(PackageLifecycleErrors::InvalidCandidate));
            const auto packageIndex = static_cast<std::size_t>(package - packages.begin());
            const auto moduleIndex = static_cast<std::size_t>(module - manifest->modules.begin());
            return Result<PreparedRole>::Success(
                {packageIndex, std::move(manifest), moduleIndex, std::move(plan).Value().plan, std::move(artifact).Value()});
        }

        /** @brief Approves exact digests before issuing revocable, permission-intersected capability handles. */
        Result<std::vector<Extensions::ExtensionCapabilityHandle>> AdmitRole(
            const PackageRestorePackage &package, const PreparedRole &role, const EnabledPackageModule &request,
            const std::uint64_t generation, const IPackageExtensionTrust &trust,
            std::optional<Extensions::ExtensionCapabilityAdmission> &admission, PackageExtensionTrustApproval &approval) {
            auto approved = trust.Approve(package, *role.manifest);
            if (approved.HasError())
                return Result<std::vector<Extensions::ExtensionCapabilityHandle>>::Failure(approved.ErrorValue());
            approval = std::move(approved).Value();
            if (approval.artifactDigest != package.archive->Digest() || approval.manifestDigest != package.archive->PackageManifestDigest())
                return Result<std::vector<Extensions::ExtensionCapabilityHandle>>::Failure(
                    MakeError(PackageLifecycleErrors::TrustRequired));
            const auto &module = role.manifest->modules[role.moduleIndex];
            Extensions::ExtensionAdmissionRequest grant{role.manifest->id, module.id, generation, {}};
            for (const auto &capability : module.requiredCapabilities) {
                Extensions::ExtensionCapabilityRequest required{{capability}, {}};
                if (request.role == Extensions::ExtensionModuleRole::RuntimeParticipant && capability == "runtime.participate")
                    required.requiredPermissions.push_back({"runtime.participate"});
                grant.capabilities.push_back(std::move(required));
            }
            auto accepted = Extensions::ExtensionCapabilityAdmission::Evaluate(grant, approval.policy);
            if (accepted.HasError())
                return Result<std::vector<Extensions::ExtensionCapabilityHandle>>::Failure(accepted.ErrorValue());
            admission.emplace(std::move(accepted).Value());
            std::vector<Extensions::ExtensionCapabilityHandle> grants;
            for (const auto &capability : module.requiredCapabilities) {
                auto handle = admission->Grant({capability});
                if (handle.HasError())
                    return Result<std::vector<Extensions::ExtensionCapabilityHandle>>::Failure(handle.ErrorValue());
                grants.push_back(std::move(handle).Value());
            }
            return Result<std::vector<Extensions::ExtensionCapabilityHandle>>::Success(std::move(grants));
        }

        /** @brief Retains declared and required service providers in canonical identity order. */
        std::vector<std::string> RequiredProviders(const PreparedRole &role) {
            const auto &module = role.manifest->modules[role.moduleIndex];
            auto required = module.dependencies;
            for (const auto &service : role.plan.serviceImports)
                if (service.ConsumerModuleId() == module.id && service.IsRequired() && service.ProviderModuleId() != module.id)
                    required.push_back(service.ProviderModuleId());
            std::ranges::sort(required);
            required.erase(std::unique(required.begin(), required.end()), required.end());
            return required;
        }
    }  // namespace

    /** @copydoc PackageModuleCandidateFactory::Prepare */
    Result<VerifiedPackageModule> PackageModuleCandidateFactory::Prepare(
        std::shared_ptr<const VerifiedPackageInstallRecord> install, const EnabledPackageModule &request,
        const PackageLifecycleConfiguration &configuration, const IPackageExtensionTrust &trust, const std::uint64_t generation,
        const std::string_view host, std::optional<Extensions::ExtensionCapabilityAdmission> &admission) {
        auto prepared = PrepareRole(*install, request, configuration);
        if (prepared.HasError())
            return Result<VerifiedPackageModule>::Failure(prepared.ErrorValue());
        auto role = std::move(prepared).Value();
        PackageExtensionTrustApproval approval;
        auto grants = AdmitRole(install->Graph()->packages[role.packageIndex], role, request, generation, trust, admission, approval);
        if (grants.HasError())
            return Result<VerifiedPackageModule>::Failure(grants.ErrorValue());
        const auto &module = role.manifest->modules[role.moduleIndex];
        PackageModuleReport identity{request.package,
                                     module.id,
                                     request.role,
                                     std::string(host),
                                     generation,
                                     PackageModuleOutcome::Compatible,
                                     {},
                                     {PackageModuleOutcome::Installed, PackageModuleOutcome::Enabled, PackageModuleOutcome::Trusted,
                                      PackageModuleOutcome::Compatible}};
        auto required = RequiredProviders(role);
        auto state = std::make_shared<VerifiedPackageModule::State>(
            VerifiedPackageModule::State{std::move(install), role.packageIndex, request.descriptor, std::move(role.artifact),
                                         std::move(role.manifest), role.moduleIndex, std::move(required), std::move(approval),
                                         std::move(role.plan), std::move(identity), admission->ActivationLease(),
                                         std::move(grants).Value()});
        return Result<VerifiedPackageModule>::Success(VerifiedPackageModule(std::move(state)));
    }
}  // namespace Horo::Packages::Detail
