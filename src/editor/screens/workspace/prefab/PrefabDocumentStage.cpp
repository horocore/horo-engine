#include "Horo/Prefab/PrefabErrors.h"
#include "Horo/Prefab/PrefabSceneExpansion.h"
#include "Horo/Scene/PrefabComponentProjection.h"
#include "editor/screens/workspace/prefab/PrefabDocumentSession.h"

#include <limits>

namespace Horo::Editor {
    /** @copydoc PrefabDocumentSession::ActivateStage */
    Result<void> PrefabDocumentSession::ActivateStage(const Assets::AssetRegistrySnapshot &registry,
                                                      const Prefab::PrefabSourceResolverSnapshot &dependencies,
                                                      const Runtime::SceneRuntimeId runtimeId) {
        if (auto admitted = Admit({owner_, identity_.instance, revision_}); admitted.HasError())
            return admitted;
        if (previewRevision_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(Prefab::PrefabErrors::WorkBudgetExceeded));
        // The canonical resolver owns composition. Only the active root is replaced by authored unsaved intent.
        std::vector<Prefab::PrefabDependencySource> sources;
        for (const auto &dependency : dependencies.Sources()) {
            if (dependency.document.Data().assetId != source_->Data().assetId)
                sources.push_back(dependency);
        }
        sources.push_back({*source_, {source_->Data().projectVersion, state_}});
        auto resolver = Prefab::BuildPrefabSourceResolverSnapshot(registry, std::move(sources), limits_);
        if (resolver.HasError())
            return Result<void>::Failure(resolver.ErrorValue());
        auto candidate = resolver.Value().Resolve(source_->Data().assetId,
                                                  Prefab::PrefabInstanceId::Create(identity_.instance.Value()).Value(), limits_);
        if (candidate.HasError())
            return Result<void>::Failure(candidate.ErrorValue());
        auto identities = Prefab::RemapPrefabCandidateToScene(candidate.Value(), {}, {}, limits_);
        if (identities.HasError())
            return Result<void>::Failure(identities.ErrorValue());
        std::vector<Prefab::PrefabRuntimeComponentProjection> components;
        for (const auto &object : candidate.Value().Objects()) {
            Prefab::PrefabRuntimeComponentProjection projection{.object = object.key};
            for (const auto &component : object.object.components) {
                auto projected = SceneSource::ProjectPrefabBuiltinComponent(component, projection.components, projection.primitiveMesh);
                if (projected.HasError())
                    return Result<void>::Failure(projected.ErrorValue());
                // Unknown provider bytes remain in source; never claim a partial preview as complete.
                if (!projected.Value())
                    return Result<void>::Failure(MakeError(Prefab::PrefabErrors::DependencyUnavailable));
            }
            projection.components.behaviors = object.object.behaviors;
            components.push_back(std::move(projection));
        }
        auto subtree = Prefab::ExpandPrefabSceneSubtree(candidate.Value(), identities.Value(), components, {}, limits_);
        if (subtree.HasError())
            return Result<void>::Failure(subtree.ErrorValue());
        Runtime::SceneDefinitionBuilder builder({identity_.instance.Value()}, {previewRevision_ + 1});
        for (const auto &entity : subtree.Value().Entities())
            builder.Add(entity);
        for (const auto &dependency : candidate.Value().Revision().dependencies) {
            if (dependency.assetType.Value() == "core.prefab")
                continue;
            if (auto required = builder.RequireAsset({dependency.assetId, dependency.assetType}); required.HasError())
                return Result<void>::Failure(required.ErrorValue());
        }
        auto definition = std::move(builder).Build();
        if (definition.HasError())
            return Result<void>::Failure(definition.ErrorValue());
        auto runtime = Runtime::RuntimeScene::Create(definition.Value(), runtimeId);
        if (runtime.HasError())
            return Result<void>::Failure(runtime.ErrorValue());
        stage_ = std::move(runtime).Value();
        ++previewRevision_;
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
