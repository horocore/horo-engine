#pragma once

/**
 * @file PrefabSceneIdentityRemap.h
 * @brief Deterministic prefab-local to scene identity mapping and reference rewrite contract.
 */

#include "Horo/Prefab/PrefabSourceResolver.h"

#include <compare>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Prefab {
    /** @brief Version of the canonical prefab-to-scene identity hash contract. */
    inline constexpr std::uint32_t PrefabSceneIdentityHashVersion = 1;

    /** @brief Stable authored scene-object identity produced before Runtime entity allocation. */
    struct PrefabSceneObjectId final {
        std::uint64_t value{};

        /** @brief Reports whether this value is usable. @return True for non-zero identities. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const PrefabSceneObjectId &) const noexcept = default;
    };

    /** @brief Provenance-preserving mapping from one expanded object to its scene identity. */
    struct PrefabSceneIdentityMapping final {
        ExpandedPrefabObjectKey source; /**< Exact instance, nested scope and source-local object. */
        Assets::AssetId sourcePrefab;   /**< Stable source asset identity. */
        PrefabSceneObjectId scene;      /**< Deterministic collision-checked scene identity. */

        [[nodiscard]] auto operator<=>(const PrefabSceneIdentityMapping &) const noexcept = default;
    };

    /** @brief Typed reference category rewritten during the same identity transaction. */
    enum class PrefabReferenceKind : std::uint8_t {
        Entity,
        Component,
        Behavior,
        Asset
    };

    /** @brief One explicit reference site and its stable prefab-local or asset target. */
    struct PrefabReferenceRewriteRequest final {
        ExpandedPrefabObjectKey owner;                              /**< Object containing the reference site. */
        PrefabReferenceKind kind{PrefabReferenceKind::Entity};      /**< Target category and validation contract. */
        std::optional<ExpandedPrefabObjectKey> objectTarget;        /**< Required for entity/component/behavior targets. */
        std::optional<PrefabComponentInstanceId> componentTarget;   /**< Required only for component targets. */
        std::optional<Gameplay::BehaviorInstanceId> behaviorTarget; /**< Required only for behavior targets. */
        std::optional<Assets::AssetId> assetTarget;                 /**< Required only for asset targets. */
    };

    /** @brief One reference rewritten to scene identity while retaining typed member provenance. */
    struct RewrittenPrefabReference final {
        PrefabSceneObjectId owner;                                  /**< Mapped scene object containing the reference. */
        PrefabReferenceKind kind{PrefabReferenceKind::Entity};      /**< Preserved reference category. */
        std::optional<PrefabSceneObjectId> objectTarget;            /**< Mapped entity owning the referenced member. */
        std::optional<PrefabComponentInstanceId> componentTarget;   /**< Stable component occurrence when applicable. */
        std::optional<Gameplay::BehaviorInstanceId> behaviorTarget; /**< Stable behavior occurrence when applicable. */
        std::optional<Assets::AssetId> assetTarget;                 /**< Stable asset identity when applicable. */
    };

    /** @brief Complete immutable all-or-nothing remap result. */
    class PrefabSceneIdentityMap final {
    public:
        /** @brief Returns the source resolver revision context. @return Registry and root source revisions. */
        [[nodiscard]] const PrefabResolutionRevision &Revision() const noexcept;
        /** @brief Returns canonical mappings ordered by expanded source key. @return Borrowed immutable mappings. */
        [[nodiscard]] std::span<const PrefabSceneIdentityMapping> Mappings() const noexcept;
        /** @brief Returns rewritten references in request order. @return Borrowed immutable references. */
        [[nodiscard]] std::span<const RewrittenPrefabReference> References() const noexcept;
        /** @brief Looks up one expanded object identity. @param source Exact source key. @return Mapped identity or null. */
        [[nodiscard]] std::optional<PrefabSceneObjectId> Find(const ExpandedPrefabObjectKey &source) const noexcept;

    private:
        friend Result<PrefabSceneIdentityMap> RemapPrefabCandidateToScene(const EffectivePrefabCandidate &,
                                                                          std::span<const PrefabSceneObjectId>,
                                                                          std::span<const PrefabReferenceRewriteRequest>,
                                                                          const PrefabLimitProfile &);

        PrefabSceneIdentityMap(PrefabResolutionRevision revision, std::vector<PrefabSceneIdentityMapping> mappings,
                               std::vector<RewrittenPrefabReference> references) noexcept;

        PrefabResolutionRevision revision_{};
        std::vector<PrefabSceneIdentityMapping> mappings_;
        std::vector<RewrittenPrefabReference> references_;
    };

    /**
     * @brief Deterministically maps a complete prefab candidate and rewrites every declared reference atomically.
     * @param candidate Immutable resolver candidate whose provenance is retained.
     * @param occupied Authored or previously expanded scene identities that must not be overwritten.
     * @param references Typed reference sites to rewrite as one transaction.
     * @param limits Captured project policy bounding mappings and reference work.
     * @return Complete immutable remap, or a typed collision, malformed-reference or bounds error.
     */
    [[nodiscard]] Result<PrefabSceneIdentityMap> RemapPrefabCandidateToScene(const EffectivePrefabCandidate &candidate,
                                                                             std::span<const PrefabSceneObjectId> occupied,
                                                                             std::span<const PrefabReferenceRewriteRequest> references,
                                                                             const PrefabLimitProfile &limits);
}  // namespace Horo::Prefab
