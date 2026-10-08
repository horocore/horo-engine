#pragma once

/** @file PrefabSceneExpansion.h
 * @brief Pure bounded prefab-to-runtime hierarchy handoff, independent of editor hosts.
 */
#include "Horo/Prefab/PrefabSceneIdentityRemap.h"
#include "Horo/Runtime/Scene/RuntimeSceneDefinition.h"

namespace Horo::Prefab {
    /** @brief Complete inert schema-adapter output for one exact resolved object. */
    struct PrefabRuntimeComponentProjection final {
        ExpandedPrefabObjectKey object;          /**< Stable source key, never an ordinal or name. */
        Runtime::RuntimeComponentSet components; /**< Owned typed payload; no provider callbacks or source paths. */
    };

    /** @brief Placement facts captured with the containing immutable scene snapshot. */
    struct PrefabRuntimePlacement final {
        Math::Transform rootTransform;                /**< Composed only onto the outer root. */
        std::optional<Runtime::SceneObjectId> parent; /**< External scene parent, validated by the containing scene builder. */
    };

    /** @brief Complete detached subtree; only its const entity view may reach a scene transaction. */
    class ExpandedPrefabSceneSubtree final {
    public:
        /** @brief Returns entities in resolved parent-before-child and sibling order. @return Immutable owned definitions. */
        [[nodiscard]] std::span<const Runtime::RuntimeEntityDefinition> Entities() const noexcept;
        /** @brief Returns the pinned source publication context. @return Exact immutable resolution revision. */
        [[nodiscard]] const PrefabResolutionRevision &Revision() const noexcept;

        /** @brief Checks retained runtime-preview evidence before replacing a scene generation.
         * @param rootAsset Root identity captured by the preview owner.
         * @param current Current immutable source resolver.
         * @param changedAssets Complete publication identities since Revision(), including resource content changes.
         * @param limits Bounded publication inspection policy.
         * @return Success only for an unaffected graph; stale evidence never mutates this subtree or active leases.
         */
        [[nodiscard]] Result<void> ValidatePublication(Assets::AssetId rootAsset, const PrefabSourceResolverSnapshot &current,
                                                       std::span<const Assets::AssetId> changedAssets,
                                                       const PrefabLimitProfile &limits) const;

    private:
        friend Result<ExpandedPrefabSceneSubtree> ExpandPrefabSceneSubtree(const EffectivePrefabCandidate &, const PrefabSceneIdentityMap &,
                                                                           std::span<const PrefabRuntimeComponentProjection>,
                                                                           const PrefabRuntimePlacement &, const PrefabLimitProfile &);
        ExpandedPrefabSceneSubtree(PrefabResolutionRevision revision, std::vector<Runtime::RuntimeEntityDefinition> entities) noexcept;
        PrefabResolutionRevision revision_;
        std::vector<Runtime::RuntimeEntityDefinition> entities_;
    };

    /**
     * @brief Produces every runtime entity of a resolved prefab as one bounded transaction.
     * @param candidate Owned immutable resolver result; source snapshots may already be retired.
     * @param identities Complete collision-checked remap from the same candidate and revision.
     * @param components Exactly one typed schema projection per source key, in any order. Adapters must preserve payload semantics.
     * @param placement Immutable containing-scene transform and optional parent.
     * @param limits Captured project policy; checked again even if resolution used a larger profile.
     * @return Complete source-free subtree, or typed failure with no partial output or input mutation.
     * @note Load/cook-time only. No I/O, registry lookup, lifecycle callbacks, or runtime activation.
     * The containing scene builder owns external-parent validation and publication of all placements.
     */
    [[nodiscard]] Result<ExpandedPrefabSceneSubtree> ExpandPrefabSceneSubtree(const EffectivePrefabCandidate &candidate,
                                                                              const PrefabSceneIdentityMap &identities,
                                                                              std::span<const PrefabRuntimeComponentProjection> components,
                                                                              const PrefabRuntimePlacement &placement,
                                                                              const PrefabLimitProfile &limits);
}  // namespace Horo::Prefab
