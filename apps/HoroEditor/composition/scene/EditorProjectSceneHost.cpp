#include "EditorPlaySavePhysicsServices.h"
#include "EditorPlaySavePrefabServices.h"
#include "EditorProjectSceneHostOwner.h"
#include "Horo/Application/PrefabProjectSettings.h"
#include "Horo/Application/ProjectCollisionSettings.h"
#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Application/ProjectSurfaceSettings.h"
#include "Horo/Gameplay/GameplayAnimationScene.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Prefab/PrefabSpawnService.h"
#include "Horo/Scene/PrefabComponentProjection.h"
#include "PhysicsStateHostLifecycle.h"
#include "RuntimeUiPaintProjectSources.h"
#include "ScenePhysicsHost.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <utility>

namespace Horo::Application::Internal {
    /** @copydoc EditorProjectSceneDetail::ProjectPlaySceneHost::GameplayServices */
    Result<Gameplay::BehaviorRuntimeServiceBindings> EditorProjectSceneDetail::ProjectPlaySceneHost::GameplayServices(
        const Gameplay::BehaviorRegistry &registry) {
        using Return = Result<Gameplay::BehaviorRuntimeServiceBindings>;
        const auto registrations = registry.Registrations();
        if (!animation_ || registrations.empty())
            return Return::Success({});
        const auto scene = scenes_->ActiveScene();
        if (!registry.IsFrozen() || !scene || sceneClosed_)
            return Return::Failure(MakeError(InvalidProjectScene));
        try {
            Gameplay::BehaviorRuntimeServiceBindings bindings;
            bindings.animationLifetime.emplace(animationLifetime_.Token());
            bindings.animationBehaviors.reserve(registrations.size());
            for (const auto &registration : registrations) {
                const auto &type = registration.descriptor.typeId;
                auto grant = Gameplay::CreateGameplayAnimationContext(animation_, scene->RuntimeId(), std::string{type.Value()},
                                                                      bindings.animationLifetime->Token());
                if (grant.HasError())
                    return Return::Failure(grant.ErrorValue());
                bindings.animationBehaviors.push_back({type, std::move(grant).Value()});
            }
            return Return::Success(std::move(bindings));
        } catch (const std::bad_alloc &) {
            return Return::Failure(MakeError(InvalidProjectScene, "Animation grant capacity exhausted while activating Play."));
        }
    }

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

    Result<Editor::EnvironmentQueryPreviewPhysics::Visibility> EditorProjectSceneDetail::ProjectPlaySceneHost::CapturePreviewVisibility(
        const Runtime::RuntimeSceneView &scene, const AI::QueryPhysicsVisibilityRequest &request) {
        using Visibility = Result<Editor::EnvironmentQueryPreviewPhysics::Visibility>;
        auto scope = native_.Borrow();
        if (scope.HasError())
            return Visibility::Failure(scope.ErrorValue());
        const auto &binding = scope.Value();
        const auto querier = scene.Get(request.querier);
        if (!scene.IsCurrent() || binding.scene != scene.RuntimeId() || querier.HasError())
            return Visibility::Failure(MakeError(Physics::PhysicsErrors::QuerySnapshotStale));
        const auto projected = AI::ProjectQueryPhysicsVisibilityRays(request, {});
        if (projected.HasError())
            return Visibility::Failure(projected.ErrorValue());

        Physics::PhysicsQueryFilter filter{.channel = request.channel, .blockingOnly = true};
        const auto body = std::ranges::find(binding.bodyBindings, querier.Value().authoredObject.value_or(Runtime::SceneObjectId{}),
                                            &Physics::PhysicsSceneBodyBinding::object);
        if (body != binding.bodyBindings.end())
            filter.excludedBody = body->handle;
        const auto &rays = projected.Value();
        Editor::EnvironmentQueryPreviewPhysics::Visibility clear{};
        std::fill_n(clear.begin(), request.points.size(), true);
        for (std::size_t index = 0; index < rays.count; ++index) {
            const Physics::PhysicsQueryDescriptor descriptor{.world = binding.physics.Identity(),
                                                             .sceneGeneration = binding.scene.value,
                                                             .geometry = rays.rays[index],
                                                             .filter = filter,
                                                             .collection = Physics::PhysicsQueryCollection::Closest,
                                                             .maximumHitCount = 1};
            std::array<Physics::PhysicsQueryHit, 1> hits;
            const auto result = binding.physics.Query(descriptor, hits);
            if (result.HasError())
                return Visibility::Failure(result.ErrorValue());
            clear[rays.indices[index]] = result.Value().hitCount == 0;
        }
        return Visibility::Success(clear);
    }

    /** @copydoc PrepareEditorProjectPreview */
    Result<EditorProjectPreviewScene> PrepareEditorProjectPreview(const std::filesystem::path &root, JobSystem &jobs,
                                                                  const EditorProjectScenePolicy &policy) {
        auto owner = std::make_unique<ProjectPlaySceneHost>(Runtime::SceneRuntimeId{1},
                                                            ScenePhysicsIdentityRange{{1}, {Editor::EditorPrefabStageNamespace - 1}});
        if (auto prepared = owner->Prepare(root, jobs, policy, nullptr); prepared.HasError())
            return Result<EditorProjectPreviewScene>::Failure(prepared.ErrorValue());
        const auto mode = owner->PhysicsMode();
        auto scenes = owner->ReleaseScenes();
        Editor::EnvironmentQueryPreviewPhysics queryPreview{
            [native = owner.get()](const Runtime::RuntimeSceneView &scene, const AI::QueryPhysicsVisibilityRequest &request) {
            return native->CapturePreviewVisibility(scene, request);
        }};
        return Result<EditorProjectPreviewScene>::Success({std::move(owner), std::move(scenes), mode, std::move(queryPreview)});
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
                                       WallClock &wallClock, std::optional<EditorPlaySaveSupplement> supplement) {
        if (policy.physicsMode == Physics::PhysicsRuntimeMode::Canonical && !policy.physicsSettings) {
            auto settings = ProductPhysicsSaveWorldSettings();
            if (settings.HasError()) {
                policy.saveError = settings.ErrorValue();
                return;
            }
            policy.physicsSettings.emplace(std::move(settings).Value());
        }
        auto factory = MakeNativeEditorPlaySaveFactory(jobs, operations, clock, wallClock, std::move(supplement));
        if (factory.HasError()) {
            policy.saveError = factory.ErrorValue();
            return;
        }
        policy.saveFactory = std::move(factory).Value();
    }
}  // namespace Horo::Application::Internal
