#include "Horo/Prefab/PrefabCookedOverrides.h"

#include "Horo/Prefab/PrefabErrors.h"

#include <algorithm>
#include <new>
#include <set>

namespace Horo::Prefab {
    namespace {
        /** @brief Reuses the cooked stable object address; the fixed transient adapter
         * scope issues no persistent/runtime identity. */
        ExpandedPrefabObjectKey Key(const CookedPrefabEntity &entity) {
            return {PrefabInstanceId::Create(1).Value(), entity.provenance.sourceObject};
        }

        /** @brief Converts one actual cooked entity without reading source files or
         * fabricating provider/schema authority. */
        ResolvedPrefabObject Object(const CookedPrefabData &data, std::size_t index) {
            const auto &entity = data.entities[index];
            PrefabObjectNode object{.localId = entity.provenance.sourceObject.SourceObject(), .localTransform = entity.localTransform};
            for (const auto &member : entity.members)
                std::visit([&object]<typename Member>(const Member &value) {
                    if constexpr (std::is_same_v<Member, RawComponentPayload>)
                        object.components.push_back(value);
                    else
                        object.behaviors.push_back(value);
                }, member);
            std::optional<ExpandedPrefabObjectKey> parent;
            if (entity.parent)
                parent = Key(data.entities[entity.parent->value]);
            return {entity.provenance.sourceAsset, Key(entity), std::move(parent), std::move(object), entity.localTransform, {}, {}};
        }

        /** @brief Admits a whole immutable cooked baseline before exposing any
         * operation-owned objects. */
        Result<std::vector<ResolvedPrefabObject>> Baseline(const CookedPrefab &source, const PrefabLimitProfile &limits) {
            using Outcome = Result<std::vector<ResolvedPrefabObject>>;
            const auto &data = source.Data();
            if (!data.rootSourceRevision)
                return Outcome::Failure(MakeError(PrefabErrors::OverrideSourceStale,
                                                  "This cooked prefab has no genuine root source revision; "
                                                  "recook the prefab before durable capture."));
            if (data.entities.empty() || data.entities.size() > limits.Policy().maximumObjectCount)
                return Outcome::Failure(MakeError(PrefabErrors::ObjectCountExceeded));
            std::vector<ResolvedPrefabObject> objects;
            objects.reserve(data.entities.size());
            std::set<PrefabObjectAddress> addresses;
            for (std::size_t index = 0; index < data.entities.size(); ++index) {
                if (!addresses.insert(data.entities[index].provenance.sourceObject).second)
                    return Outcome::Failure(MakeError(PrefabErrors::IdentityCollision));
                objects.push_back(Object(data, index));
            }
            return Outcome::Success(std::move(objects));
        }
    }  // namespace

    /** @copydoc PrepareCookedPrefabOverrideBaseline */
    Result<std::vector<ResolvedPrefabObject>> PrepareCookedPrefabOverrideBaseline(const CookedPrefab &source,
                                                                                  const PrefabLimitProfile &limits) {
        try {
            return Baseline(source, limits);
        } catch (const std::bad_alloc &) {
            return Result<std::vector<ResolvedPrefabObject>>::Failure(MakeError(PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc ResolveCookedPrefabOverrides */
    Result<PrefabOverrideResolution> ResolveCookedPrefabOverrides(const CookedPrefab &source, const PrefabValueOverrideSet &overrides,
                                                                  const PrefabValueSchemaSnapshot *schemas,
                                                                  const PrefabLimitProfile &limits, const CancellationToken &cancellation) {
        auto objects = PrepareCookedPrefabOverrideBaseline(source, limits);
        if (objects.HasError())
            return Result<PrefabOverrideResolution>::Failure(objects.ErrorValue());
        const auto &data = source.Data();
        const PrefabOverrideBaseline baseline{data.assetId, *data.rootSourceRevision, objects.Value()};
        return ResolvePrefabValueOverrides(baseline, overrides, schemas, limits, cancellation);
    }
}  // namespace Horo::Prefab
