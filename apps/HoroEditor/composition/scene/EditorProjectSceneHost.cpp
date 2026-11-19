#include "EditorPlaySavePhysicsServices.h"
#include "EditorPlaySavePrefabServices.h"
#include "EditorProjectSceneHostOwner.h"
#include "Horo/Application/PrefabProjectSettings.h"
#include "Horo/Application/ProjectCollisionSettings.h"
#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Application/ProjectSurfaceSettings.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Prefab/PrefabSpawnService.h"
#include "Horo/Scene/PrefabComponentProjection.h"
#include "PhysicsStateHostLifecycle.h"
#include "RuntimeUiPaintProjectSources.h"
#include "ScenePhysicsHost.h"

#include <cmath>
#include <exception>
#include <limits>
#include <utility>

namespace Horo::Application::Internal {
    namespace {
        using EditorProjectSceneDetail::InvalidProjectScene;
        using EditorProjectSceneDetail::ProjectPlaySceneHost;

        /** @brief Composes identical project resources under an explicitly disjoint host identity grant. */
        Editor::EditorProjectPlaySceneFactory ProjectFactory(JobSystem &jobs, EditorProjectScenePolicy policy,
                                                             const ScenePhysicsIdentityRange range) {
            return [&jobs, policy = std::move(policy), range](const std::filesystem::path &root, const Runtime::SceneRuntimeId identity,
                                                              const Runtime::RuntimeSceneDefinition &definition) {
                if (identity.value < range.first.value || identity.value > range.last.value)
                    return Result<std::unique_ptr<Editor::EditorPlaySceneHost>>::Failure(MakeError(InvalidProjectScene));
                auto host = std::make_unique<ProjectPlaySceneHost>(identity, range);
                if (auto prepared = host->Prepare(root, jobs, policy, &definition); prepared.HasError())
                    return Result<std::unique_ptr<Editor::EditorPlaySceneHost>>::Failure(prepared.ErrorValue());
                return Result<std::unique_ptr<Editor::EditorPlaySceneHost>>::Success(std::move(host));
            };
        }
    }  // namespace

    /** @copydoc PrepareEditorProjectPreview */
    Result<EditorProjectPreviewScene> PrepareEditorProjectPreview(const std::filesystem::path &root, JobSystem &jobs,
                                                                  const EditorProjectScenePolicy &policy) {
        auto owner = std::make_unique<ProjectPlaySceneHost>(Runtime::SceneRuntimeId{1},
                                                            ScenePhysicsIdentityRange{{1}, {Editor::EditorPrefabStageNamespace - 1}});
        if (auto prepared = owner->Prepare(root, jobs, policy, nullptr); prepared.HasError())
            return Result<EditorProjectPreviewScene>::Failure(prepared.ErrorValue());
        const auto mode = owner->PhysicsMode();
        auto scenes = owner->ReleaseScenes();
        return Result<EditorProjectPreviewScene>::Success({std::move(owner), std::move(scenes), mode});
    }

    /** @copydoc ValidateEditorPreviewProjectScope */
    Result<void> ValidateEditorPreviewProjectScope(const std::filesystem::path &startupRoot, const std::filesystem::path &incomingRoot,
                                                   const Physics::PhysicsRuntimeMode mode) {
        if (mode == Physics::PhysicsRuntimeMode::Null)
            return Result<void>::Success();
        std::error_code error;
        if (const bool same = std::filesystem::equivalent(startupRoot, incomingRoot, error); error || !same)
            return Result<void>::Failure(MakeError(InvalidProjectScene, "Canonical Physics preview owns its startup project's captures. "
                                                                        "Restart the editor with --project for the selected project."));
        return Result<void>::Success();
    }

    /** @copydoc ObserveEditorPlayMountedWorld */
    std::optional<WorldStreaming::OriginFrameLease> ObserveEditorPlayMountedWorld(const Editor::EditorPlaySceneHost &host) {
        const auto *selected = dynamic_cast<const ProjectPlaySceneHost *>(&host);
        return selected ? selected->WorldOrigin() : std::nullopt;
    }

    /** @copydoc MakeEditorProjectSceneFactory */
    Editor::EditorProjectPlaySceneFactory MakeEditorProjectSceneFactory(JobSystem &jobs, EditorProjectScenePolicy policy) {
        return ProjectFactory(jobs, std::move(policy), {Editor::EditorFirstPlayScene, {std::numeric_limits<std::uint64_t>::max()}});
    }

    /** @copydoc MakeEditorProjectPrefabStageFactory */
    Editor::EditorProjectPrefabStageFactory MakeEditorProjectPrefabStageFactory(JobSystem &jobs, EditorProjectScenePolicy policy) {
        policy.world.reset();
        policy.decisions.reset();
        return {ProjectFactory(jobs, std::move(policy), {Editor::EditorFirstPrefabStage, Editor::EditorLastPrefabStage})};
    }

    /** @copydoc ConfigureEditorPlaySavePolicy */
    void ConfigureEditorPlaySavePolicy(EditorProjectScenePolicy &policy, JobSystem &jobs, OperationStore &operations, Clock &clock,
                                       WallClock &wallClock) {
        if (policy.physicsMode == Physics::PhysicsRuntimeMode::Canonical && !policy.physicsSettings) {
            auto settings = ProductPhysicsSaveWorldSettings();
            if (settings.HasError()) {
                policy.saveError = settings.ErrorValue();
                return;
            }
            policy.physicsSettings.emplace(std::move(settings).Value());
        }
        auto factory = MakeNativeEditorPlaySaveFactory(jobs, operations, clock, wallClock);
        if (factory.HasError()) {
            policy.saveError = factory.ErrorValue();
            return;
        }
        policy.saveFactory = std::move(factory).Value();
    }
}  // namespace Horo::Application::Internal
