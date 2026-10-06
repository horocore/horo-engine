#pragma once

/**
 * @file RuntimeSceneConversion.h
 * @brief Editor-owned conversion from authoritative documents to immutable runtime definitions.
 */

#include "Horo/Prefab/PrefabSourceResolver.h"
#include "Horo/Runtime/Scene/RuntimeSceneDefinition.h"
#include "Horo/Scene/SceneRuntimeConversion.h"
#include "editor/document/SceneDocument.h"

#include <optional>
#include <vector>

namespace Horo::Editor {
    /**
     * @brief Converts one committed authoring snapshot to a validated backend-neutral runtime definition.
     * @param document Immutable committed document snapshot.
     * @param sceneId Stable logical identity of this editor preview scene.
     * @return Immutable definition or the first typed validation diagnostic. Unresolved prefab references reject the
     * conversion rather than producing a partial runtime candidate.
     */
    [[nodiscard]] Result<Runtime::RuntimeSceneDefinition> ConvertSceneDocumentToRuntime(const SceneDocumentSnapshot &document,
                                                                                        Runtime::SceneDefinitionId sceneId);

    /** @brief One immutable editor projection of an authored prefab placement. */
    using ScenePrefabInstanceProjection = SceneSource::ScenePrefabInstanceProjection;

    /** @brief Complete detached prefab projection for one scene-document snapshot. */
    using ScenePrefabProjection = SceneSource::ScenePrefabProjection;

    /**
     * @brief Resolves every authored placement into a repairable immutable editor projection.
     * @param document Immutable committed scene snapshot.
     * @param resolver Immutable source/resolver snapshot.
     * @param limits Captured bounded prefab policy.
     * @return Complete projection; failed placements remain authored and carry their typed error.
     */
    [[nodiscard]] Result<ScenePrefabProjection> BuildScenePrefabProjection(const SceneDocumentSnapshot &document,
                                                                           const Prefab::PrefabSourceResolverSnapshot &resolver,
                                                                           const Prefab::PrefabLimitProfile &limits);

    /**
     * @brief Converts authored scene content and all required prefab candidates transactionally.
     * @param document Immutable committed scene snapshot.
     * @param sceneId Stable logical identity of the runtime scene.
     * @param resolver Immutable source/resolver snapshot used for every placement.
     * @param limits Captured bounded prefab policy.
     * @return Complete runtime definition, or the typed failure for the first required placement.
     */
    [[nodiscard]] Result<Runtime::RuntimeSceneDefinition> ConvertSceneDocumentToRuntime(
        const SceneDocumentSnapshot &document, Runtime::SceneDefinitionId sceneId, const Prefab::PrefabSourceResolverSnapshot &resolver,
        const Prefab::PrefabLimitProfile &limits);
}  // namespace Horo::Editor
