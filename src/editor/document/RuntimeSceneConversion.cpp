#include "editor/document/RuntimeSceneConversion.h"

namespace Horo::Editor {

    /** @copydoc BuildScenePrefabProjection */
    Result<ScenePrefabProjection> BuildScenePrefabProjection(const SceneDocumentSnapshot &document,
                                                             const Prefab::PrefabSourceResolverSnapshot &resolver,
                                                             const Prefab::PrefabLimitProfile &limits,
                                                             const Prefab::PrefabValueSchemaSnapshot *schemas) {
        return SceneSource::BuildScenePrefabProjection({document.objects, document.prefabInstances}, resolver, limits, schemas);
    }

    /** @copydoc ConvertScenePrefabProjectionToRuntime */
    Result<Runtime::RuntimeSceneDefinition> ConvertScenePrefabProjectionToRuntime(
        const SceneDocumentSnapshot &document, const Runtime::SceneDefinitionId sceneId, const ScenePrefabProjection &projection,
        const Prefab::PrefabSourceResolverSnapshot &resolver, const Prefab::PrefabLimitProfile &limits,
        const Prefab::PrefabValueSchemaSnapshot *schemas,
        const std::optional<SceneSource::TriggerVolumeConversionContext> &triggerBinding) {
        return SceneSource::ConvertScenePrefabProjectionToRuntime({document.objects, document.prefabInstances}, sceneId,
                                                                  Runtime::SceneDefinitionRevision{document.state.value}, projection,
                                                                  resolver, limits, schemas, triggerBinding);
    }

    /** @copydoc ConvertSceneDocumentToRuntime */
    Result<Runtime::RuntimeSceneDefinition> ConvertSceneDocumentToRuntime(
        const SceneDocumentSnapshot &document, const Runtime::SceneDefinitionId sceneId,
        const std::optional<SceneSource::TriggerVolumeConversionContext> &triggerBinding) {
        return SceneSource::ConvertSceneSourceToRuntime({document.objects, document.prefabInstances}, sceneId,
                                                        Runtime::SceneDefinitionRevision{document.state.value}, triggerBinding);
    }

    /** @copydoc ConvertSceneDocumentToRuntime */
    Result<Runtime::RuntimeSceneDefinition> ConvertSceneDocumentToRuntime(
        const SceneDocumentSnapshot &document, const Runtime::SceneDefinitionId sceneId,
        const Prefab::PrefabSourceResolverSnapshot &resolver, const Prefab::PrefabLimitProfile &limits,
        const Prefab::PrefabValueSchemaSnapshot *schemas,
        const std::optional<SceneSource::TriggerVolumeConversionContext> &triggerBinding) {
        auto projection = BuildScenePrefabProjection(document, resolver, limits, schemas);
        if (projection.HasError())
            return Result<Runtime::RuntimeSceneDefinition>::Failure(projection.ErrorValue());
        return ConvertScenePrefabProjectionToRuntime(document, sceneId, projection.Value(), resolver, limits, schemas, triggerBinding);
    }
}  // namespace Horo::Editor
