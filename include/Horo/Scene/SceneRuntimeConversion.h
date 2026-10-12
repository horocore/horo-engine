#pragma once

/** @file SceneRuntimeConversion.h
 * @brief Headless immutable authored-scene and prefab projection into runtime-owned definitions.
 */
#include "Horo/Prefab/PrefabExpansionCache.h"
#include "Horo/Prefab/PrefabSourceResolver.h"
#include "Horo/Runtime/Scene/RuntimeSceneDefinition.h"
#include "Horo/Scene/SceneSource.h"

namespace Horo::SceneSource {
    /** @brief Detached result for one authored placement; failures retain the original repairable reference. */
    struct ScenePrefabInstanceProjection final {
        ScenePrefabInstance authored;
        std::optional<Prefab::EffectivePrefabCandidate> expanded;
        std::optional<Error> failure;
        bool stale{false}; /**< Retained expansion requires explicit re-resolution before publication. */

        /** @brief Reports whether the retained candidate matches its inspected publication. @return True for a complete current candidate.
         */
        [[nodiscard]] bool IsSynchronized() const noexcept {
            return expanded.has_value() && !failure && !stale;
        }

        /** @brief Reports unresolved required content. @return True when this placement failed resolution. */
        [[nodiscard]] bool IsBroken() const noexcept {
            return failure.has_value();
        }
    };

    /** @brief Complete immutable-by-convention projection owned by the calling document or cook operation. */
    struct ScenePrefabProjection final {
        std::vector<ScenePrefabInstanceProjection> instances;
        /** @brief Reports unresolved placements. @return True when at least one required placement failed. */
        [[nodiscard]] bool HasBrokenInstances() const noexcept;
    };

    /**
     * @brief Resolves every placement using the single Prefab-domain resolver without source I/O or activation.
     * @param document Borrowed coherent authored values; retained only for this call.
     * @param resolver Exact immutable source snapshot.
     * @param limits Captured bounded prefab policy.
     * @return Owned projection including typed errors and authored references for failed placements.
     */
    [[nodiscard]] Result<ScenePrefabProjection> BuildScenePrefabProjection(const SceneSourceView &document,
                                                                           const Prefab::PrefabSourceResolverSnapshot &resolver,
                                                                           const Prefab::PrefabLimitProfile &limits);

    /**
     * @brief Uses the owner cache for complete equivalent-to-fresh prefab candidates before ordinary Scene conversion.
     * @param document Coherent authored values borrowed only during this call.
     * @param resolver Exact immutable source publication. @param limits Complete captured expansion policy.
     * @param cache Calling document/cook owner's bounded cache, accessed only on its owning thread.
     * @param cancellation Cooperative token checked between placements and before result publication.
     * @return Owned projection or cancellation/capacity failure; individual malformed sources retain repairable errors.
     */
    [[nodiscard]] Result<ScenePrefabProjection> BuildScenePrefabProjection(const SceneSourceView &document,
                                                                           const Prefab::PrefabSourceResolverSnapshot &resolver,
                                                                           const Prefab::PrefabLimitProfile &limits,
                                                                           Prefab::PrefabExpansionCache &cache,
                                                                           const CancellationToken &cancellation = {});

    /**
     * @brief Marks retained projections stale after affected source/resource publications without replacing their evidence.
     * @param projection Owned retained placements, updated on their owner's thread.
     * @param current Current immutable resolver publication.
     * @param changedAssets Complete intervening publication identities, including ordinary resources.
     * @param limits Bounded revision inspection policy.
     * @details Staleness remains sticky until rebuilding the projection; authored references remain available for repair.
     */
    void InvalidateScenePrefabProjection(ScenePrefabProjection &projection, const Prefab::PrefabSourceResolverSnapshot &current,
                                         std::span<const Assets::AssetId> changedAssets, const Prefab::PrefabLimitProfile &limits);

    /**
     * @brief Converts retained prefab candidates only after validating authored and current source evidence.
     * @param document Coherent authored values captured by the calling host.
     * @param sceneId Stable containing scene identity.
     * @param revision Captured authored revision.
     * @param projection Retained candidates; stale, broken or mismatched placements reject the complete conversion.
     * @param resolver Current immutable source publication.
     * @param limits Bounded conversion and publication policy.
     * @return Complete immutable runtime definition or typed failure, without partial publication.
     * @details The owner must invalidate retained projections for intervening resource publications before conversion.
     */
    [[nodiscard]] Result<Runtime::RuntimeSceneDefinition> ConvertScenePrefabProjectionToRuntime(
        const SceneSourceView &document, Runtime::SceneDefinitionId sceneId, Runtime::SceneDefinitionRevision revision,
        const ScenePrefabProjection &projection, const Prefab::PrefabSourceResolverSnapshot &resolver,
        const Prefab::PrefabLimitProfile &limits);

    /**
     * @brief Converts an ordinary source-free authored scene into one validated runtime definition.
     * @param document Coherent authored values; raw prefab placements are rejected by this overload.
     * @param sceneId Stable containing scene identity, never a process-local runtime handle.
     * @param revision Captured authored revision owned by the host/document.
     * @return Complete immutable definition or typed conversion/aggregate validation failure.
     */
    [[nodiscard]] Result<Runtime::RuntimeSceneDefinition> ConvertSceneSourceToRuntime(const SceneSourceView &document,
                                                                                      Runtime::SceneDefinitionId sceneId,
                                                                                      Runtime::SceneDefinitionRevision revision);

    /**
     * @brief Expands all static placements and publishes only one complete validated containing scene.
     * @param document Coherent authored objects and placements.
     * @param sceneId Stable logical scene identity.
     * @param revision Exact authored revision captured before conversion.
     * @param resolver Immutable source/resolver snapshot used for every placement.
     * @param limits Captured bounded prefab policy.
     * @return Complete runtime definition with no source prefab references, or typed failure; no partial scene escapes.
     * @details Component schema projection, collision-checked identity mapping and hierarchy expansion use the same
     * implementation as Editor previews. No lifecycle callback, provider discovery, source mutation or runtime activation occurs.
     */
    [[nodiscard]] Result<Runtime::RuntimeSceneDefinition> ConvertSceneSourceToRuntime(const SceneSourceView &document,
                                                                                      Runtime::SceneDefinitionId sceneId,
                                                                                      Runtime::SceneDefinitionRevision revision,
                                                                                      const Prefab::PrefabSourceResolverSnapshot &resolver,
                                                                                      const Prefab::PrefabLimitProfile &limits);

    /**
     * @brief Converts actual cached expansion through the same all-or-nothing SceneSource transaction.
     * @param document Coherent authored objects and placement values, including current transforms and parents.
     * @param sceneId Stable logical scene identity. @param revision Exact containing document revision.
     * @param resolver Immutable source publication. @param limits Complete captured policy.
     * @param cache Explicit owner-thread memoization authority. @param cancellation Cooperative operation cancellation.
     * @return Complete source-free runtime definition, or typed failure; no partial definition escapes.
     */
    [[nodiscard]] Result<Runtime::RuntimeSceneDefinition> ConvertSceneSourceToRuntime(
        const SceneSourceView &document, Runtime::SceneDefinitionId sceneId, Runtime::SceneDefinitionRevision revision,
        const Prefab::PrefabSourceResolverSnapshot &resolver, const Prefab::PrefabLimitProfile &limits, Prefab::PrefabExpansionCache &cache,
        const CancellationToken &cancellation = {});
}  // namespace Horo::SceneSource
