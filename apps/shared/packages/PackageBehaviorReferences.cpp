#include "PackageBehaviorReferences.h"

#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Packages/PackageLifecycle.h"
#include "Horo/Prefab/PrefabDocument.h"
#include "Horo/Scene/SceneSource.h"

#include <algorithm>
#include <array>
#include <map>
#include <set>

namespace Horo::Application::Internal {
    namespace {
        using Owners = std::map<Gameplay::BehaviorTypeId, std::set<Packages::HoroPackageId>>;
        using References = std::vector<PackageBehaviorReference>;
        constexpr std::size_t MaximumBehaviorReferences = 16'384;
        constexpr std::size_t MaximumServiceReferences = 16'384;
        constexpr auto ServiceInUse = "packages.host.service_in_use";
        using Registration = Packages::GameplayPackageModuleHost::RegistrationInventory;
        using ServiceOwners = std::map<Gameplay::GameplayServiceId, std::set<Packages::HoroPackageId>>;
        using LockedPackages = std::map<Packages::HoroPackageId, const Packages::LockedPackage *>;

        Error ReferenceError(std::string code, std::string_view path = {}) {
            return {ErrorCode{std::move(code)}, ErrorDomainId{"packages.host"}, ErrorSeverity::Error, std::string{path}};
        }

        /** @brief Tests membership in the exact withdrawal plan. */
        bool IsDeparting(const Packages::PackageRemovalGraphPreview &removal, const Packages::HoroPackageId &package) {
            return std::ranges::find(removal.removed, package, &Packages::LockedPackageReference::package) != removal.removed.end();
        }

        /** @brief Accepts only registrations attributed to the exact locked artifact being withdrawn. */
        Result<Owners> IndexOwners(std::span<const Packages::GameplayPackageModuleHost::RegistrationInventory> registrations,
                                   const Packages::ValidatedPackageLockfileV1 &lock, const Packages::PackageRemovalGraphPreview &removal) {
            Owners owners;
            for (const auto &registration : registrations) {
                if (!IsDeparting(removal, registration.package))
                    continue;
                if (const auto entry = std::ranges::find(lock.Packages(), registration.package, &Packages::LockedPackage::package);
                    entry == lock.Packages().end() || entry->version != registration.version ||
                    entry->artifactDigest != registration.artifact)
                    return Result<Owners>::Failure(ReferenceError("packages.host.behavior_ownership_stale"));
                for (const auto &type : registration.behaviors)
                    owners[type].insert(registration.package);
            }
            return Result<Owners>::Success(std::move(owners));
        }

        /** @brief Keeps every saved attachment, including disabled behavior instances. */
        Result<void> Collect(References &references, const Owners &owners, std::string_view path, std::uint64_t object,
                             std::span<const Gameplay::BehaviorComponent> behaviors) {
            for (const auto &behavior : behaviors) {
                const auto matching = owners.find(behavior.typeId);
                if (matching == owners.end())
                    continue;
                if (matching->second.size() > MaximumBehaviorReferences - references.size())
                    return Result<void>::Failure(ReferenceError("packages.host.behavior_reference_limit", path));
                for (const auto &package : matching->second)
                    references.emplace_back(package, behavior.typeId, std::string{path}, object, behavior.instanceId);
            }
            return Result<void>::Success();
        }

        /** @brief Reads direct authored attachments; each referenced child or prefab has its own registry entry. */
        Result<void> InspectScene(References &references, const Owners &owners, std::string_view path, std::string_view text) {
            const auto scene = SceneSource::DecodeSceneSourceWithChildReferences(text);
            if (scene.HasError())
                return Result<void>::Failure(
                    WithCause(ReferenceError("packages.host.scene_reference_unreadable", path), scene.ErrorValue()));
            if (!scene.Value().unsupportedComponentOwners.empty())
                return Result<void>::Failure(ReferenceError("packages.host.scene_reference_unsupported", path));
            for (const auto &object : scene.Value().objects)
                if (auto collected = Collect(references, owners, path, object.id.value, object.components.behaviors); collected.HasError())
                    return collected;
            return Result<void>::Success();
        }

        /** @brief Reads direct prefab attachments, retaining both asset and prefab-local object identity. */
        Result<void> InspectPrefab(References &references, const Owners &owners, const Assets::AssetRecord &record, std::string_view text,
                                   const Prefab::PrefabLimitProfile &limits) {
            const auto path = record.sourcePath.String();
            const auto prefab = Prefab::PrefabDocument::Parse(text, limits);
            if (prefab.HasError())
                return Result<void>::Failure(
                    WithCause(ReferenceError("packages.host.prefab_reference_unreadable", path), prefab.ErrorValue()));
            if (prefab.Value().Data().assetId != record.id)
                return Result<void>::Failure(ReferenceError("packages.host.prefab_reference_identity_mismatch", path));
            for (const auto &object : prefab.Value().Data().objects)
                if (auto collected = Collect(references, owners, path, object.localId.value, object.behaviors); collected.HasError())
                    return collected;
            return Result<void>::Success();
        }

        /** @brief Shares the original deadline across bounded descendant reads and the resulting observations. */
        Result<References> InspectAssets(const Platform::IProjectReadFiles &files, const Platform::ProjectReadContext &context,
                                         const Assets::AssetRegistryCandidate &candidate, const Owners &owners) {
            const auto limits = Prefab::PrefabLimitProfile::Create({});
            if (limits.HasError())
                return Result<References>::Failure(limits.ErrorValue());
            References references;
            for (const auto &record : candidate.records) {
                const auto type = record.type.Value();
                if (type != "core.scene" && type != "core.prefab")
                    continue;
                const auto source = files.SourceText(record.sourcePath.String(), context, {});
                if (source.HasError())
                    return Result<References>::Failure(
                        WithCause(ReferenceError("packages.host.behavior_reference_unreadable", record.sourcePath.String()),
                                  source.ErrorValue()));
                const auto inspected = type == "core.scene"
                                           ? InspectScene(references, owners, record.sourcePath.String(), *source.Value().text)
                                           : InspectPrefab(references, owners, record, *source.Value().text, limits.Value());
                if (inspected.HasError())
                    return Result<References>::Failure(inspected.ErrorValue());
            }
            if (const auto current = Platform::CheckProjectReadContext(context); current.HasError())
                return Result<References>::Failure(current.ErrorValue());
            return Result<References>::Success(std::move(references));
        }

        /** @brief Records each provider and consumer pair before proceeding to the next requirement. */
        Result<void> CollectServiceReferences(std::vector<Diagnostic> &references, const ServiceOwners &owners,
                                              const Registration &consumer, std::string_view consumerId,
                                              std::span<const Gameplay::GameplayServiceId> requirements) {
            for (const auto &id : requirements) {
                const auto matching = owners.find(id);
                if (matching == owners.end())
                    continue;
                if (matching->second.size() > MaximumServiceReferences - references.size())
                    return Result<void>::Failure(ReferenceError("packages.host.service_reference_limit"));
                for (const auto &provider : matching->second)
                    references.emplace_back(DiagnosticCode{ServiceInUse}, DiagnosticSeverity::Error,
                                            provider.Value() + ": " + std::string{id.Value()}, SourceLocation{consumer.descriptorPath},
                                            consumer.package.Value() + "/" + std::string{consumerId});
            }
            return Result<void>::Success();
        }

        /** @brief Checks both dependency-bearing registration kinds against the same provider identities. */
        Result<void> InspectServiceConsumer(std::vector<Diagnostic> &references, const ServiceOwners &owners,
                                            const Registration &consumer) {
            for (const auto &service : consumer.services)
                if (auto result = CollectServiceReferences(references, owners, consumer, service.id.Value(), service.dependencies);
                    result.HasError())
                    return result;
            for (const auto &system : consumer.systems)
                if (auto result = CollectServiceReferences(references, owners, consumer, system.id.Value(), system.requiredServices);
                    result.HasError())
                    return result;
            return Result<void>::Success();
        }

        /** @brief Confirms that an observed role belongs to the exact locked artifact. */
        bool MatchesLockedArtifact(const Registration &registration, const Packages::LockedPackage &locked) {
            return registration.activationGeneration != 0 && registration.package == locked.package &&
                   registration.version == locked.version && registration.artifact == locked.artifactDigest;
        }

        /** @brief Excludes cached observations from another artifact or without an admitted generation. */
        bool MatchesCurrentLock(const LockedPackages &locked, const Registration &registration) {
            const auto entry = locked.find(registration.package);
            return entry != locked.end() && MatchesLockedArtifact(registration, *entry->second);
        }

        /** @brief A departing service needs evidence from the retained roles that could consume it. */
        bool HasDepartingService(std::span<const Registration> registrations, const Packages::ValidatedPackageLockfileV1 &lock,
                                 const Packages::PackageRemovalGraphPreview &removal) {
            return std::ranges::any_of(registrations, [&](const Registration &entry) {
                if (entry.services.empty() || !IsDeparting(removal, entry.package))
                    return false;
                const auto locked = std::ranges::find(lock.Packages(), entry.package, &Packages::LockedPackage::package);
                return locked != lock.Packages().end() && MatchesLockedArtifact(entry, *locked);
            });
        }

        /** @brief Rejects a withdrawal plan that refers to a different locked graph. */
        Result<void> ValidateWithdrawal(const Packages::ValidatedPackageLockfileV1 &lock,
                                        const Packages::PackageRemovalGraphPreview &removal) {
            for (const auto &reference : removal.removed) {
                if (const auto expected = std::ranges::find(lock.Packages(), reference.package, &Packages::LockedPackage::package);
                    expected == lock.Packages().end() || expected->version != reference.version)
                    return Result<void>::Failure(
                        ReferenceError("packages.host.removal_archive_evidence_unavailable", reference.package.Value()));
            }
            return Result<void>::Success();
        }

        /** @brief Reads declared roles of one exact locked artifact and collects missing observations. */
        Result<void> InspectRuntimeCoverage(const PackageRemovalSources &sources, const Packages::PackageRestoreGraph &graph,
                                            const Packages::LockedPackage &locked, std::vector<Diagnostic> &missing) {
            if (auto authority = Platform::CheckProjectReadContext(sources.context); authority.HasError())
                return authority;
            if (const auto actual = std::ranges::find_if(graph.packages,
                                                         [&locked](const auto &entry) {
                return entry.lock.package == locked.package;
            });
                actual == graph.packages.end() || actual->lock != locked)
                return Result<void>::Failure(ReferenceError("packages.host.removal_archive_evidence_unavailable", locked.package.Value()));
            const auto declared = Packages::InspectPackageRuntimeRoles(*sources.install, locked.package);
            if (declared.HasError())
                return Result<void>::Failure(declared.ErrorValue());
            for (const auto &role : declared.Value()) {
                const auto observed = std::ranges::any_of(sources.registrations, [&](const auto &entry) {
                    return MatchesLockedArtifact(entry, locked) && entry.moduleId == role.moduleId &&
                           entry.descriptorPath == role.descriptor.Value();
                });
                if (!observed)
                    missing.emplace_back(DiagnosticCode{"packages.host.runtime_registration_unobserved"}, DiagnosticSeverity::Error,
                                         locked.package.Value(), SourceLocation{role.descriptor.Value()}, role.moduleId);
            }
            return Result<void>::Success();
        }

        /** @brief Allows only data files in the asset namespace along with the package manifest. */
        bool IsOfflineAssetArchive(const Packages::PackageRestorePackage &package) {
            if (!std::ranges::equal(package.lock.contributions, std::array<std::string_view, 1>{"assets"}))
                return false;
            return std::ranges::all_of(package.archive->Manifest().Entries(), [](const auto &file) {
                const auto &path = file.id.path.Value();
                return !file.executable && (path == "horo-package.toml" || path.starts_with("assets/"));
            });
        }

        /** @brief Accepts only exact staged archives that contain no executable or extension contribution. */
        Result<void> CheckOfflineRemovalCoverage(const Packages::StagedPackageRestoreRecord &staged,
                                                 const Packages::ValidatedPackageLockfileV1 &lock,
                                                 const Packages::PackageRemovalGraphPreview &removal) {
            const auto graph = staged.Graph();
            if (!graph || graph->requestHash != lock.RequestHash())
                return Result<void>::Failure(ReferenceError("packages.host.removal_archive_evidence_unavailable"));
            for (const auto &reference : removal.removed) {
                const auto package = std::ranges::find_if(graph->packages, [&reference](const auto &entry) {
                    return entry.lock.package == reference.package;
                });
                if (package == graph->packages.end() || !package->archive || package->lock.version != reference.version)
                    return Result<void>::Failure(
                        ReferenceError("packages.host.removal_archive_evidence_unavailable", reference.package.Value()));
                if (!IsOfflineAssetArchive(*package))
                    return Result<void>::Failure(
                        ReferenceError("packages.host.removal_requires_lifecycle_owner", reference.package.Value()));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc CheckRemovalRegistrationCoverage */
    Result<void> CheckRemovalRegistrationCoverage(const PackageRemovalSources &sources, const Packages::ValidatedPackageLockfileV1 &lock,
                                                  const Packages::PackageRemovalGraphPreview &removal) {
        if (removal.removed.empty())
            return Result<void>::Success();
        if (sources.offline)
            return CheckOfflineRemovalCoverage(*sources.offline, lock, removal);
        const auto graph = sources.install ? sources.install->Graph() : nullptr;
        if (!graph || graph->requestHash != lock.RequestHash())
            return Result<void>::Failure(ReferenceError("packages.host.removal_archive_evidence_unavailable"));
        if (auto valid = ValidateWithdrawal(lock, removal); valid.HasError())
            return valid;
        Error missing = ReferenceError("packages.host.runtime_registration_unobserved");
        const bool inspectRetained = HasDepartingService(sources.registrations, lock, removal);
        for (const auto &entry : lock.Packages()) {
            if (!inspectRetained && !IsDeparting(removal, entry.package))
                continue;
            if (auto checked = InspectRuntimeCoverage(sources, *graph, entry, missing.diagnostics); checked.HasError())
                return checked;
        }
        if (!missing.diagnostics.empty())
            return Result<void>::Failure(std::move(missing));
        return Result<void>::Success();
    }

    /** @copydoc InspectPackageServiceReferences */
    Result<std::vector<Diagnostic>> InspectPackageServiceReferences(const PackageRemovalSources &sources,
                                                                    const Packages::ValidatedPackageLockfileV1 &lock,
                                                                    const Packages::PackageRemovalGraphPreview &removal) {
        using Inspection = Result<std::vector<Diagnostic>>;
        if (removal.removed.empty())
            return Inspection::Success({});
        LockedPackages locked;
        for (const auto &entry : lock.Packages())
            locked.try_emplace(entry.package, &entry);
        std::set<Packages::HoroPackageId> departing;
        for (const auto &entry : removal.removed)
            departing.insert(entry.package);

        ServiceOwners owners;
        std::vector<const Registration *> retained;
        for (const auto &registration : sources.registrations) {
            if (!MatchesCurrentLock(locked, registration))
                continue;
            if (departing.contains(registration.package)) {
                for (const auto &service : registration.services)
                    owners[service.id].insert(registration.package);
            } else {
                retained.push_back(&registration);
            }
        }
        if (owners.empty())
            return Inspection::Success({});
        std::vector<Diagnostic> references;
        for (const auto *consumer : retained)
            if (auto result = InspectServiceConsumer(references, owners, *consumer); result.HasError())
                return Inspection::Failure(result.ErrorValue());
        return Inspection::Success(std::move(references));
    }

    /** @copydoc InspectPackageBehaviorReferences */
    Result<References> InspectPackageBehaviorReferences(
        const Assets::IAssetRegistrySource &assets, const Platform::IProjectReadFiles &files, Platform::ProjectReadContext context,
        std::span<const Packages::GameplayPackageModuleHost::RegistrationInventory> registrations,
        const Packages::ValidatedPackageLockfileV1 &lock, const Packages::PackageRemovalGraphPreview &removal) {
        const auto owners = IndexOwners(registrations, lock, removal);
        if (owners.HasError())
            return Result<References>::Failure(owners.ErrorValue());
        if (owners.Value().empty())
            return Result<References>::Success({});
        context.deadline =
            std::min(context.deadline, std::chrono::steady_clock::now() + Platform::ProjectSourceReadLimits{}.maximumDuration);
        const auto candidate = Assets::PrepareAssetRegistryCandidate(assets);
        if (candidate.HasError())
            return Result<References>::Failure(candidate.ErrorValue());
        if (candidate.Value().status != Assets::AssetRegistryBuildStatus::Complete)
            return Result<References>::Failure(ReferenceError("packages.host.asset_inventory_incomplete"));
        return InspectAssets(files, context, candidate.Value(), owners.Value());
    }
}  // namespace Horo::Application::Internal
