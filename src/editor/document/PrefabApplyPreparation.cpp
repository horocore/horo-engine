#include "editor/document/PrefabApplyPreparation.h"

#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <new>

namespace Horo::Editor {
    namespace {
        using namespace Prefab;

        /** @brief Stable local object target of an existing transform or property intent. */
        const PrefabObjectAddress &ValueObject(const PrefabValueOverrideRecord &record) {
            if (const auto *property = std::get_if<PrefabPropertyAddress>(&record.target))
                return property->Object();
            return std::get<PrefabObjectAddress>(record.target);
        }

        /** @brief Stable local object target of topology intent. */
        const PrefabObjectAddress &ComponentObject(const PrefabComponentOverrideRecord &record) {
            if (const auto *add = std::get_if<PrefabAddComponentOverride>(&record))
                return add->object;
            return std::get<PrefabRemoveComponentOverride>(record).target.object;
        }

        /** @brief Rejects an incomplete or inherited selection before invoking the ordinary override algebra. */
        Result<PrefabValueOverrideSet> SelectedOverrides(const PrefabValueOverrideSet &set, const ScenePrefabRevertTarget &scope) {
            using Return = Result<PrefabValueOverrideSet>;
            const auto selectedOriginal = [&scope](const auto &entry) {
                return MatchesPrefabOverrideScope(scope, entry.original);
            };
            if (std::ranges::any_of(set.conflicts, selectedOriginal) || std::ranges::any_of(set.componentConflicts, selectedOriginal))
                return Return::Failure(MakeError(PrefabErrors::OverrideConflict));
            if (std::ranges::any_of(set.orphans, selectedOriginal) || std::ranges::any_of(set.componentOrphans, selectedOriginal))
                return Return::Failure(MakeError(PrefabErrors::OverrideOrphan));
            PrefabValueOverrideSet selected{set.sourcePrefab, set.authoredAgainst, set.schemaRevision};
            const auto select = [&scope](const auto &record) {
                return MatchesPrefabOverrideScope(scope, record);
            };
            std::ranges::copy_if(set.records, std::back_inserter(selected.records), select);
            std::ranges::copy_if(set.componentRecords, std::back_inserter(selected.componentRecords), select);
            const auto nested = [](const PrefabObjectAddress &address) {
                return !address.NestedInstanceScope().empty();
            };
            if (selected.IsEmpty() || std::ranges::any_of(selected.records, nested, ValueObject) ||
                std::ranges::any_of(selected.componentRecords, nested, ComponentObject))
                return Return::Failure(MakeError(PrefabErrors::OverrideInvalid));
            return Return::Success(std::move(selected));
        }

        /** @brief Replaces only the concrete owner's authored object records from the fully resolved selection. */
        Result<PrefabDocument> PromoteConcreteSource(const PrefabDocument &source, const EffectivePrefabCandidate &applied,
                                                     const PrefabLimitProfile &limits) {
            auto data = source.Data();
            for (const auto &object : applied.Objects()) {
                if (!object.key.object.NestedInstanceScope().empty())
                    continue;
                const auto found = std::ranges::find(data.objects, object.key.object.SourceObject(), &PrefabObjectNode::localId);
                if (object.sourcePrefab != data.assetId || found == data.objects.end())
                    return Result<PrefabDocument>::Failure(MakeError(PrefabErrors::AddressInvalid));
                *found = object.object;
            }
            return PrefabDocument::Create(std::move(data), limits);
        }

        /** @brief Applies ordinary rebase to a complete detached instance envelope without guessing any dropped identity. */
        Result<void> RebasePlacement(ScenePrefabInstance &placement, const PrefabSourceResolverSnapshot &sources,
                                     const PrefabValueSchemaSnapshot *schemas, const PrefabLimitProfile &limits,
                                     const CancellationToken &cancellation) {
            if (!placement.valueOverrides)
                return Result<void>::Success();
            auto source = sources.Resolve(placement.sourcePrefab.Asset(), placement.instanceId, limits, cancellation);
            if (source.HasError())
                return Result<void>::Failure(std::move(source).ErrorValue());
            auto resolved = ResolvePrefabValueOverrides(source.Value(), *placement.valueOverrides, schemas, limits, cancellation);
            if (resolved.HasError())
                return Result<void>::Failure(std::move(resolved).ErrorValue());
            auto rebased = std::move(resolved).Value().rebased;
            if (rebased.IsEmpty())
                placement.valueOverrides.reset();
            else
                placement.valueOverrides = std::move(rebased);
            return Result<void>::Success();
        }

        /** @brief Rebuilds the exact captured resolver, then rebases the changed source's active Scene placements. */
        Result<void> RebasePlacements(PreparedPrefabApply &prepared, const RevertScenePrefabOverridesCommand &command,
                                      const Assets::AssetRegistrySnapshot &registry, std::vector<PrefabDependencySource> sources,
                                      std::shared_ptr<const PrefabValueSchemaSnapshot> schemas, const PrefabLimitProfile &limits,
                                      const CancellationToken &cancellation) {
            auto next = BuildPrefabSourceResolverSnapshot(registry, std::move(sources), limits, {}, schemas);
            if (next.HasError())
                return Result<void>::Failure(std::move(next).ErrorValue());
            for (auto &placement : prepared.placements) {
                if (placement.sourcePrefab.Asset() != prepared.source.Data().assetId || !placement.valueOverrides)
                    continue;
                if (placement.instanceId == command.instance)
                    ErasePrefabOverrideScope(*placement.valueOverrides, command.target);
                if (auto rebased = RebasePlacement(placement, next.Value(), schemas.get(), limits, cancellation); rebased.HasError())
                    return rebased;
            }
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PreparePrefabApply */
    Result<PreparedPrefabApply> PreparePrefabApply(const SceneDocumentSnapshot &scene, const RevertScenePrefabOverridesCommand &command,
                                                   const Assets::AssetRegistrySnapshot &registry,
                                                   const Prefab::PrefabSourceResolverSnapshot &sources,
                                                   std::shared_ptr<const Prefab::PrefabValueSchemaSnapshot> schemas,
                                                   const Prefab::PrefabLimitProfile &limits, const CancellationToken &cancellation) {
        using namespace Prefab;
        using Return = Result<PreparedPrefabApply>;
        try {
            if (scene.revision != command.expectedRevision || sources.RegistryRevision() != registry.Revision() ||
                command.expectedSchema != (schemas ? schemas->Revision() : Sha256Digest{}))
                return Return::Failure(MakeError(PrefabErrors::OverrideSourceStale));
            if (command.target.valueless_by_exception())
                return Return::Failure(MakeError(PrefabErrors::AddressInvalid));
            const auto instance = std::ranges::find(scene.prefabInstances, command.instance, &ScenePrefabInstance::instanceId);
            if (instance == scene.prefabInstances.end() || !instance->valueOverrides)
                return Return::Failure(MakeError(PrefabErrors::OverrideInvalid));
            const auto asset = instance->sourcePrefab.Asset();
            auto original = sources.Resolve(asset, command.instance, limits, cancellation);
            if (original.HasError())
                return Return::Failure(std::move(original).ErrorValue());
            if (original.Value().Revision() != command.expectedSource)
                return Return::Failure(MakeError(PrefabErrors::OverrideSourceStale));
            auto chosen = SelectedOverrides(*instance->valueOverrides, command.target);
            if (chosen.HasError())
                return Return::Failure(std::move(chosen).ErrorValue());
            const auto source = std::ranges::find_if(sources.Sources(), [asset](const auto &candidate) {
                return candidate.document.Data().assetId == asset;
            });
            if (source == sources.Sources().end() ||
                (source->document.Data().composition && source->document.Data().composition->variantParent))
                return Return::Failure(
                    MakeError(PrefabErrors::OverrideInvalid, "Apply requires the selected values to belong to the concrete source."));
            auto selected = ResolvePrefabValueOverrides(original.Value(), chosen.Value(), schemas.get(), limits, cancellation);
            if (selected.HasError())
                return Return::Failure(std::move(selected).ErrorValue());
            if (!selected.Value().effective)
                return Return::Failure(MakeError(PrefabErrors::OverrideConflict));
            auto updated = PromoteConcreteSource(source->document, *selected.Value().effective, limits);
            if (updated.HasError())
                return Return::Failure(std::move(updated).ErrorValue());
            auto bytes = updated.Value().SerializeCanonical();
            if (bytes.HasError())
                return Return::Failure(std::move(bytes).ErrorValue());
            const auto revision =
                PrefabSourceRevision{updated.Value().Data().projectVersion, ComputeSha256(std::as_bytes(std::span{bytes.Value()}))};
            auto updatedSources = std::vector(sources.Sources().begin(), sources.Sources().end());
            const auto sourceIndex = static_cast<std::size_t>(source - sources.Sources().begin());
            updatedSources[sourceIndex] = {updated.Value(), revision};
            PreparedPrefabApply prepared{std::move(updated).Value(), scene.prefabInstances};
            if (auto result =
                    RebasePlacements(prepared, command, registry, std::move(updatedSources), std::move(schemas), limits, cancellation);
                result.HasError())
                return Return::Failure(std::move(result).ErrorValue());
            return Return::Success(std::move(prepared));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }
}  // namespace Horo::Editor
