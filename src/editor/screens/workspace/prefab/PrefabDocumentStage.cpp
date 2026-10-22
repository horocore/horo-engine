#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabSceneExpansion.h"
#include "Horo/Scene/PrefabComponentProjection.h"
#include "editor/screens/workspace/prefab/PrefabDocumentSession.h"

#include <limits>

namespace Horo::Editor {
    namespace {
        /** @brief Projects complete source payloads without instantiating gameplay providers or executing callbacks. */
        Result<std::vector<Prefab::PrefabRuntimeComponentProjection>> Components(const Prefab::EffectivePrefabCandidate &candidate) {
            std::vector<Prefab::PrefabRuntimeComponentProjection> result;
            for (const auto &object : candidate.Objects()) {
                Prefab::PrefabRuntimeComponentProjection projection{.object = object.key};
                for (const auto &component : object.object.components) {
                    auto projected = SceneSource::ProjectPrefabBuiltinComponent(component, projection.components, projection.primitiveMesh);
                    if (projected.HasError())
                        return Result<std::vector<Prefab::PrefabRuntimeComponentProjection>>::Failure(projected.ErrorValue());
                    if (!projected.Value())
                        projection.components.gameplayComponents.push_back(component.component);
                }
                projection.components.behaviors = object.object.behaviors;
                result.push_back(std::move(projection));
            }
            return Result<std::vector<Prefab::PrefabRuntimeComponentProjection>>::Success(std::move(result));
        }

        /** @brief Builds the entire runtime definition and actual registered asset requirements before any host activation. */
        Result<Runtime::RuntimeSceneDefinition> Definition(const Prefab::EffectivePrefabCandidate &candidate,
                                                           const Prefab::PrefabLimitProfile &limits, Runtime::SceneDefinitionId id,
                                                           Runtime::SceneDefinitionRevision revision) {
            auto identities = Prefab::RemapPrefabCandidateToScene(candidate, {}, {}, limits);
            if (identities.HasError())
                return Result<Runtime::RuntimeSceneDefinition>::Failure(identities.ErrorValue());
            auto components = Components(candidate);
            if (components.HasError())
                return Result<Runtime::RuntimeSceneDefinition>::Failure(components.ErrorValue());
            auto subtree = Prefab::ExpandPrefabSceneSubtree(candidate, identities.Value(), components.Value(), {}, limits);
            if (subtree.HasError())
                return Result<Runtime::RuntimeSceneDefinition>::Failure(subtree.ErrorValue());
            Runtime::SceneDefinitionBuilder builder(id, revision);
            for (const auto &entity : subtree.Value().Entities())
                builder.Add(entity);
            for (const auto &dependency : candidate.Revision().dependencies) {
                if (dependency.assetType.Value() == "core.prefab")
                    continue;
                if (auto required = builder.RequireAsset({dependency.assetId, dependency.assetType}); required.HasError())
                    return Result<Runtime::RuntimeSceneDefinition>::Failure(required.ErrorValue());
            }
            return std::move(builder).Build();
        }
    }  // namespace

    /** @copydoc PrefabDocumentSession::ActivateStage */
    Result<void> PrefabDocumentSession::ActivateStage(const Assets::AssetRegistrySnapshot &registry,
                                                      const Prefab::PrefabSourceResolverSnapshot &dependencies,
                                                      const Runtime::SceneRuntimeId runtimeId, const EditorPlaySceneFactory &factory) {
        if (auto admitted = Admit({owner_, identity_.instance, revision_}); admitted.HasError())
            return admitted;
        if (preview_.revision == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::WorkBudgetExceeded));
        std::vector<Prefab::PrefabDependencySource> sources;
        for (const auto &dependency : dependencies.Sources())
            if (dependency.document.Data().assetId != source_->Data().assetId)
                sources.push_back(dependency);
        sources.push_back({*source_, {source_->Data().projectVersion, state_}});
        auto resolver = Prefab::BuildPrefabSourceResolverSnapshot(registry, std::move(sources), limits_);
        if (resolver.HasError())
            return Result<void>::Failure(resolver.ErrorValue());
        auto candidate = resolver.Value().Resolve(source_->Data().assetId,
                                                  Prefab::PrefabInstanceId::Create(identity_.instance.Value()).Value(), limits_);
        if (candidate.HasError())
            return Result<void>::Failure(candidate.ErrorValue());
        auto definition = Definition(candidate.Value(), limits_, {identity_.instance.Value()}, {preview_.revision + 1});
        if (definition.HasError())
            return Result<void>::Failure(definition.ErrorValue());
        return InstallStage(std::move(definition).Value(), runtimeId, factory);
    }

    /** @copydoc PrefabDocumentSession::InstallStage */
    Result<void> PrefabDocumentSession::InstallStage(Runtime::RuntimeSceneDefinition definition, Runtime::SceneRuntimeId runtimeId,
                                                     const EditorPlaySceneFactory &factory) {
        if (factory) {
            auto prepared = factory(runtimeId, definition);
            if (prepared.HasError())
                return Result<void>::Failure(prepared.ErrorValue());
            if (!prepared.Value())
                return Result<void>::Failure(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
            CancellationSource cancellation;
            auto &scenes = prepared.Value()->Scenes();
            if (auto started = scenes.Startup(cancellation.Token()); started.HasError()) {
                prepared.Value()->Shutdown();
                return started;
            }
            if (auto queued = scenes.QueuePreparation(std::move(definition)); queued.HasError()) {
                prepared.Value()->Shutdown();
                return queued;
            }
            ReleaseStage();
            preview_.cancellation = std::move(cancellation);
            preview_.host = std::move(prepared).Value();
            preview_.frame = 1;
        } else {
            auto runtime = Runtime::RuntimeScene::Create(definition, runtimeId);
            if (runtime.HasError())
                return Result<void>::Failure(runtime.ErrorValue());
            ReleaseStage();
            preview_.scene = std::move(runtime).Value();
            ++preview_.revision;
        }
        return Result<void>::Success();
    }

    /** @copydoc PrefabDocumentSession::AdvanceStage */
    Result<bool> PrefabDocumentSession::AdvanceStage() {
        if (auto admitted = Admit({owner_, identity_.instance, revision_}); admitted.HasError())
            return Result<bool>::Failure(admitted.ErrorValue());
        if (!preview_.host)
            return Result<bool>::Success(preview_.scene != nullptr);
        if (preview_.published)
            return Result<bool>::Success(true);
        const Runtime::FrameContext frame{.frameNumber = preview_.frame++, .cancellation = preview_.cancellation.Token()};
        auto advanced = preview_.host->CommitSceneChanges(frame);
        if (auto failure = advanced.HasError() ? std::optional<Error>{advanced.ErrorValue()} : preview_.host->Scenes().TakeOperationError();
            failure) {
            ReleaseStage();
            return Result<bool>::Failure(*failure);
        }
        if (!preview_.host->Scenes().ActiveScene())
            return Result<bool>::Success(false);
        preview_.published = true;
        ++preview_.revision;
        return Result<bool>::Success(true);
    }

    /** @copydoc PrefabDocumentSession::StageView */
    std::optional<Runtime::RuntimeSceneView> PrefabDocumentSession::StageView() const noexcept {
        if (preview_.scene)
            return preview_.scene->View();
        return preview_.host && preview_.published ? preview_.host->Scenes().ActiveScene() : std::nullopt;
    }
}  // namespace Horo::Editor
