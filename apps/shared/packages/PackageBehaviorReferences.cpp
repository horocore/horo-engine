#include "PackageBehaviorReferences.h"

#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Packages/PackageLifecycle.h"
#include "Horo/Packages/PackageMigration.h"
#include "Horo/Prefab/PrefabDocument.h"
#include "Horo/Scene/SceneSource.h"

#include <algorithm>
#include <array>
#include <functional>
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
        using ComponentOwners = std::map<Gameplay::ComponentTypeId, std::set<Packages::HoroPackageId>>;
        constexpr std::size_t MaximumBehaviorReferences = 16'384;
        constexpr std::size_t MaximumServiceReferences = 16'384;
        constexpr std::size_t MaximumCoverageDiagnostics = 16'384;
        constexpr auto ServiceInUse = "packages.host.service_in_use";
        using Registration = Packages::GameplayPackageModuleHost::RegistrationInventory;
        using ServiceOwners = std::map<Gameplay::GameplayServiceId, std::set<Packages::HoroPackageId>>;
        using LockedPackages = std::map<Packages::HoroPackageId, const Packages::LockedPackage *>;
        constexpr std::string_view SceneAssetType = "core.scene";
        constexpr std::string_view PrefabAssetType = "core.prefab";

        /** @brief Exact departing provider identities or inert behavior claims for one saved-source scan. */
        struct SavedReferenceOwners final {
            Owners behaviors;
            ComponentOwners components;
            std::set<Packages::HoroPackageId> senses;

            /** @brief Reports whether this scan has an observed or declared provider to inspect. */
            [[nodiscard]] bool Empty() const {
                return behaviors.empty() && components.empty() && senses.empty();
            }
        };

        /** @brief Bounded matches from the same captured Scene and Prefab bytes. */
        struct SavedReferenceScan final {
            References behaviors;
            std::map<std::string, std::string, std::less<>> behaviorSourceRevisions;
            std::vector<Diagnostic> senses;
            std::vector<Diagnostic> components;
            std::vector<Diagnostic> composition;
            PackageSourceReviewFailures failures;
        };

        /** @brief Pins the same ordered asset inventory for removal and scopes migration to direct attachments. */
        struct SavedReferenceReview final {
            const SavedReferenceOwners &owners;
            std::span<const Assets::AssetRecord> compositionCatalog;
        };

        Error ReferenceError(std::string code, std::string_view path = {}) {
            return {ErrorCode{std::move(code)}, ErrorDomainId{"packages.host"}, ErrorSeverity::Error, std::string{path}};
        }

        /** @brief Tests membership in the exact withdrawal plan. */
        bool IsDeparting(const Packages::PackageRemovalGraphPreview &removal, const Packages::HoroPackageId &package) {
            return std::ranges::find(removal.removed, package, &Packages::LockedPackageReference::package) != removal.removed.end();
        }

        /** @brief Confirms that an observed role belongs to the exact locked artifact. */
        bool MatchesLockedArtifact(const Registration &registration, const Packages::LockedPackage &locked) {
            return registration.activationGeneration != 0 && registration.package == locked.package &&
                   registration.version == locked.version && registration.artifact == locked.artifactDigest;
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
                    entry == lock.Packages().end() || !MatchesLockedArtifact(registration, *entry))
                    return Indexed::Failure(ReferenceError("packages.host.behavior_ownership_stale"));
                for (const auto &type : registration.behaviors)
                    owners.behaviors[type].insert(registration.package);
                for (const auto &type : registration.components)
                    owners.components[type].insert(registration.package);
                if (registration.hasCustomSenses)
                    owners.senses.insert(registration.package);
            }
            return Indexed::Success(std::move(owners));
        }

        /** @brief Records exact component usage without interpreting serialized payload bytes. */
        struct ComponentReferences final {
            std::vector<Diagnostic> &diagnostics;
            const ComponentOwners &owners;
            std::string_view path;
            std::optional<std::uint64_t> placement{};

            /** @brief Keeps a matching object, placement and optional component instance in authored address order. */
            Result<void> Collect(const Gameplay::ComponentTypeId &type, std::uint64_t object, std::optional<std::uint64_t> instance = {},
                                 std::span<const Prefab::LocalObjectId> scope = {}) const {
                const auto matched = owners.find(type);
                if (matched == owners.end())
                    return Result<void>::Success();
                if (matched->second.size() > MaximumBehaviorReferences - diagnostics.size())
                    return Result<void>::Failure(ReferenceError("packages.host.component_reference_limit", path));
                std::string target;
                if (placement)
                    target = std::to_string(*placement) + '/';
                for (const auto step : scope)
                    target += std::to_string(step.value) + '/';
                target += std::to_string(object);
                if (instance)
                    target += '/' + std::to_string(*instance);
                for (const auto &package : matched->second)
                    diagnostics.emplace_back(DiagnosticCode{"packages.host.component_in_use"}, DiagnosticSeverity::Error,
                                             package.Value() + " / " + type.Value(), SourceLocation{std::string{path}}, target);
                return Result<void>::Success();
            }

            /** @brief Records a property, add or remove component target in its owning prefab scope. */
            Result<void> Collect(const Gameplay::ComponentTypeId &type, const Prefab::PrefabObjectAddress &object,
                                 const Prefab::PrefabComponentInstanceId instance) const {
                return Collect(type, object.SourceObject().value, instance.Value(), object.NestedInstanceScope());
            }

            Result<void> operator()(const Prefab::PrefabValueOverrideRecord &value) const {
                const auto *target = std::get_if<Prefab::PrefabPropertyAddress>(&value.target);
                return target ? Collect(target->ComponentType(), target->Object(), target->ComponentInstance()) : Result<void>::Success();
            }

            Result<void> operator()(const Prefab::PrefabComponentOverrideRecord &record) const {
                if (const auto *added = std::get_if<Prefab::PrefabAddComponentOverride>(&record))
                    return Collect(added->payload.component.typeId, added->object, added->payload.instance);
                const auto &target = std::get<Prefab::PrefabRemoveComponentOverride>(record).target;
                return Collect(target.type, target.object, target.instance);
            }
        };

        /** @brief Visits stored intent including original records retained as conflicts and orphans. */
        Result<void> InspectComponentOverrides(const std::optional<Prefab::PrefabValueOverrideSet> &overrides,
                                               const ComponentReferences &references) {
            if (!overrides || references.owners.empty())
                return Result<void>::Success();
            const auto &set = *overrides;
            const auto scan = [&references](const auto &records, auto projection) -> Result<void> {
                for (const auto &record : records)
                    if (auto found = references(std::invoke(projection, record)); found.HasError())
                        return found;
                return Result<void>::Success();
            };
            if (auto result = scan(set.records, std::identity{}); result.HasError())
                return result;
            if (auto result = scan(set.conflicts, &Prefab::PrefabValueOverrideConflict::original); result.HasError())
                return result;
            if (auto result = scan(set.orphans, &Prefab::PrefabValueOverrideOrphan::original); result.HasError())
                return result;
            if (auto result = scan(set.componentRecords, std::identity{}); result.HasError())
                return result;
            if (auto result = scan(set.componentConflicts, &Prefab::PrefabComponentOverrideConflict::original); result.HasError())
                return result;
            return scan(set.componentOrphans, &Prefab::PrefabComponentOverrideOrphan::original);
        }

        /** @brief Reads component intents stored outside the prefab's direct object list. */
        Result<void> InspectPrefabComposition(const std::optional<Prefab::PrefabComposition> &composition,
                                              const ComponentReferences &references) {
            if (!composition || references.owners.empty())
                return Result<void>::Success();
            for (const auto &nested : composition->nestedPlacements) {
                auto nestedReferences = references;
                nestedReferences.placement = nested.placementLocalId.value;
                if (auto found = InspectComponentOverrides(nested.valueOverrides, nestedReferences); found.HasError())
                    return found;
            }
            return InspectComponentOverrides(composition->variantOverrides, references);
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

        /** @brief Reads direct Scene objects and the saved overrides owned by their containing document. */
        Result<void> InspectSceneObjects(const SceneSource::SceneSourceDocument &scene, SavedReferenceScan &scan,
                                         const SavedReferenceOwners &owners, std::string_view path) {
            ComponentReferences components{scan.components, owners.components, path};
            for (const auto &object : scene.objects) {
                if (auto found = Collect(scan.behaviors, owners.behaviors, path, object.id.value, object.components.behaviors);
                    found.HasError())
                    return found;
                for (const auto &component : object.components.gameplayComponents)
                    if (auto found = components.Collect(component.typeId, object.id.value); found.HasError())
                        return found;
            }
            for (const auto &instance : scene.prefabInstances) {
                components.placement = instance.instanceId.Value();
                if (auto found = InspectComponentOverrides(instance.valueOverrides, components); found.HasError())
                    return found;
            }
            return Result<void>::Success();
        }

        /** @brief Confirms a composition edge can be inspected elsewhere in this same registry scan. */
        struct CompositionSourceReview final {
            std::span<const Assets::AssetRecord> catalog;
            std::vector<Diagnostic> &diagnostics;
            std::string_view path;

            /** @brief Reports the unresolved asset identity, expected kind and authored source. */
            Result<void> Check(const Assets::AssetId &asset, std::string_view type) const {
                const auto found = std::ranges::lower_bound(catalog, asset, {}, &Assets::AssetRecord::id);
                const bool missing = found == catalog.end() || found->id != asset;
                if (!missing && found->type.Value() == type)
                    return Result<void>::Success();
                if (diagnostics.size() >= MaximumBehaviorReferences)
                    return Result<void>::Failure(ReferenceError("packages.host.composition_reference_limit", path));
                const auto *code = missing ? "packages.host.composition_reference_missing"
                                           : "packages.host.composition_reference_type_mismatch";
                diagnostics.emplace_back(DiagnosticCode{code}, DiagnosticSeverity::Error, std::string{type},
                                         SourceLocation{std::string{path}}, asset.ToString());
                return Result<void>::Success();
            }
        };

        /** @brief Verifies child scene and prefab identities without composing or executing the scene. */
        Result<void> InspectSceneSourceLinks(const SceneSource::SceneSourceDocument &scene, const CompositionSourceReview &review) {
            if (review.catalog.empty())
                return Result<void>::Success();
            for (const auto &child : scene.childScenes)
                if (auto checked = review.Check(child.asset, SceneAssetType); checked.HasError())
                    return checked;
            for (const auto &placement : scene.prefabInstances)
                if (auto checked = review.Check(placement.sourcePrefab.Asset(), PrefabAssetType); checked.HasError())
                    return checked;
            return Result<void>::Success();
        }

        /** @brief Verifies all nested and variant prefab sources in the same asset inventory. */
        Result<void> InspectPrefabSourceLinks(const std::optional<Prefab::PrefabComposition> &composition,
                                             const CompositionSourceReview &review) {
            if (review.catalog.empty() || !composition)
                return Result<void>::Success();
            for (const auto &placement : composition->nestedPlacements)
                if (auto checked = review.Check(placement.sourcePrefab.Asset(), PrefabAssetType); checked.HasError())
                    return checked;
            return composition->variantParent ? review.Check(composition->variantParent->Asset(), PrefabAssetType) : Result<void>::Success();
        }

        /** @brief Reads direct authored attachments; each referenced child or prefab has its own registry entry. */
        Result<void> InspectScene(SavedReferenceScan &scan, const SavedReferenceReview &review, std::string_view path, std::string_view text) {
            const auto scene = SceneSource::DecodeSceneSourceWithChildReferences(text);
            if (scene.HasError())
                return scan.failures.Record(path,
                    WithCause(ReferenceError("packages.host.scene_reference_unreadable", path), scene.ErrorValue()));
            if (!scene.Value().unsupportedComponentOwners.empty())
                return scan.failures.Record(path, ReferenceError("packages.host.scene_reference_unsupported", path));
            const auto &owners = review.owners;
            if (auto found = InspectSceneObjects(scene.Value(), scan, owners, path); found.HasError())
                return found;
            if (auto links = InspectSceneSourceLinks(scene.Value(), {review.compositionCatalog, scan.composition, path}); links.HasError())
                return links;
            return !owners.senses.empty() && RequiresSenseReview(scene.Value()) ? CollectSenseReview(scan, owners, path)
                                                                                 : Result<void>::Success();
        }

        /** @brief Uses prefab-local component instances and visits nested and variant override records. */
        Result<void> InspectPrefabObjects(const Prefab::PrefabDocumentData &prefab, SavedReferenceScan &scan,
                                          const SavedReferenceOwners &owners, std::string_view path) {
            const ComponentReferences components{scan.components, owners.components, path};
            for (const auto &object : prefab.objects) {
                if (auto found = Collect(scan.behaviors, owners.behaviors, path, object.localId.value, object.behaviors); found.HasError())
                    return found;
                for (const auto &payload : object.components)
                    if (auto found = components.Collect(payload.component.typeId, object.localId.value, payload.instance.Value());
                        found.HasError())
                        return found;
            }
            return InspectPrefabComposition(prefab.composition, components);
        }

        /** @brief Reads direct prefab attachments, retaining both asset and prefab-local object identity. */
        Result<void> InspectPrefab(SavedReferenceScan &scan, const SavedReferenceReview &review, const Assets::AssetRecord &record,
                                   std::string_view text, const Prefab::PrefabLimitProfile &limits) {
            const auto path = record.sourcePath.String();
            const auto prefab = Prefab::PrefabDocument::Parse(text, limits);
            if (prefab.HasError())
                return scan.failures.Record(path,
                    WithCause(ReferenceError("packages.host.prefab_reference_unreadable", path), prefab.ErrorValue()));
            if (prefab.Value().Data().assetId != record.id)
                return scan.failures.Record(path, ReferenceError("packages.host.prefab_reference_identity_mismatch", path));
            const auto &data = prefab.Value().Data();
            const auto &owners = review.owners;
            if (auto found = InspectPrefabObjects(data, scan, owners, path); found.HasError())
                return found;
            if (auto links = InspectPrefabSourceLinks(data.composition, {review.compositionCatalog, scan.composition, path}); links.HasError())
                return links;
            return !owners.senses.empty() && RequiresSenseReview(data) ? CollectSenseReview(scan, owners, path)
                                                                       : Result<void>::Success();
        }

        /** @brief Reads a canonical source or records its path while the registry scan can still continue. */
        Result<void> InspectAsset(const Platform::IProjectReadFiles &files, const Platform::ProjectReadContext &context,
                                  const Assets::AssetRecord &record, const Prefab::PrefabLimitProfile &limits,
                                  const SavedReferenceReview &review, SavedReferenceScan &scan) {
            const auto path = record.sourcePath.String();
            const auto source = files.SourceText(path, context, {});
            if (source.HasError())
                return scan.failures.Record(path, WithCause(ReferenceError("packages.host.behavior_reference_unreadable", path),
                                                             source.ErrorValue()));
            const auto before = scan.behaviors.size();
            auto inspected = record.type.Value() == SceneAssetType ? InspectScene(scan, review, path, *source.Value().text)
                                                                   : InspectPrefab(scan, review, record, *source.Value().text, limits);
            if (inspected.HasValue() && scan.behaviors.size() != before)
                scan.behaviorSourceRevisions.emplace(path, source.Value().revision);
            return inspected;
        }

        /** @brief Shares the original deadline across bounded descendant reads and the resulting observations. */
        Result<SavedReferenceScan> InspectAssets(const Platform::IProjectReadFiles &files, const Platform::ProjectReadContext &context,
                                                std::span<const Assets::AssetRecord> records, const SavedReferenceReview &review) {
            using Inspection = Result<SavedReferenceScan>;
            const auto limits = Prefab::PrefabLimitProfile::Create({});
            if (limits.HasError())
                return Inspection::Failure(limits.ErrorValue());
            SavedReferenceScan scan;
            for (const auto &record : records) {
                const auto type = record.type.Value();
                if (type != SceneAssetType && type != PrefabAssetType)
                    continue;
                if (auto current = Platform::CheckProjectReadContext(context); current.HasError())
                    return Inspection::Failure(current.ErrorValue());
                const auto inspected = InspectAsset(files, context, record, limits.Value(), review, scan);
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

        /** @brief Indexes services and component schemas from roles in the exact departing set. */
        struct NativeReferenceOwners final {
            ServiceOwners services;
            ComponentOwners components;

            /** @brief Adds inert provider identities from an already matched departing registration. */
            void Add(const Registration &registration) {
                for (const auto &service : registration.services)
                    services[service.id].insert(registration.package);
                for (const auto &component : registration.components)
                    components[component].insert(registration.package);
            }

            /** @brief Reports whether the withdrawal supplies a native identity that retained roles can use. */
            [[nodiscard]] bool Empty() const {
                return services.empty() && components.empty();
            }
        };

        /** @brief Checks explicit dependencies on a departing service or component type. */
        Result<void> InspectNativeConsumer(std::vector<Diagnostic> &references, const NativeReferenceOwners &owners,
                                           const Registration &consumer) {
            for (const auto &service : consumer.services)
                if (auto result = CollectServiceReferences(references, owners.services, consumer, service.id.Value(), service.dependencies);
                    result.HasError())
                    return result;
            for (const auto &system : consumer.systems)
                if (auto result = CollectServiceReferences(references, owners.services, consumer, system.id.Value(), system.requiredServices);
                    result.HasError())
                    return result;
            for (const auto &use : consumer.componentUses) {
                const auto matching = owners.components.find(use.component);
                if (matching == owners.components.end())
                    continue;
                if (matching->second.size() > MaximumServiceReferences - references.size())
                    return Result<void>::Failure(ReferenceError("packages.host.component_reference_limit"));
                const auto &registration = std::visit([](const auto &identity) -> const std::string & { return identity.Value(); },
                                                      use.consumer);
                for (const auto &provider : matching->second)
                    references.emplace_back(DiagnosticCode{"packages.host.component_access_in_use"}, DiagnosticSeverity::Error,
                                            provider.Value() + " / " + use.component.Value(), SourceLocation{consumer.descriptorPath},
                                            consumer.package.Value() + "/" + registration);
            }
            return Result<void>::Success();
        }

        /** @brief Excludes cached observations from another artifact or without an admitted generation. */
        bool MatchesCurrentLock(const LockedPackages &locked, const Registration &registration) {
            const auto entry = locked.find(registration.package);
            return entry != locked.end() && MatchesLockedArtifact(registration, *entry->second);
        }

        /** @brief Departing services and components require coverage of retained native consumers. */
        bool HasDepartingNativeDependency(std::span<const Registration> registrations,
                                          const Packages::ValidatedPackageLockfileV1 &lock,
                                          const Packages::PackageRemovalGraphPreview &removal) {
            return std::ranges::any_of(registrations, [&](const Registration &entry) {
                if ((entry.services.empty() && entry.components.empty()) || !IsDeparting(removal, entry.package))
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
                                                                                       const Packages::PackageRemovalLease *reservation) {
            using Roles = Result<std::vector<Packages::PackageRuntimeRoleDeclaration>>;
            if (!reservation)
                return Packages::InspectPackageRuntimeRoles(install, package.lock.package);
            if (!OfflineRemovalContributionBlocker(package.lock) && IsOfflineAssetArchive(package))
                return Roles::Success({});
            auto roles = Packages::InspectLivePackageRemovalRoles(package, *reservation);
            if (roles.HasError()) {
                auto cause = std::move(roles).ErrorValue();
                auto error = ReferenceError("packages.host.removal_requires_lifecycle_owner", package.lock.package.Value());
                error.diagnostics = std::move(cause.diagnostics);
                return Roles::Failure(WithCause(std::move(error), std::move(cause)));
            }
            return roles;
        }

        /** @brief Identifies an exact locked role with missing or insufficient registration evidence. */
        void ObserveRuntimeRole(const PackageRemovalSources &sources, const Packages::LockedPackage &locked,
                                const Packages::PackageRuntimeRoleDeclaration &role, std::vector<Diagnostic> &blockers) {
            const auto observed = std::ranges::find_if(sources.registrations, [&](const auto &entry) {
                return MatchesLockedArtifact(entry, locked) && entry.moduleId == role.moduleId &&
                       entry.descriptorPath == role.descriptor.Value();
            });
            const auto unobserved = observed == sources.registrations.end();
            if (!unobserved && (!sources.requireGameplayOwner || !observed->hasAdditionalReferenceClasses))
                return;
            const auto *code = unobserved ? "packages.host.runtime_registration_unobserved"
                                          : "packages.host.removal_reference_coverage_unavailable";
            blockers.emplace_back(DiagnosticCode{code}, DiagnosticSeverity::Error, locked.package.Value(),
                                  SourceLocation{role.descriptor.Value()}, role.moduleId);
        }

        /** @brief Reads declared roles of one exact locked artifact and collects role blockers. */
        Result<void> InspectRuntimeCoverage(const PackageRemovalSources &sources, const Packages::PackageRestoreGraph &graph,
                                            const Packages::VerifiedPackageInstallRecord &install, const Packages::LockedPackage &locked,
                                            const bool departing, std::vector<Diagnostic> &blockers) {
            const auto *package = FindExactArchive(graph, locked);
            if (!package)
                return Result<void>::Failure(ReferenceError("packages.host.removal_archive_evidence_unavailable", locked.package.Value()));
            const auto declared = DeclaredRemovalRoles(install, *package,
                                                       departing && sources.requireGameplayOwner ? sources.live.get() : nullptr);
            if (declared.HasError())
                return Result<void>::Failure(declared.ErrorValue());
            if (!declared.Value().empty() && sources.registrationFailure)
                return Result<void>::Failure(WithCause(
                    ReferenceError("packages.host.runtime_registration_unavailable", locked.package.Value()), *sources.registrationFailure));
            for (const auto &role : declared.Value())
                ObserveRuntimeRole(sources, locked, role, blockers);
            return Result<void>::Success();
        }

        /** @brief Preserves the first typed failure while making each affected package visible in the review. */
        void RecordRuntimeCoverageFailure(const Packages::LockedPackage &locked, Error failure, std::optional<Error> &firstFailure,
                                          std::vector<Diagnostic> &blockers) {
            if (failure.diagnostics.empty())
                blockers.emplace_back(DiagnosticCode{failure.code.Value()}, DiagnosticSeverity::Error, failure.message,
                                      SourceLocation{"packages.lock"}, locked.package.Value());
            else
                std::ranges::move(failure.diagnostics, std::back_inserter(blockers));
            if (!firstFailure) {
                failure.diagnostics.clear();
                firstFailure.emplace(std::move(failure));
            }
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
            std::vector<Diagnostic> blockers;
            std::optional<Error> firstFailure;
            const bool inspectRetained = HasDepartingNativeDependency(sources.registrations, lock, removal);
            for (const auto &entry : lock.Packages()) {
                const bool departing = IsDeparting(removal, entry.package);
                if (!inspectRetained && !departing)
                    continue;
                if (auto authority = Platform::CheckProjectReadContext(sources.context); authority.HasError())
                    return authority;
                if (auto checked = InspectRuntimeCoverage(sources, *graph, *install, entry, departing, blockers); checked.HasError())
                    RecordRuntimeCoverageFailure(entry, std::move(checked).ErrorValue(), firstFailure, blockers);
                if (blockers.size() > MaximumCoverageDiagnostics)
                    return Result<void>::Failure(ReferenceError("packages.host.removal_reference_coverage_limit"));
            }
            if (!blockers.empty()) {
                auto error = firstFailure ? std::move(*firstFailure) : ReferenceError(blockers.front().code.Value());
                error.diagnostics = std::move(blockers);
                return Result<void>::Failure(std::move(error));
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PackageSourceReviewFailures::MarkInventoryIncomplete */
    void PackageSourceReviewFailures::MarkInventoryIncomplete() {
        if (!first_)
            first_.emplace(ReferenceError("packages.host.asset_inventory_incomplete"));
    }

    /** @copydoc PackageSourceReviewFailures::Record */
    Result<void> PackageSourceReviewFailures::Record(const std::string_view path, Error failure) {
        constexpr std::size_t maximumFailures = 16'384;
        auto reason = failure.code.Value();
        if (!first_)
            first_.emplace(std::move(failure));
        if (first_->diagnostics.size() >= maximumFailures)
            return Result<void>::Failure(ReferenceError("packages.host.saved_reference_source_limit", path));
        first_->diagnostics.emplace_back(DiagnosticCode{"packages.host.saved_reference_source_unreadable"}, DiagnosticSeverity::Error,
                                         std::move(reason), SourceLocation{std::string{path}}, "");
        return Result<void>::Success();
    }

    /** @copydoc PackageSourceReviewFailures::Finish */
    Result<std::vector<Diagnostic>> PackageSourceReviewFailures::Finish(std::vector<Diagnostic> references) && {
        using Review = Result<std::vector<Diagnostic>>;
        if (!first_)
            return Review::Success(std::move(references));
        std::ranges::move(references, std::back_inserter(first_->diagnostics));
        return Review::Failure(std::move(*first_));
    }

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

    /** @copydoc InspectPackageNativeReferences */
    Result<std::vector<Diagnostic>> InspectPackageNativeReferences(const PackageRemovalSources &sources,
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

        NativeReferenceOwners owners;
        std::vector<const Registration *> retained;
        for (const auto &registration : sources.registrations) {
            if (!MatchesCurrentLock(locked, registration))
                continue;
            if (departing.contains(registration.package))
                owners.Add(registration);
            else
                retained.push_back(&registration);
        }
        if (owners.Empty())
            return Inspection::Success({});
        std::vector<Diagnostic> references;
        for (const auto *consumer : retained)
            if (auto result = InspectNativeConsumer(references, owners, *consumer); result.HasError())
                return Inspection::Failure(result.ErrorValue());
        return Inspection::Success(std::move(references));
    }

    /** @brief Scans saved sources against either observed registrations or inert preview claims. */
    static Result<SavedReferenceScan> ScanSavedReferences(const Assets::IAssetRegistrySource &assets,
                                                          const Platform::IProjectReadFiles &files, Platform::ProjectReadContext context,
                                                          const SavedReferenceOwners &owners,
                                                          const Assets::AssetRegistryCandidate *reviewed = nullptr) {
        using Inspection = Result<SavedReferenceScan>;
        if (owners.Empty())
            return Inspection::Success({});
        context.deadline =
            std::min(context.deadline, std::chrono::steady_clock::now() + Platform::ProjectSourceReadLimits{}.maximumDuration);
        if (!reviewed) {
            const auto candidate = Assets::PrepareAssetRegistryCandidate(assets);
            if (candidate.HasError())
                return Inspection::Failure(candidate.ErrorValue());
            if (candidate.Value().status != Assets::AssetRegistryBuildStatus::Complete)
                return Inspection::Failure(ReferenceError("packages.host.asset_inventory_incomplete"));
            return InspectAssets(files, context, candidate.Value().records, {owners, {}});
        }
        const bool complete = reviewed->status == Assets::AssetRegistryBuildStatus::Complete;
        const std::span<const Assets::AssetRecord> records = reviewed->records;
        const auto catalog = complete ? records : std::span<const Assets::AssetRecord>{};
        auto scan = InspectAssets(files, context, records, {owners, catalog});
        if (scan.HasError() || complete)
            return scan;
        auto partial = std::move(scan).Value();
        partial.failures.MarkInventoryIncomplete();
        return Inspection::Success(std::move(partial));
    }

    /** @copydoc InspectPackageScenePrefabReferences */
    Result<std::vector<Diagnostic>> InspectPackageScenePrefabReferences(
        const PackageRemovalSources &sources,
        const Packages::ValidatedPackageLockfileV1 &lock, const Packages::PackageRemovalGraphPreview &removal,
        const Assets::AssetRegistryCandidate &inventory) {
        using Inspection = Result<std::vector<Diagnostic>>;
        const auto owners = IndexOwners(sources.registrations, lock, removal);
        if (owners.HasError())
            return Inspection::Failure(owners.ErrorValue());
        auto inspected = ScanSavedReferences(sources.assets, sources.files, sources.context, owners.Value(), &inventory);
        if (inspected.HasError())
            return Inspection::Failure(inspected.ErrorValue());
        auto scan = std::move(inspected).Value();
        std::vector<Diagnostic> diagnostics;
        diagnostics.reserve(scan.behaviors.size() + scan.senses.size() + scan.components.size() + scan.composition.size());
        for (const auto &reference : scan.behaviors)
            diagnostics.emplace_back(DiagnosticCode{"packages.host.behavior_in_use"}, DiagnosticSeverity::Error,
                                     reference.package.Value() + ": " + reference.type.Value(), SourceLocation{reference.assetPath},
                                     std::to_string(reference.object) + "/" + std::to_string(reference.instance.value));
        std::ranges::move(scan.senses, std::back_inserter(diagnostics));
        std::ranges::move(scan.components, std::back_inserter(diagnostics));
        std::ranges::move(scan.composition, std::back_inserter(diagnostics));
        return std::move(scan.failures).Finish(std::move(diagnostics));
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
        auto matches = ScanSavedReferences(assets, files, std::move(context), assertedOwners);
        if (matches.HasError())
            return Inspection::Failure(matches.ErrorValue());
        auto scan = std::move(matches).Value();
        std::vector<Diagnostic> diagnostics;
        for (const auto &match : scan.behaviors | std::views::take(diagnosticLimit)) {
            const auto *target = targets.at({match.package, match.type});
            const auto &revision = scan.behaviorSourceRevisions.at(match.assetPath);
            auto message = match.package.Value() + " [source sha256:" + revision + "]: " + match.type.Value() + " -> " + target->Value() +
                           " (object " + std::to_string(match.object) + ", attachment " + std::to_string(match.instance.value) + ')';
            diagnostics.emplace_back(DiagnosticCode{"packages.host.update_possible_behavior_rename"}, DiagnosticSeverity::Note,
                                     std::move(message), SourceLocation{"packages.host"}, match.assetPath);
        }
        return std::move(scan.failures).Finish(std::move(diagnostics));
    }
}  // namespace Horo::Application::Internal
