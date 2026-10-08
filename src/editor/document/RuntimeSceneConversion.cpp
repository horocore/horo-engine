#include "editor/document/RuntimeSceneConversion.h"

namespace Horo::Editor {
    /** @copydoc BuildScenePrefabProjection */
    Result<ScenePrefabProjection> BuildScenePrefabProjection(const SceneDocumentSnapshot &document,
                                                             const Prefab::PrefabSourceResolverSnapshot &resolver,
                                                             const Prefab::PrefabLimitProfile &limits) {
        return SceneSource::BuildScenePrefabProjection({document.objects, document.prefabInstances}, resolver, limits);
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
        return SceneSource::ConvertSceneSourceToRuntime({document.objects, document.prefabInstances}, sceneId,
                                                        Runtime::SceneDefinitionRevision{document.state.value}, resolver, limits);
    }
}  // namespace Horo::Editor
