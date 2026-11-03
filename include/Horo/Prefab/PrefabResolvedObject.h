#pragma once
/** @file PrefabResolvedObject.h
 * @brief Owned stable-address prefab values shared by canonical delta
 * preparation and source/cooked adapters.
 */
#include "Horo/Gameplay/BehaviorTypes.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Prefab/PrefabCompositionOrigin.h"
#include "Horo/Prefab/PrefabIdentity.h"

#include <optional>
#include <string>
#include <vector>

namespace Horo::Prefab {
    /** @brief One authored prefab-local hierarchy object and its portable payloads.
     */
    struct PrefabObjectNode final {
        LocalObjectId localId;                              /**< Stable sparse slot; zero is reserved for the root. */
        std::optional<LocalObjectId> parentLocalId;         /**< Empty only for the root object. */
        std::string name;                                   /**< Advisory UTF-8 display name, never identity. */
        Math::Transform localTransform;                     /**< Backend-neutral local transform. */
        std::vector<RawComponentPayload> components;        /**< Typed or opaque project component envelopes. */
        std::vector<Gameplay::BehaviorComponent> behaviors; /**< Portable schema-checked behavior payloads. */

        [[nodiscard]] bool operator==(const PrefabObjectNode &) const noexcept = default;
    };

    /** @brief One authored object copied into an effective revision-pinned
     * hierarchy. */
    struct ResolvedPrefabObject final {
        Assets::AssetId sourcePrefab;                         /**< Source asset that owns the authored object. */
        ExpandedPrefabObjectKey key;                          /**< Stable instance-qualified nested object identity. */
        std::optional<ExpandedPrefabObjectKey> parent;        /**< Effective parent, when the object is not a placement root. */
        PrefabObjectNode object;                              /**< Owned authored object data; contains no source path. */
        Math::Transform effectiveLocalTransform;              /**< Placement-adjusted transform
                                                                 used by Scene conversion. */
        Math::Transform placementTransform;                   /**< Exact mount TRS applied to a nested root;
                                                                 identity for ordinary source objects. */
        std::vector<PrefabCompositionOriginStep> originChain; /**< Bounded actual application lineage, independent of
                                                                 paths/labels. */

        [[nodiscard]] bool operator==(const ResolvedPrefabObject &) const noexcept = default;
    };

}  // namespace Horo::Prefab
