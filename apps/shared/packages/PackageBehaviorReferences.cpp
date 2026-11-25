#include "PackageBehaviorReferences.h"

#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Packages/PackageLifecycle.h"
#include "Horo/Packages/PackageMigration.h"
#include "Horo/Prefab/PrefabDocument.h"
#include "Horo/Scene/SceneSource.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <map>
#include <ranges>
#include <set>

namespace Horo::Application::Internal {
    namespace {
        /** @brief One saved attachment with its package claim and authored identity. */
        struct PackageBehaviorReference final {
            Packages::HoroPackageId package;
            Gameplay::BehaviorTypeId type;
            std::string assetPath;
            std::uint64_t object;
            Gameplay::BehaviorInstanceId instance;
        };

        using Owners = std::map<Gameplay::BehaviorTypeId, std::set<Packages::HoroPackageId>>;
        using References = std::vector<PackageBehaviorReference>;
        constexpr std::size_t MaximumBehaviorReferences = 16'384;
        constexpr std::size_t MaximumServiceReferences = 16'384;
        constexpr auto ServiceInUse = "packages.host.service_in_use";
        using Registration = Packages::GameplayPackageModuleHost::RegistrationInventory;
        using ServiceOwners = std::map<Gameplay::GameplayServiceId, std::set<Packages::HoroPackageId>>;
        using LockedPackages = std::map<Packages::HoroPackageId, const Packages::LockedPackage *>;

        /** @brief Exact departing provider identities or inert behavior claims for one saved-source scan. */
        struct SavedReferenceOwners final {
            Owners behaviors;
            std::set<Packages::HoroPackageId> senses;

            /** @brief Reports whether this scan has an observed or declared provider to inspect. */
            [[nodiscard]] bool Empty() const {
                return behaviors.empty() && senses.empty();
            }
        };

        /** @brief Bounded matches from the same captured Scene and Prefab bytes. */
        struct SavedReferenceScan final {
            References behaviors;
            std::vector<Diagnostic> senses;
        };

        Error ReferenceError(std::string code, std::string_view path = {}) {
            return {ErrorCode{std::move(code)}, ErrorDomainId{"packages.host"}, ErrorSeverity::Error, std::string{path}};
        }

        /** @brief Tests membership in the exact withdrawal plan. */
        bool IsDeparting(const Packages::PackageRemovalGraphPreview &removal, const Packages::HoroPackageId &package) {
            return std::ranges::find(removal.removed, package, &Packages::LockedPackageReference::package) != removal.removed.end();
        }

        /** @brief Accepts only registrations attributed to the exact locked artifact being withdrawn. */
        Result<SavedReferenceOwners> IndexOwners(std::span<const Registration> registrations, const Packages::ValidatedPackageLockfileV1 &lock,
                                                const Packages::PackageRemovalGraphPreview &removal) {
            using Indexed = Result<SavedReferenceOwners>;
            SavedReferenceOwners owners;
            for (const auto &registration : registrations) {
                if (!IsDeparting(removal, registration.package))
                    continue;
                if (const auto entry = std::ranges::find(lock.Packages(), registration.package, &Packages::LockedPackage::package);
                    entry == lock.Packages().end() || entry->version != registration.version ||
                    entry->artifactDigest != registration.artifact)
                    return Indexed::Failure(ReferenceError("packages.host.behavior_ownership_stale"));
                for (const auto &type : registration.behaviors)
                    owners.behaviors[type].insert(registration.package);
                if (registration.hasCustomSenses)
                    owners.senses.insert(registration.package);
            }
            return Indexed::Success(std::move(owners));
        }

        /** @brief Records a document that can still bind an unclassified Perception implementation. */
        Result<void> CollectSenseReview(SavedReferenceScan &scan, const SavedReferenceOwners &owners, std::string_view path) {
            if (owners.senses.size() > MaximumBehaviorReferences - scan.senses.size())
                return Result<void>::Failure(ReferenceError("packages.host.perception_reference_limit", path));
            for (const auto &package : owners.senses)
                scan.senses.emplace_back(DiagnosticCode{"packages.host.perception_reference_review"}, DiagnosticSeverity::Error,
                                         package.Value(), SourceLocation{std::string{path}}, "");
            return Result<void>::Success();
        }

        /** @brief Detects unresolved Scene composition and authored AI or gameplay content, including disabled owners. */
        bool RequiresSenseReview(const SceneSource::SceneSourceDocument &scene) {
            return !scene.prefabInstances.empty() || !scene.childScenes.empty() ||
                   std::ranges::any_of(scene.objects, [](const auto &object) {
                       const auto &components = object.components;
                       return components.aiAgent || components.aiController || !components.behaviors.empty() ||
                              !components.gameplayComponents.empty();
                   });
        }

        /** @brief Detects unresolved composition, asset bindings and opaque component envelopes. */
        bool RequiresSenseReview(const Prefab::PrefabDocumentData &prefab) {
            return prefab.composition || !prefab.referencedAssets.empty() ||
                   std::ranges::any_of(prefab.objects, [](const auto &object) {
                       return !object.behaviors.empty() || !object.components.empty();
                   });
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
        Result<void> InspectScene(SavedReferenceScan &scan, const SavedReferenceOwners &owners, std::string_view path, std::string_view text) {
            const auto scene = SceneSource::DecodeSceneSourceWithChildReferences(text);
            if (scene.HasError())
                return Result<void>::Failure(
                    WithCause(ReferenceError("packages.host.scene_reference_unreadable", path), scene.ErrorValue()));
            if (!scene.Value().unsupportedComponentOwners.empty())
                return Result<void>::Failure(ReferenceError("packages.host.scene_reference_unsupported", path));
            for (const auto &object : scene.Value().objects)
                if (auto collected = Collect(scan.behaviors, owners.behaviors, path, object.id.value, object.components.behaviors);
                    collected.HasError())
                    return collected;
            return !owners.senses.empty() && RequiresSenseReview(scene.Value()) ? CollectSenseReview(scan, owners, path)
                                                                                 : Result<void>::Success();
        }

        /** @brief Reads direct prefab attachments, retaining both asset and prefab-local object identity. */
        Result<void> InspectPrefab(SavedReferenceScan &scan, const SavedReferenceOwners &owners, const Assets::AssetRecord &record, std::string_view text,
                                   const Prefab::PrefabLimitProfile &limits) {
            const auto path = record.sourcePath.String();
            const auto prefab = Prefab::PrefabDocument::Parse(text, limits);
            if (prefab.HasError())
                return Result<void>::Failure(
                    WithCause(ReferenceError("packages.host.prefab_reference_unreadable", path), prefab.ErrorValue()));
            if (prefab.Value().Data().assetId != record.id)
                return Result<void>::Failure(ReferenceError("packages.host.prefab_reference_identity_mismatch", path));
            const auto &data = prefab.Value().Data();
            for (const auto &object : data.objects)
                if (auto collected = Collect(scan.behaviors, owners.behaviors, path, object.localId.value, object.behaviors); collected.HasError())
                    return collected;
            return !owners.senses.empty() && RequiresSenseReview(data) ? CollectSenseReview(scan, owners, path)
                                                                       : Result<void>::Success();
        }

        /** @brief Shares the original deadline across bounded descendant reads and the resulting observations. */
        Result<SavedReferenceScan> InspectAssets(const Platform::IProjectReadFiles &files, const Platform::ProjectReadContext &context,
                                                const Assets::AssetRegistryCandidate &candidate, const SavedReferenceOwners &owners) {
            using Inspection = Result<SavedReferenceScan>;
            const auto limits = Prefab::PrefabLimitProfile::Create({});
            if (limits.HasError())
                return Inspection::Failure(limits.ErrorValue());
            SavedReferenceScan scan;
            for (const auto &record : candidate.records) {
                const auto type = record.type.Value();
                if (type != "core.scene" && type != "core.prefab")
                    continue;
                const auto source = files.SourceText(record.sourcePath.String(), context, {});
                if (source.HasError())
                    return Inspection::Failure(
                        WithCause(ReferenceError("packages.host.behavior_reference_unreadable", record.sourcePath.String()),
                                  source.ErrorValue()));
                const auto inspected = type == "core.scene"
                                           ? InspectScene(scan, owners, record.sourcePath.String(), *source.Value().text)
                                           : InspectPrefab(scan, owners, record, *source.Value().text, limits.Value());
                if (inspected.HasError())
                    return Inspection::Failure(inspected.ErrorValue());
            }
            if (const auto current = Platform::CheckProjectReadContext(context); current.HasError())
                return Inspection::Failure(current.ErrorValue());
            return Inspection::Success(std::move(scan));
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

        /** @brief Checks that a verified graph contains the entire locked identity and its archive. */
        const Packages::PackageRestorePackage *FindExactArchive(const Packages::PackageRestoreGraph &graph,
                                                                 const Packages::LockedPackage &locked) {
            const auto actual = std::ranges::find_if(graph.packages, [&locked](const auto &entry) {
                return entry.lock.package == locked.package;
            });
            return actual != graph.packages.end() && actual->archive && actual->lock == locked ? &*actual : nullptr;
        }

        /** @brief Allows only data files in the asset namespace along with the package manifest. */
        bool IsOfflineAssetArchive(const Packages::PackageRestorePackage &package) {
            return std::ranges::all_of(package.archive->Manifest().Entries(), [](const auto &file) {
                const auto &path = file.id.path.Value();
                return !file.executable &&
                       (path == "horo-package.toml" || path == Packages::PackageAssetRenameDeclarationPath || path.starts_with("assets/"));
            });
        }

        /** @brief Separates data archives from executable ones and verifies the editor's supported executable owner. */
        Result<std::vector<Packages::PackageRuntimeRoleDeclaration>> DeclaredRemovalRoles(const Packages::VerifiedPackageInstallRecord &install,
                                                                                       const Packages::PackageRestorePackage &package,
                                                                                       const bool requireGameplayOwner) {
            using Roles = Result<std::vector<Packages::PackageRuntimeRoleDeclaration>>;
            if (!requireGameplayOwner)
                return Packages::InspectPackageRuntimeRoles(install, package.lock.package);
            if (!OfflineRemovalContributionBlocker(package.lock) && IsOfflineAssetArchive(package))
                return Roles::Success({});
            auto roles = Packages::InspectGameplayRemovalRoles(package);
            if (roles.HasError())
                return Roles::Failure(WithCause(ReferenceError("packages.host.removal_requires_lifecycle_owner", package.lock.package.Value()),
                                                roles.ErrorValue()));
            return roles;
        }

        /** @brief Reads declared roles of one exact locked artifact and collects missing observations. */
        Result<void> InspectRuntimeCoverage(const PackageRemovalSources &sources, const Packages::PackageRestoreGraph &graph,
                                            const Packages::VerifiedPackageInstallRecord &install, const Packages::LockedPackage &locked,
                                            const bool requireGameplayOwner, std::vector<Diagnostic> &missing) {
            if (auto authority = Platform::CheckProjectReadContext(sources.context); authority.HasError())
                return authority;
            const auto *package = FindExactArchive(graph, locked);
            if (!package)
                return Result<void>::Failure(ReferenceError("packages.host.removal_archive_evidence_unavailable", locked.package.Value()));
            const auto declared = DeclaredRemovalRoles(install, *package, requireGameplayOwner);
            if (declared.HasError())
                return Result<void>::Failure(declared.ErrorValue());
            if (!declared.Value().empty() && sources.registrationFailure)
                return Result<void>::Failure(WithCause(
                    ReferenceError("packages.host.runtime_registration_unavailable", locked.package.Value()), *sources.registrationFailure));
            for (const auto &role : declared.Value()) {
                const auto observed = std::ranges::find_if(sources.registrations, [&](const auto &entry) {
                    return MatchesLockedArtifact(entry, locked) && entry.moduleId == role.moduleId &&
                           entry.descriptorPath == role.descriptor.Value();
                });
                if (observed == sources.registrations.end())
                    missing.emplace_back(DiagnosticCode{"packages.host.runtime_registration_unobserved"}, DiagnosticSeverity::Error,
                                         locked.package.Value(), SourceLocation{role.descriptor.Value()}, role.moduleId);
                else if (requireGameplayOwner && observed->hasAdditionalReferenceClasses)
                    return Result<void>::Failure(
                        ReferenceError("packages.host.removal_reference_coverage_unavailable", locked.package.Value()));
            }
            return Result<void>::Success();
        }

        /** @brief Accepts only exact staged archives that contain no executable or extension contribution. */
        Result<void> CheckAssetOnlyRemovalCoverage(const std::shared_ptr<const Packages::PackageRestoreGraph> &graph,
                                                   const Packages::ValidatedPackageLockfileV1 &lock,
                                                   const Packages::PackageRemovalGraphPreview &removal) {
            if (!graph || graph->requestHash != lock.RequestHash())
                return Result<void>::Failure(ReferenceError("packages.host.removal_archive_evidence_unavailable"));
            for (const auto &expected : lock.Packages()) {
                if (!IsDeparting(removal, expected.package))
                    continue;
                const auto *package = FindExactArchive(*graph, expected);
                if (!package)
                    return Result<void>::Failure(
                        ReferenceError("packages.host.removal_archive_evidence_unavailable", expected.package.Value()));
                if (OfflineRemovalContributionBlocker(expected) || !IsOfflineAssetArchive(*package))
                    return Result<void>::Failure(
                        ReferenceError("packages.host.removal_requires_lifecycle_owner", expected.package.Value()));
            }
            return Result<void>::Success();
        }
        /** @brief Reads the sealed archives from the removal reservation and checks required registration coverage. */
        Result<void> CheckLiveRemovalCoverage(const PackageRemovalSources &sources, const Packages::ValidatedPackageLockfileV1 &lock,
                                              const Packages::PackageRemovalGraphPreview &removal) {
            if (!sources.live)
                return Result<void>::Failure(ReferenceError("packages.host.removal_archive_evidence_unavailable"));
            const auto install = sources.live->InstalledRecord();
            const auto graph = install->Graph();
            if (!graph || graph->requestHash != lock.RequestHash())
                return Result<void>::Failure(ReferenceError("packages.host.removal_archive_evidence_unavailable"));
            Error missing = ReferenceError("packages.host.runtime_registration_unobserved");
            const bool inspectRetained = HasDepartingService(sources.registrations, lock, removal);
            for (const auto &entry : lock.Packages()) {
                const bool departing = IsDeparting(removal, entry.package);
                if (!inspectRetained && !departing)
                    continue;
                if (auto checked = InspectRuntimeCoverage(sources, *graph, *install, entry, sources.requireGameplayOwner, missing.diagnostics);
                    checked.HasError())
                    return checked;
            }
            if (!missing.diagnostics.empty())
                return Result<void>::Failure(std::move(missing));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc OfflineRemovalContributionBlocker */
    std::optional<Diagnostic> OfflineRemovalContributionBlocker(const Packages::LockedPackage &package) {
        if (std::ranges::equal(package.contributions, std::array<std::string_view, 1>{"assets"}))
            return std::nullopt;
        return Diagnostic{DiagnosticCode{"packages.host.removal_requires_lifecycle_owner"}, DiagnosticSeverity::Error,
                          package.package.Value(), SourceLocation{"packages.host"}, package.package.Value()};
    }

    /** @copydoc CheckRemovalRegistrationCoverage */
    Result<void> CheckRemovalRegistrationCoverage(const PackageRemovalSources &sources, const Packages::ValidatedPackageLockfileV1 &lock,
                                                  const Packages::PackageRemovalGraphPreview &removal) {
        if (removal.removed.empty())
            return Result<void>::Success();
        if (auto valid = ValidateWithdrawal(lock, removal); valid.HasError())
            return valid;
        if (sources.offline)
            return CheckAssetOnlyRemovalCoverage(sources.offline->Graph(), lock, removal);
        return CheckLiveRemovalCoverage(sources, lock, removal);
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

    /** @brief Scans saved sources against either observed registrations or inert preview claims. */
    static Result<SavedReferenceScan> ScanSavedReferences(const Assets::IAssetRegistrySource &assets,
                                                          const Platform::IProjectReadFiles &files, Platform::ProjectReadContext context,
                                                          const SavedReferenceOwners &owners) {
        using Inspection = Result<SavedReferenceScan>;
        if (owners.Empty())
            return Inspection::Success({});
        context.deadline =
            std::min(context.deadline, std::chrono::steady_clock::now() + Platform::ProjectSourceReadLimits{}.maximumDuration);
        const auto candidate = Assets::PrepareAssetRegistryCandidate(assets);
        if (candidate.HasError())
            return Inspection::Failure(candidate.ErrorValue());
        if (candidate.Value().status != Assets::AssetRegistryBuildStatus::Complete)
            return Inspection::Failure(ReferenceError("packages.host.asset_inventory_incomplete"));
        return InspectAssets(files, context, candidate.Value(), owners);
    }

    /** @copydoc InspectPackageScenePrefabReferences */
    Result<std::vector<Diagnostic>> InspectPackageScenePrefabReferences(
        const PackageRemovalSources &sources,
        const Packages::ValidatedPackageLockfileV1 &lock, const Packages::PackageRemovalGraphPreview &removal) {
        using Inspection = Result<std::vector<Diagnostic>>;
        const auto owners = IndexOwners(sources.registrations, lock, removal);
        if (owners.HasError())
            return Inspection::Failure(owners.ErrorValue());
        auto inspected = ScanSavedReferences(sources.assets, sources.files, sources.context, owners.Value());
        if (inspected.HasError())
            return Inspection::Failure(inspected.ErrorValue());
        auto scan = std::move(inspected).Value();
        std::vector<Diagnostic> diagnostics;
        diagnostics.reserve(scan.behaviors.size() + scan.senses.size());
        for (const auto &reference : scan.behaviors)
            diagnostics.emplace_back(DiagnosticCode{"packages.host.behavior_in_use"}, DiagnosticSeverity::Error,
                                     reference.package.Value() + ": " + reference.type.Value(), SourceLocation{reference.assetPath},
                                     std::to_string(reference.object) + "/" + std::to_string(reference.instance.value));
        std::ranges::move(scan.senses, std::back_inserter(diagnostics));
        return Inspection::Success(std::move(diagnostics));
    }

    /** @copydoc InspectPackageBehaviorMigrations */
    Result<std::vector<Diagnostic>> InspectPackageBehaviorMigrations(
        const Assets::IAssetRegistrySource &assets, const Platform::IProjectReadFiles &files, Platform::ProjectReadContext context,
        std::span<const PackageBehaviorMigration> claims, std::size_t diagnosticLimit) {
        using Inspection = Result<std::vector<Diagnostic>>;
        SavedReferenceOwners assertedOwners;
        std::map<std::pair<Packages::HoroPackageId, Gameplay::BehaviorTypeId>, const Gameplay::BehaviorTypeId *> targets;
        for (const auto &claim : claims) {
            assertedOwners.behaviors[claim.previous].insert(claim.package);
            targets.emplace(std::pair{claim.package, claim.previous}, &claim.candidate);
        }
        const auto matches = ScanSavedReferences(assets, files, std::move(context), assertedOwners);
        if (matches.HasError())
            return Inspection::Failure(matches.ErrorValue());
        std::vector<Diagnostic> diagnostics;
        for (const auto &match : matches.Value().behaviors | std::views::take(diagnosticLimit)) {
            const auto *target = targets.at({match.package, match.type});
            auto message = match.package.Value() + ": " + match.type.Value() + " -> " + target->Value() +
                           " (object " + std::to_string(match.object) + ", attachment " + std::to_string(match.instance.value) + ')';
            diagnostics.emplace_back(DiagnosticCode{"packages.host.update_possible_behavior_rename"}, DiagnosticSeverity::Note,
                                     std::move(message), SourceLocation{"packages.host"}, match.assetPath);
        }
        return Inspection::Success(std::move(diagnostics));
    }
}  // namespace Horo::Application::Internal
