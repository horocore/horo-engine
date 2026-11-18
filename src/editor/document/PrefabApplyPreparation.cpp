#include "editor/document/PrefabApplyPreparation.h"

#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <map>
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

        /** @brief Rejects an incomplete selection before invoking the ordinary override algebra. */
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
            if (selected.IsEmpty())
                return Return::Failure(MakeError(PrefabErrors::OverrideInvalid));
            return Return::Success(std::move(selected));
        }

        /** @brief Replaces only the concrete owner's authored object records from the fully resolved selection. */
        Result<void> PromoteConcreteObjects(PrefabDocumentData &data, const EffectivePrefabCandidate &applied) {
            for (const auto &object : applied.Objects()) {
                if (!object.key.object.NestedInstanceScope().empty())
                    continue;
                const auto found = std::ranges::find(data.objects, object.key.object.SourceObject(), &PrefabObjectNode::localId);
                if (object.sourcePrefab != data.assetId || found == data.objects.end())
                    return Result<void>::Failure(MakeError(PrefabErrors::AddressInvalid));
                *found = object.object;
            }
            return Result<void>::Success();
        }

        /** @brief Immutable source authority and one shared work budget across all affected owning layers. */
        struct LayerPreparation final {
            const PrefabSourceResolverSnapshot &sources;
            const PrefabValueSchemaSnapshot *schemas;
            const PrefabLimitProfile &limits;
            const CancellationToken &cancellation;
            PrefabInstanceId instance;
            PrefabExpansionBudget work;
        };

        /** @brief Strips exactly the selected owning occurrence while preserving the typed component/property identity. */
        Result<PrefabValueOverrideTarget> RelativeTarget(const PrefabValueOverrideRecord &record) {
            using Return = Result<PrefabValueOverrideTarget>;
            const auto &object = ValueObject(record);
            auto relative = PrefabObjectAddress::Create(object.NestedInstanceScope().subspan(1), object.SourceObject());
            if (relative.HasError())
                return Return::Failure(std::move(relative).ErrorValue());
            const auto *property = std::get_if<PrefabPropertyAddress>(&record.target);
            if (!property)
                return Return::Success(std::move(relative).Value());
            auto target = PrefabPropertyAddress::Create(std::move(relative).Value(), property->ComponentType(),
                                                        property->ComponentInstance(), property->Property());
            return target.HasError() ? Return::Failure(std::move(target).ErrorValue()) : Return::Success(std::move(target).Value());
        }

        /** @brief Stages exact authored values relative to the actual immediate source of this layer. */
        Result<void> PromoteValueLayer(LayerPreparation &context, const Assets::AssetId immediate, PrefabSourceRevision &authored,
                                       std::optional<PrefabValueOverrideSet> &owned, std::span<const PrefabValueEdit> assignments) {
            if (assignments.empty())
                return Result<void>::Success();
            auto source = context.sources.Resolve(immediate, context.instance, context.limits, context.cancellation);
            if (source.HasError())
                return Result<void>::Failure(std::move(source).ErrorValue());
            const auto objects = source.Value().Objects();
            for (const auto &edit : assignments) {
                if (auto budget = context.work.Consume(objects.size()); budget.HasError())
                    return budget;
                if (auto budget = context.work.Consume(owned ? owned->records.size() + owned->componentRecords.size() : 0);
                    budget.HasError())
                    return budget;
                auto prepared = PreparePrefabValueEdit(source.Value(), owned, edit, context.schemas, context.limits, context.cancellation);
                if (prepared.HasError())
                    return Result<void>::Failure(std::move(prepared).ErrorValue());
                if (!prepared.Value().effective)
                    return Result<void>::Failure(MakeError(PrefabErrors::OverrideConflict));
                auto next = std::move(prepared).Value().rebased;
                if (next.IsEmpty())
                    owned.reset();
                else
                    owned = std::move(next);
            }
            authored = source.Value().Revision().rootSource;
            return Result<void>::Success();
        }

        /** @brief Promotes simple values in the variant layer or the selected concrete occurrence, without editing any dependency. */
        Result<PrefabDocument> PromoteSource(const PrefabDocument &source, const EffectivePrefabCandidate &applied,
                                             const PrefabValueOverrideSet &selected, LayerPreparation &context) {
            auto data = source.Data();
            auto *composition = data.composition ? &*data.composition : nullptr;
            const bool variant = composition && composition->variantParent.has_value();
            if (std::ranges::any_of(selected.componentRecords, [variant](const auto &record) {
                return variant || !ComponentObject(record).NestedInstanceScope().empty();
            }))
                return Result<PrefabDocument>::Failure(
                    MakeError(PrefabErrors::OverrideInvalid, "Layered component topology requires an explicit owning-layer edit."));
            if (variant) {
                std::vector<PrefabValueEdit> values;
                values.reserve(selected.records.size());
                for (const auto &record : selected.records)
                    values.push_back({PrefabValueEditKind::Assign, record.target, record.value});
                auto result = PromoteValueLayer(context, composition->variantParent->Asset(), *composition->variantAuthoredAgainst,
                                                composition->variantOverrides, values);
                if (result.HasError())
                    return Result<PrefabDocument>::Failure(std::move(result).ErrorValue());
                return PrefabDocument::Create(std::move(data), context.limits);
            }

            if (auto direct = PromoteConcreteObjects(data, applied); direct.HasError())
                return Result<PrefabDocument>::Failure(std::move(direct).ErrorValue());
            std::map<LocalObjectId, std::vector<PrefabValueEdit>> nested;
            for (const auto &record : selected.records) {
                const auto scope = ValueObject(record).NestedInstanceScope();
                if (scope.empty())
                    continue;
                auto target = RelativeTarget(record);
                if (target.HasError())
                    return Result<PrefabDocument>::Failure(std::move(target).ErrorValue());
                nested[scope.front()].push_back({PrefabValueEditKind::Assign, std::move(target).Value(), record.value});
            }
            for (const auto &[slot, values] : nested) {
                if (!composition)
                    return Result<PrefabDocument>::Failure(MakeError(PrefabErrors::AddressInvalid));
                const auto placement = std::ranges::find(composition->nestedPlacements, slot, &NestedPrefabPlacement::placementLocalId);
                if (placement == composition->nestedPlacements.end())
                    return Result<PrefabDocument>::Failure(MakeError(PrefabErrors::AddressInvalid));
                auto result = PromoteValueLayer(context, placement->sourcePrefab.Asset(), placement->authoredAgainst,
                                                placement->valueOverrides, values);
                if (result.HasError())
                    return Result<PrefabDocument>::Failure(std::move(result).ErrorValue());
            }
            return PrefabDocument::Create(std::move(data), context.limits);
        }

        /** @brief Proves that source promotion reproduces the selected ordinary and placement-adjusted values. */
        bool SameSourceEffect(const EffectivePrefabCandidate &expected, const EffectivePrefabCandidate &rebuilt) {
            return std::ranges::equal(expected.Objects(), rebuilt.Objects(), [](const auto &left, const auto &right) {
                return left.key == right.key && left.parent == right.parent && left.object == right.object &&
                       left.effectiveLocalTransform == right.effectiveLocalTransform;
            });
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
                                      const EffectivePrefabCandidate &expected, const Assets::AssetRegistrySnapshot &registry,
                                      std::vector<PrefabDependencySource> sources, std::shared_ptr<const PrefabValueSchemaSnapshot> schemas,
                                      const PrefabLimitProfile &limits, const CancellationToken &cancellation) {
            auto next = BuildPrefabSourceResolverSnapshot(registry, std::move(sources), limits, {}, schemas);
            if (next.HasError())
                return Result<void>::Failure(std::move(next).ErrorValue());
            const auto rebuilt = next.Value().Resolve(prepared.source.Data().assetId, command.instance, limits, cancellation);
            if (rebuilt.HasError())
                return Result<void>::Failure(rebuilt.ErrorValue());
            if (!SameSourceEffect(expected, rebuilt.Value()))
                return Result<void>::Failure(
                    MakeError(PrefabErrors::OverrideConflict, "The owning source cannot preserve the selected effective values."));
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
            if (source == sources.Sources().end())
                return Return::Failure(MakeError(PrefabErrors::DependencyUnavailable));
            auto selected = ResolvePrefabValueOverrides(original.Value(), chosen.Value(), schemas.get(), limits, cancellation);
            if (selected.HasError())
                return Return::Failure(std::move(selected).ErrorValue());
            if (!selected.Value().effective)
                return Return::Failure(MakeError(PrefabErrors::OverrideConflict));
            LayerPreparation layers{sources, schemas.get(), limits, cancellation, command.instance, PrefabExpansionBudget{limits}};
            auto updated = PromoteSource(source->document, *selected.Value().effective, selected.Value().rebased, layers);
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
            if (auto result = RebasePlacements(prepared, command, *selected.Value().effective, registry, std::move(updatedSources),
                                               std::move(schemas), limits, cancellation);
                result.HasError())
                return Return::Failure(std::move(result).ErrorValue());
            return Return::Success(std::move(prepared));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }
}  // namespace Horo::Editor
