#include "Horo/Prefab/PrefabErrors.h"
#include "PrefabSourceExpansionInternal.h"

#include <algorithm>
#include <array>

namespace Horo::Prefab {
    /** @brief The source resolver's private bridge to the same detached value/component resolver. */
    class PrefabSourceLayerResolver final {
    public:
        /** @brief Creates only an operation-owned source slice, never a published partial hierarchy. */
        static EffectivePrefabCandidate Slice(const PrefabDependencySource &source, std::vector<ResolvedPrefabObject> objects,
                                              const PrefabValueSchemaSnapshot *schemas) {
            PrefabResolutionRevision revision{{}, source.sourceRevision};
            revision.valueSchemaRevision = schemas ? schemas->Revision() : Sha256Digest{};
            return {source.document.Data().assetId, std::move(revision), std::move(objects)};
        }
    };

    namespace Detail {
        namespace {
            /** @brief Prefixes only admitted stable placement IDs; fixed scratch rejects overflow before copying. */
            Result<PrefabObjectAddress> Prefix(const PrefabObjectAddress &address, const std::span<const LocalObjectId> prefix) {
                const auto suffix = address.NestedInstanceScope();
                if (prefix.size() > MaximumPrefabObjectScopeDepth || suffix.size() > MaximumPrefabObjectScopeDepth - prefix.size())
                    return Result<PrefabObjectAddress>::Failure(MakeError(PrefabErrors::AddressInvalid));
                std::array<LocalObjectId, MaximumPrefabObjectScopeDepth> scope;
                std::ranges::copy(prefix, scope.begin());
                std::ranges::copy(suffix, scope.begin() + prefix.size());
                return PrefabObjectAddress::Create(std::span{scope}.first(prefix.size() + suffix.size()), address.SourceObject());
            }

            /** @brief Removes only the exact containing occurrence prefix, never a guessed local object ID. */
            Result<PrefabObjectAddress> Relative(const PrefabObjectAddress &address, std::span<const LocalObjectId> prefix) {
                const auto scope = address.NestedInstanceScope();
                if (scope.size() < prefix.size() || !std::equal(prefix.begin(), prefix.end(), scope.begin()))
                    return Result<PrefabObjectAddress>::Failure(MakeError(PrefabErrors::AddressInvalid));
                return PrefabObjectAddress::Create(scope.subspan(prefix.size()), address.SourceObject());
            }

            /** @brief Copies a complete source slice into source-relative addresses for the one canonical override resolver. */
            Result<std::vector<ResolvedPrefabObject>> CaptureSlice(const SourceExpansionState &state, const std::size_t begin) {
                std::vector<ResolvedPrefabObject> objects(state.objects.begin() + begin, state.objects.end());
                for (auto &object : objects) {
                    auto key = Relative(object.key.object, state.scope);
                    if (key.HasError())
                        return Result<std::vector<ResolvedPrefabObject>>::Failure(key.ErrorValue());
                    object.key.object = key.Value();
                    if (object.key.object.NestedInstanceScope().empty() && object.key.object.SourceObject().IsRoot())
                        object.parent.reset();
                    else if (object.parent) {
                        auto parent = Relative(object.parent->object, state.scope);
                        if (parent.HasError())
                            return Result<std::vector<ResolvedPrefabObject>>::Failure(parent.ErrorValue());
                        object.parent->object = parent.Value();
                    }
                }
                return Result<std::vector<ResolvedPrefabObject>>::Success(std::move(objects));
            }

            /** @brief Remounts an already complete resolved slice, retaining the original root's actual containing parent. */
            Result<void> RestoreSlice(SourceExpansionState &state, const std::size_t begin, const EffectivePrefabCandidate &candidate) {
                std::vector<ResolvedPrefabObject> objects(candidate.Objects().begin(), candidate.Objects().end());
                for (auto &object : objects) {
                    const bool root = object.key.object.NestedInstanceScope().empty() && object.key.object.SourceObject().IsRoot();
                    auto key = Prefix(object.key.object, state.scope);
                    if (key.HasError())
                        return Result<void>::Failure(key.ErrorValue());
                    object.key.object = key.Value();
                    if (root)
                        object.parent = state.objects[begin].parent;
                    else if (object.parent) {
                        auto parent = Prefix(object.parent->object, state.scope);
                        if (parent.HasError())
                            return Result<void>::Failure(parent.ErrorValue());
                        object.parent->object = parent.Value();
                    }
                }
                std::move(objects.begin(), objects.end(), state.objects.begin() + begin);
                return Result<void>::Success();
            }

            /** @brief Charges each normal/preserved operation against the same whole-expansion work authority. */
            Result<void> ChargeLayer(SourceExpansionState &state, const PrefabValueOverrideSet &layer, std::size_t begin) {
                if (auto charged = state.budget.Consume(state.objects.size() - begin); charged.HasError())
                    return charged;
                for (const auto count : {layer.records.size(), layer.conflicts.size(), layer.orphans.size(), layer.componentRecords.size(),
                                         layer.componentConflicts.size(), layer.componentOrphans.size()})
                    if (auto charged = state.budget.Consume(count); charged.HasError())
                        return charged;
                return Result<void>::Success();
            }
        }  // namespace

        /** @copydoc ApplySourceOverrideLayer */
        Result<void> ApplySourceOverrideLayer(SourceExpansionState &state, const std::optional<PrefabValueOverrideSet> &layer,
                                              const PrefabDependencySource &source, Assets::AssetId owner,
                                              std::optional<LocalObjectId> placement, std::size_t begin) {
            if (!layer || state.unresolved)
                return Result<void>::Success();
            if (auto charged = ChargeLayer(state, *layer, begin); charged.HasError())
                return charged;
            auto objects = CaptureSlice(state, begin);
            if (objects.HasError())
                return Result<void>::Failure(objects.ErrorValue());
            auto candidate = PrefabSourceLayerResolver::Slice(source, std::move(objects).Value(), state.schemas);
            auto resolved = ResolvePrefabValueOverrides(candidate, *layer, state.schemas, state.limits, state.cancellation);
            if (resolved.HasError())
                return Result<void>::Failure(resolved.ErrorValue());
            auto value = std::move(resolved).Value();
            state.unresolved = !value.effective.has_value();
            state.layers.emplace_back(owner, placement, state.originChain, std::move(value.rebased));
            return state.unresolved ? Result<void>::Success() : RestoreSlice(state, begin, *value.effective);
        }
    }  // namespace Detail
}  // namespace Horo::Prefab
