#include "PackageBehaviorReferences.h"

#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Prefab/PrefabDocument.h"
#include "Horo/Scene/SceneSource.h"

#include <algorithm>
#include <map>
#include <set>

namespace Horo::Application::Internal {
    namespace {
        using Owners = std::map<Gameplay::BehaviorTypeId, std::set<Packages::HoroPackageId>>;
        using References = std::vector<PackageBehaviorReference>;
        constexpr std::size_t MaximumBehaviorReferences = 16'384;

        Error ReferenceError(std::string code, std::string_view path = {}) {
            return {ErrorCode{std::move(code)}, ErrorDomainId{"packages.host"}, ErrorSeverity::Error, std::string{path}};
        }

        /** @brief Accepts only registrations attributed to the exact locked artifact being withdrawn. */
        Result<Owners> IndexOwners(std::span<const Packages::GameplayPackageModuleHost::RegistrationInventory> registrations,
                                   const Packages::ValidatedPackageLockfileV1 &lock, const Packages::PackageRemovalGraphPreview &removal) {
            Owners owners;
            for (const auto &registration : registrations) {
                if (std::ranges::find(removal.removed, registration.package, &Packages::LockedPackageReference::package) ==
                    removal.removed.end())
                    continue;
                const auto entry = std::ranges::find(lock.Packages(), registration.package, &Packages::LockedPackage::package);
                if (entry == lock.Packages().end() || entry->version != registration.version ||
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
    }  // namespace

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
