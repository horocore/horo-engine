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
#include "Horo/WorldStreaming/WorldArtifactErrors.h"
#include "PhysicsStateHostLifecycle.h"
#include "RuntimeUiPaintProjectSources.h"
#include "ScenePhysicsHost.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <limits>
#include <utility>
#include <vector>

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

    namespace {
        /** @brief Measures the strongest positive contact between already admitted native body handles. */
        Result<std::optional<Editor::InspectionPenetrationSample>> MeasurePreviewPenetration(
            const Physics::Detail::PhysicsSceneHostScope &binding, Physics::BodyHandle source, Physics::BodyHandle target,
            Physics::PhysicsQueryChannelId channel) {
            using Output = Result<std::optional<Editor::InspectionPenetrationSample>>;
            const auto body = binding.physics.ReadSceneBodyReconciliation(source);
            if (body.HasError())
                return Output::Failure(body.ErrorValue());
            const auto &state = body.Value();
            const Physics::PhysicsQueryDescriptor descriptor{.world = binding.physics.Identity(),
                                                             .sceneGeneration = binding.scene.value,
                                                             .geometry =
                                                                 Physics::PhysicsOverlapQuery{state.observedShape, state.state.pose},
                                                             .filter = {.channel = channel, .excludedBody = source},
                                                             .collection = Physics::PhysicsQueryCollection::All,
                                                             .maximumHitCount = Physics::MaximumPhysicsQueryHits};
            std::vector<Physics::PhysicsQueryHit> hits(descriptor.maximumHitCount);
            const auto result = binding.physics.Query(descriptor, hits);
            if (result.HasError())
                return Output::Failure(result.ErrorValue());
            if (result.Value().truncated)
                return Output::Failure(MakeError(Physics::PhysicsErrors::CapacityExceeded));
            std::optional<Editor::InspectionPenetrationSample> strongest;
            for (const auto &hit : std::span{hits}.first(result.Value().hitCount)) {
                const auto depth = hit.penetrationDepthMeters;
                if (hit.body == target && depth > (strongest ? strongest->depthMeters : 0.0F))
                    strongest = Editor::InspectionPenetrationSample{depth, {hit.position.x, hit.position.y, hit.position.z}};
            }
            return Output::Success(strongest);
        }
    }  // namespace

    /** @copydoc EditorProjectSceneDetail::ProjectPlaySceneHost::CapturePreviewPenetration */
    Result<std::optional<Editor::InspectionPenetrationSample>> EditorProjectSceneDetail::ProjectPlaySceneHost::CapturePreviewPenetration(
        const Runtime::RuntimeSceneView &scene, const Runtime::PhysicsBodyReference &source, const Runtime::PhysicsBodyReference &target,
        const std::string_view channelId) {
        using Output = Result<std::optional<Editor::InspectionPenetrationSample>>;
        const auto channel = Physics::PhysicsQueryChannelId::Parse(channelId);
        if (channel.HasError())
            return Output::Failure(channel.ErrorValue());
        auto scope = native_.Borrow();
        if (scope.HasError())
            return Output::Failure(scope.ErrorValue());
        const auto &binding = scope.Value();
        if (!scene.IsCurrent() || binding.scene != scene.RuntimeId())
            return Output::Failure(MakeError(Physics::PhysicsErrors::QuerySnapshotStale));
        const auto findBody = [&binding](const Runtime::PhysicsBodyReference &ref) {
            return std::ranges::find_if(binding.bodyBindings, [&ref](const Physics::PhysicsSceneBodyBinding &body) {
                return body.object == ref.object && body.body == ref.body;
            });
        };
        const auto from = findBody(source);
        const auto to = findBody(target);
        if (from == binding.bodyBindings.end() || to == binding.bodyBindings.end() || from == to)
            return Output::Failure(MakeError(Physics::PhysicsErrors::HandleStale));
        return MeasurePreviewPenetration(binding, from->handle, to->handle, channel.Value());
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
        Editor::EnvironmentQueryPreviewNavigation navigationPreview{
            [native = owner.get(), root, cook = policy.cooked, &jobs](const Editor::EnvironmentQueryPreviewNavigationRequest &request) {
            return native->CapturePreviewNavigation(root, cook, jobs, request);
        }};
        Editor::EditorInspectionPhysics inspection{
            [native = owner.get()](const Runtime::RuntimeSceneView &scene, const Runtime::PhysicsBodyReference &source,
                                   const Runtime::PhysicsBodyReference &target, std::string_view channel) {
            return native->CapturePreviewPenetration(scene, source, target, channel);
        }};
        return Result<EditorProjectPreviewScene>::Success(
            {std::move(owner), std::move(scenes), mode, std::move(queryPreview), std::move(navigationPreview), std::move(inspection)});
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

    /** @copydoc InspectEditorPlayMountedCell */
    Result<MountedWorldCellReadiness> InspectEditorPlayMountedCell(const Editor::EditorPlaySceneHost &host,
                                                                   Runtime::SceneInstanceId occurrence) {
        const auto *selected = dynamic_cast<const ProjectPlaySceneHost *>(&host);
        if (!selected)
            return Result<MountedWorldCellReadiness>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Stale));
        return selected->InspectWorldCell(occurrence);
    }

    /** @copydoc QueueEditorPlayNavigationRebuild */
    Result<void> QueueEditorPlayNavigationRebuild(Editor::EditorPlaySceneHost &host, ProjectSceneNavigationRebuildCommand command) {
        auto *selected = dynamic_cast<ProjectPlaySceneHost *>(&host);
        if (!selected)
            return Result<void>::Failure(MakeError(Navigation::NavigationErrors::CapabilityUnavailable));
        return selected->QueueNavigationRebuild(std::move(command));
    }

    /** @copydoc InspectEditorPlayNavigationRebuildError */
    std::optional<Error> InspectEditorPlayNavigationRebuildError(const Editor::EditorPlaySceneHost &host) {
        const auto *selected = dynamic_cast<const ProjectPlaySceneHost *>(&host);
        return selected ? selected->NavigationRebuildError() : std::nullopt;
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
