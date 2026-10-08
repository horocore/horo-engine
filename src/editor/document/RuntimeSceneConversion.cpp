#include "editor/document/RuntimeSceneConversion.h"

namespace Horo::Editor {

    /** @copydoc BuildScenePrefabProjection */
    Result<ScenePrefabProjection> BuildScenePrefabProjection(const SceneDocumentSnapshot &document,
                                                             const Prefab::PrefabSourceResolverSnapshot &resolver,
                                                             const Prefab::PrefabLimitProfile &limits) {
        return SceneSource::BuildScenePrefabProjection({document.objects, document.prefabInstances}, resolver, limits);
    }

    /** @copydoc ConvertScenePrefabProjectionToRuntime */
    Result<Runtime::RuntimeSceneDefinition> ConvertScenePrefabProjectionToRuntime(const SceneDocumentSnapshot &document,
                                                                                  const Runtime::SceneDefinitionId sceneId,
                                                                                  const ScenePrefabProjection &projection,
                                                                                  const Prefab::PrefabSourceResolverSnapshot &resolver,
                                                                                  const Prefab::PrefabLimitProfile &limits) {
        return SceneSource::ConvertScenePrefabProjectionToRuntime({document.objects, document.prefabInstances}, sceneId,
                                                                  Runtime::SceneDefinitionRevision{document.state.value}, projection,
                                                                  resolver, limits);
    }

    /** @copydoc ConvertSceneDocumentToRuntime */
    Result<Runtime::RuntimeSceneDefinition> ConvertSceneDocumentToRuntime(const SceneDocumentSnapshot &document,
                                                                          const Runtime::SceneDefinitionId sceneId) {
        return SceneSource::ConvertSceneSourceToRuntime({document.objects, document.prefabInstances}, sceneId,
                                                        Runtime::SceneDefinitionRevision{document.state.value});
    }

    /** @copydoc ConvertSceneDocumentToRuntime */
    Result<Runtime::RuntimeSceneDefinition> ConvertSceneDocumentToRuntime(const SceneDocumentSnapshot &document,
                                                                          const Runtime::SceneDefinitionId sceneId,
                                                                          const Prefab::PrefabSourceResolverSnapshot &resolver,
                                                                          const Prefab::PrefabLimitProfile &limits) {
        auto projection = BuildScenePrefabProjection(document, resolver, limits);
        if (projection.HasError())
            return Result<Runtime::RuntimeSceneDefinition>::Failure(projection.ErrorValue());
        return ConvertScenePrefabProjectionToRuntime(document, sceneId, projection.Value(), resolver, limits);
    }
}  // namespace Horo::Editor
