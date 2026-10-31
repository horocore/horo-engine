#pragma once

/** @file SceneRuntimeConversion.h
 * @brief Headless immutable authored-scene and prefab projection into runtime-owned definitions.
 */
#include "Horo/Prefab/PrefabDebugSnapshot.h"
#include "Horo/Prefab/PrefabExpansionCache.h"
#include "Horo/Prefab/PrefabSourceResolver.h"
#include "Horo/Prefab/PrefabValueOverrides.h"
#include "Horo/Prefab/PrefabValueSnapshot.h"
#include "Horo/Runtime/Scene/RuntimeSceneDefinition.h"
#include "Horo/Scene/SceneSource.h"

namespace Horo::SceneSource {
    /** @brief Owned project capture for one conversion operation; no mutable project service is retained.
     * @details The host supplies the selected project profile/material and exact captured semantics. The live
     * filter generation is only an operation freshness fence, never written into cooked scene evidence.
     */
    struct TriggerVolumeConversionContext final {
        Runtime::TriggerVolumeProjectBinding binding;
        std::uint64_t collisionFilterGeneration{};
        Sha256Digest materialSourceDigest; /**< Exact source-byte fence used by the cook host; never runtime behavior. */
        [[nodiscard]] bool operator==(const TriggerVolumeConversionContext &) const noexcept = default;
    };

    /** @brief Detached result for one authored placement; failures retain the original repairable reference. */
    struct ScenePrefabInstanceProjection final {
        ScenePrefabInstance authored;
        std::optional<Prefab::EffectivePrefabCandidate> expanded;
        std::optional<Error> failure;
        std::optional<Prefab::PrefabValueOverrideSet> rebasedValues;      /**< Repairable rebase evidence, never runtime component data. */
        std::shared_ptr<const Prefab::PrefabValueSnapshot> valueSnapshot; /**< Bounded immutable value/default/diff/origin inspection. */
        std::optional<Error> valueInspectionFailure; /**< Read-only inspection failure never rejects an otherwise valid source expansion. */
        Sha256Digest schemaRevision;                 /**< Exact copied schema context used for value resolution. */
        bool stale{false};                           /**< Retained expansion requires explicit re-resolution before publication. */
        std::optional<Prefab::PrefabDebugSnapshot> debugSnapshot; /**< Bounded detached inspection captured during preparation. */
        std::optional<Error> debugCaptureFailure; /**< Inspection failure never changes canonical expansion/publication success. */
        bool debugCaptureOmitted{false};          /**< Shared scene inspection row budget exhausted before this placement. */

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

    /** @brief Coherent borrowed source publication and policy for one synchronous retained conversion.
     * @details The caller keeps every referenced value alive and immutable until conversion returns.
     * Conversion does not retain the context, copy snapshots or discover project services.
     */
    struct ScenePrefabResolutionContext final {
        const Prefab::PrefabSourceResolverSnapshot &resolver; /**< Exact current immutable source publication. */
        const Prefab::PrefabLimitProfile &limits;             /**< Captured bounded expansion and publication policy. */
        const Prefab::PrefabValueSchemaSnapshot *schemas{};   /**< Optional current immutable property schema. */
    };

    /**
     * @brief Resolves every placement using the single Prefab-domain resolver without source I/O or activation.
     * @param document Borrowed coherent authored values; retained only for this call.
     * @param resolver Exact immutable source snapshot.
     * @param schemas Explicit owned schema metadata borrowed synchronously; null preserves unavailable property intent.
     * @param limits Captured bounded prefab policy.
     * @return Owned projection including typed errors and authored references for failed placements.
     */
    [[nodiscard]] Result<ScenePrefabProjection> BuildScenePrefabProjection(const SceneSourceView &document,
                                                                           const Prefab::PrefabSourceResolverSnapshot &resolver,
                                                                           const Prefab::PrefabLimitProfile &limits,
                                                                           const Prefab::PrefabValueSchemaSnapshot *schemas = nullptr);

    /**
     * @brief Uses the owner cache for complete equivalent-to-fresh prefab candidates before ordinary Scene conversion.
     * @param document Coherent authored values borrowed only during this call.
     * @param resolver Exact immutable source publication. @param limits Complete captured expansion policy.
     * @param cache Calling document/cook owner's bounded cache, accessed only on its owning thread.
     * @param schemas Immutable schema metadata borrowed only for this call.
     * @param cancellation Cooperative token checked between placements and before result publication.
     * @return Owned projection or cancellation/capacity failure; individual malformed sources retain repairable errors.
     */
    [[nodiscard]] Result<ScenePrefabProjection> BuildScenePrefabProjection(const SceneSourceView &document,
                                                                           const Prefab::PrefabSourceResolverSnapshot &resolver,
                                                                           const Prefab::PrefabLimitProfile &limits,
                                                                           Prefab::PrefabExpansionCache &cache,
                                                                           const CancellationToken &cancellation = {},
                                                                           const Prefab::PrefabValueSchemaSnapshot *schemas = nullptr);

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
     * @param context Current immutable resolver, bounded policy and schema; mismatched retained evidence rejects publication.
     * @param triggerBinding Explicit project authority required by enabled legacy triggers.
     * @return Complete immutable runtime definition or typed failure, without partial publication.
     * @details The owner must invalidate retained projections for intervening resource publications before conversion.
     */
    [[nodiscard]] Result<Runtime::RuntimeSceneDefinition> ConvertScenePrefabProjectionToRuntime(
        const SceneSourceView &document, Runtime::SceneDefinitionId sceneId, Runtime::SceneDefinitionRevision revision,
        const ScenePrefabProjection &projection, const ScenePrefabResolutionContext &context,
        const std::optional<TriggerVolumeConversionContext> &triggerBinding = {});

    /**
     * @brief Converts an ordinary source-free authored scene into one validated runtime definition.
     * @param document Coherent authored values; raw prefab placements are rejected by this overload.
     * @param sceneId Stable containing scene identity, never a process-local runtime handle.
     * @param revision Captured authored revision owned by the host/document.
     * @param triggerBinding Explicit project authority required by enabled legacy triggers.
     * @return Complete immutable definition or typed conversion/aggregate validation failure.
     */
    [[nodiscard]] Result<Runtime::RuntimeSceneDefinition> ConvertSceneSourceToRuntime(
        const SceneSourceView &document, Runtime::SceneDefinitionId sceneId, Runtime::SceneDefinitionRevision revision,
        const std::optional<TriggerVolumeConversionContext> &triggerBinding = {});

    /**
     * @brief Expands all static placements and publishes only one complete validated containing scene.
     * @param document Coherent authored objects and placements.
     * @param sceneId Stable logical scene identity.
     * @param revision Exact authored revision captured before conversion.
     * @param resolver Immutable source/resolver snapshot used for every placement.
     * @param schemas Explicit owned schema metadata borrowed synchronously; null preserves unavailable property intent.
     * @param limits Captured bounded prefab policy.
     * @param triggerBinding Explicit project authority required by enabled legacy triggers.
     * @return Complete runtime definition with no source prefab references, or typed failure; no partial scene escapes.
     * @details Component schema projection, collision-checked identity mapping and hierarchy expansion use the same
     * implementation as Editor previews. No lifecycle callback, provider discovery, source mutation or runtime activation occurs.
     */
    [[nodiscard]] Result<Runtime::RuntimeSceneDefinition> ConvertSceneSourceToRuntime(
        const SceneSourceView &document, Runtime::SceneDefinitionId sceneId, Runtime::SceneDefinitionRevision revision,
        const Prefab::PrefabSourceResolverSnapshot &resolver, const Prefab::PrefabLimitProfile &limits,
        const Prefab::PrefabValueSchemaSnapshot *schemas = nullptr,
        const std::optional<TriggerVolumeConversionContext> &triggerBinding = {});

    /** @brief Explicit synchronous preparation policy for cached Prefab value conversion; never retained by the cache. */
    struct ScenePrefabCachedConversionOptions final {
        CancellationToken cancellation;                               /**< Cooperative preparation fence. */
        const Prefab::PrefabValueSchemaSnapshot *schemas{};           /**< Borrowed immutable property schema for this conversion only. */
        std::optional<TriggerVolumeConversionContext> triggerBinding; /**< Owned project binding for this exact conversion. */
    };

    /**
     * @brief Converts actual cached expansion through the same all-or-nothing SceneSource transaction.
     * @param document Coherent authored objects and placement values, including current transforms and parents.
     * @param sceneId Stable logical scene identity. @param revision Exact containing document revision.
     * @param resolver Immutable source publication. @param limits Complete captured policy.
     * @param options Explicit synchronous schema, trigger binding and cooperative preparation policy.
     * @param cache Explicit owner-thread memoization authority.
     * @return Complete source-free runtime definition, or typed failure; no partial definition escapes.
     */
    [[nodiscard]] Result<Runtime::RuntimeSceneDefinition> ConvertSceneSourceToRuntime(
        const SceneSourceView &document, Runtime::SceneDefinitionId sceneId, Runtime::SceneDefinitionRevision revision,
        const Prefab::PrefabSourceResolverSnapshot &resolver, const Prefab::PrefabLimitProfile &limits, Prefab::PrefabExpansionCache &cache,
        const ScenePrefabCachedConversionOptions &options = {});
}  // namespace Horo::SceneSource
