#pragma once

/** @file SceneRuntimeConversion.h
 * @brief Headless immutable authored-scene and prefab projection into runtime-owned definitions.
 */
#include "Horo/Prefab/PrefabSourceResolver.h"
#include "Horo/Runtime/Scene/RuntimeSceneDefinition.h"
#include "Horo/Scene/SceneSource.h"

namespace Horo::SceneSource {
    /** @brief Detached result for one authored placement; failures retain the original repairable reference. */
    struct ScenePrefabInstanceProjection final {
        ScenePrefabInstance authored;
        std::optional<Prefab::EffectivePrefabCandidate> expanded;
        std::optional<Error> failure;

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
}  // namespace Horo::SceneSource
