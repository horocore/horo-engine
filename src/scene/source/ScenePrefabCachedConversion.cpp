#include "Horo/Scene/SceneRuntimeConversion.h"
#include "ScenePrefabProjectionContext.h"

#include <new>

namespace Horo::SceneSource {
    using Detail::AddInstanceContext;

    /** @copydoc BuildScenePrefabProjection */
    Result<ScenePrefabProjection> BuildScenePrefabProjection(const SceneSourceView &document,
                                                             const Prefab::PrefabSourceResolverSnapshot &resolver,
                                                             const Prefab::PrefabLimitProfile &limits, Prefab::PrefabExpansionCache &cache,
                                                             const CancellationToken &cancellation) {
        try {
            if (document.prefabInstances.size() > limits.Policy().maximumObjectCount)
                return Result<ScenePrefabProjection>::Failure(MakeError(Prefab::PrefabErrors::ObjectCountExceeded));
            ScenePrefabProjection projection;
            projection.instances.reserve(document.prefabInstances.size());
            for (const auto &instance : document.prefabInstances) {
                if (cancellation.IsCancellationRequested())
                    return Result<ScenePrefabProjection>::Failure(MakeError(Prefab::PrefabErrors::Cancelled));
                ScenePrefabInstanceProjection entry{.authored = instance};
                if (auto candidate = cache.Resolve(resolver, instance.sourcePrefab.Asset(), instance.instanceId, limits, cancellation);
                    candidate.HasError()) {
                    entry.failure = candidate.ErrorValue();
                    AddInstanceContext(*entry.failure, entry);
                } else
                    entry.expanded = *candidate.Value();
                projection.instances.push_back(std::move(entry));
            }
            if (cancellation.IsCancellationRequested())
                return Result<ScenePrefabProjection>::Failure(MakeError(Prefab::PrefabErrors::Cancelled));
            return Result<ScenePrefabProjection>::Success(std::move(projection));
        } catch (const std::bad_alloc &) {
            return Result<ScenePrefabProjection>::Failure(MakeError(Prefab::PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }

    /** @copydoc ConvertSceneSourceToRuntime */
    Result<Runtime::RuntimeSceneDefinition> ConvertSceneSourceToRuntime(
        const SceneSourceView &document, const Runtime::SceneDefinitionId sceneId, const Runtime::SceneDefinitionRevision revision,
        const Prefab::PrefabSourceResolverSnapshot &resolver, const Prefab::PrefabLimitProfile &limits, Prefab::PrefabExpansionCache &cache,
        const CancellationToken &cancellation) {
        try {
            auto projection = BuildScenePrefabProjection(document, resolver, limits, cache, cancellation);
            if (projection.HasError())
                return Result<Runtime::RuntimeSceneDefinition>::Failure(projection.ErrorValue());
            auto definition = ConvertScenePrefabProjectionToRuntime(document, sceneId, revision, projection.Value(), resolver, limits);
            if (cancellation.IsCancellationRequested())
                return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(Prefab::PrefabErrors::Cancelled));
            return definition;
        } catch (const std::bad_alloc &) {
            return Result<Runtime::RuntimeSceneDefinition>::Failure(MakeError(Prefab::PrefabErrors::ExpansionCacheAllocationFailed));
        }
    }
}  // namespace Horo::SceneSource
