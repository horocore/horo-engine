#include "HeadlessSimulationAssets.h"

#include "Horo/AI/DecisionPlanCodec.h"
#include "Horo/AI/EnvironmentQuerySourceCodec.h"
#include "Horo/Animation/AnimationCookedAssets.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Prefab/CookedPrefab.h"

#include <set>

namespace Horo::Application::Internal {
    namespace {
        struct AssetClosure final {
            const CommittedSceneAssets &committed;
            std::set<Assets::AssetId> selected;
            std::vector<const Assets::AssetCookArtifact *> pending;

            void Add(Assets::AssetId id) {
                if (const auto *artifact = committed.Find(id); artifact && selected.insert(id).second)
                    pending.push_back(artifact);
            }

            void AddScene(const Runtime::RuntimeSceneDefinition &scene) {
                for (const auto &dependency : scene.AssetDependencies())
                    Add(dependency.id);
            }
        };

        /** @brief Selects the existing Animation codec for a retained authoritative Animation artifact. */
        std::optional<Animation::AnimationCookedKind> AnimationKind(const Assets::AssetTypeId &type) {
            using enum Animation::AnimationCookedKind;
            if (type.Value() == "core.animation.graph")
                return Graph;
            if (type.Value() == "core.animation.clip")
                return Clip;
            return std::nullopt;
        }

        /** @brief Preserves admitted templates and the same typed catalogs used by controller admission. */
        void AddCatalogs(AssetClosure &closure, const HeadlessSimulationAssets &selection) {
            for (const auto id : selection.decisions)
                closure.Add(id);
            for (const auto &artifact : closure.committed.Artifacts()) {
                const auto &type = artifact.type.Value();
                if ((selection.runtimePrefabs && type == "core.prefab") ||
                    (!selection.decisions.empty() &&
                     (type == AI::BlackboardSchemaAssetType || type == AI::EnvironmentQuerySourceAssetType)))
                    closure.Add(artifact.id);
            }
        }

        /** @brief Reads owner-defined dependency tables. Semantic validation remains with the actual loaders. */
        void AddPayloadDependencies(AssetClosure &closure, const Assets::AssetCookArtifact &artifact,
                                    const Prefab::PrefabLimitProfile &prefabLimits, const CancellationToken &cancellation) {
            if (artifact.type.Value() == "core.prefab") {
                const auto prefab = Prefab::CookedPrefab::Parse(std::as_bytes(std::span{artifact.payload}), artifact.id, prefabLimits);
                if (prefab.HasValue())
                    for (const auto &dependency : prefab.Value().Data().dependencies)
                        closure.Add(dependency.asset.id);
                return;
            }
            if (const auto animation = AnimationKind(artifact.type)) {
                const auto dependencies =
                    Animation::InspectAnimationPayloadDependencies(artifact.payload, *animation, artifact.id,
                                                                   closure.committed.Artifacts().size(), {.cancellation = cancellation});
                if (dependencies.HasValue())
                    for (const auto &dependency : dependencies.Value())
                        closure.Add(dependency.id);
            }
        }
    }  // namespace

    /** @copydoc SelectHeadlessSimulationAssets */
    Result<std::vector<Assets::AssetId>> SelectHeadlessSimulationAssets(
        const CommittedSceneAssets &committed, const Runtime::RuntimeSceneDefinition &scene, const Assets::AssetId cookedScene,
        const HeadlessSimulationAssets &selection, const Prefab::PrefabProjectPolicy &prefabPolicy, const CancellationToken &cancellation) {
        using Selected = Result<std::vector<Assets::AssetId>>;
        const auto prefabLimits = Prefab::PrefabLimitProfile::Create(prefabPolicy);
        if (prefabLimits.HasError())
            return Selected::Failure(prefabLimits.ErrorValue());
        AssetClosure closure{committed};
        closure.Add(cookedScene);
        closure.AddScene(scene);
        for (const auto &child : scene.ChildScenes()) {
            const auto definition = committed.DecodeChildScene(child);
            if (definition.HasError())
                return Selected::Failure(definition.ErrorValue());
            closure.Add(child.asset);
            closure.AddScene(definition.Value());
        }
        AddCatalogs(closure, selection);
        for (std::size_t index = 0; index < closure.pending.size(); ++index) {
            if (cancellation.IsCancellationRequested())
                return Selected::Failure(JobCancelled().ErrorValue());
            AddPayloadDependencies(closure, *closure.pending[index], prefabLimits.Value(), cancellation);
        }
        return Selected::Success({closure.selected.begin(), closure.selected.end()});
    }
}  // namespace Horo::Application::Internal
