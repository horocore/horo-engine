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
    namespace {
        /** @brief Reads one complete published tick using a short, owner-lane Physics capability. */
        std::optional<std::size_t> ReadAgentContactRecords(Physics::PhysicsWorld &world, const Physics::PhysicsPublishedTick published,
                                                           const std::span<const Physics::BodyHandle> bodies,
                                                           const std::span<Physics::PhysicsEventRecord> storage) {
            const auto limit = std::min(storage.size(), static_cast<std::size_t>(world.Settings().Values().budgets.maximumEvents));
            if (limit == 0 || published.eventTick == 0 || published.eventTick != published.completedTick)
                return std::nullopt;
            const auto access = world.IssueQueryEventCapability();
            if (access.HasError())
                return std::nullopt;
            const auto &capability = access.Value();
            const Physics::PhysicsEventReadCommand command{capability.Identity(), published.eventTick, published.publicationRevision,
                                                           static_cast<std::uint32_t>(limit)};
            const auto events = bodies.empty() ? capability.ReadEvents(command, storage.first(limit))
                                               : capability.ReadEventsForBodies(command, bodies, storage.first(limit));
            const auto revoked = world.RevokeQueryEventCapability(capability);
            if (revoked.HasError() || events.HasError() || events.Value().truncated || events.Value().droppedRecordCount)
                return std::nullopt;
            return events.Value().recordCount;
        }

        /** @brief Collects exact selected handles. An empty filter falls back to a complete tick when the filter is too small. */
        std::optional<std::span<const Physics::BodyHandle>> SelectContactBodyFilter(
            const std::span<const Physics::PhysicsSceneBodyBinding> bindings, const Runtime::SceneObjectId object,
            const std::span<Physics::BodyHandle> storage) {
            std::size_t count{};
            for (const auto &binding : bindings) {
                if (binding.object != object)
                    continue;
                if (count == storage.size())
                    return std::span<const Physics::BodyHandle>{};
                storage[count++] = binding.handle;
            }
            if (count == 0)
                return std::nullopt;
            return storage.first(count);
        }

        /** @brief Resolves a physical body only through the published Scene's exact binding. */
        Runtime::SceneObjectId ReadContactObject(const std::span<const Physics::PhysicsSceneBodyBinding> bindings,
                                                 const Physics::BodyHandle handle) {
            const auto found = std::ranges::find(bindings, handle, &Physics::PhysicsSceneBodyBinding::handle);
            return found == bindings.end() ? Runtime::SceneObjectId{} : found->object;
        }

        /** @brief Projects incident transitions and discards continuous solver updates. */
        std::optional<Editor::EditorPlayContactSnapshot::Kind> ContactTransition(const Physics::PhysicsEventKind kind) noexcept {
            using enum Physics::PhysicsEventKind;
            using Kind = Editor::EditorPlayContactSnapshot::Kind;
            switch (kind) {
                case ContactBegin:
                    return Kind::ContactBegin;
                case ContactEnd:
                    return Kind::ContactEnd;
                case TriggerEnter:
                    return Kind::TriggerEnter;
                case TriggerExit:
                    return Kind::TriggerExit;
                default:
                    return std::nullopt;
            }
        }

        /** @brief Adds one incident transition without translating it into a gameplay action. */
        bool AppendAgentContact(Editor::EditorPlayContactSnapshot &review, const Runtime::RuntimeSceneView &scene,
                                const Runtime::SceneObjectId anchor, const std::span<const Physics::PhysicsSceneBodyBinding> bindings,
                                const Physics::PhysicsEventRecord &record) {
            const auto kind = ContactTransition(record.kind);
            if (!kind)
                return true;
            const auto first = ReadContactObject(bindings, record.pair.first.body);
            const auto second = ReadContactObject(bindings, record.pair.second.body);
            if (first != anchor && second != anchor)
                return true;
            const auto peer = first == anchor ? second : first;
            if (!scene.Find(peer))
                return false;
            const Editor::EditorPlayContactSnapshot::Contact contact{peer, *kind};
            const auto observed = std::span{review.contacts}.first(review.count);
            if (std::ranges::find(observed, contact) != observed.end())
                return true;
            if (review.count == review.contacts.size())
                return false;
            review.contacts[review.count++] = contact;
            return true;
        }
    }  // namespace

    /** @copydoc EditorProjectSceneDetail::ProjectPlaySceneHost::ReadAgentSceneContacts */
    std::optional<Editor::EditorPlayContactSnapshot> EditorProjectSceneDetail::ProjectPlaySceneHost::ReadAgentSceneContacts(
        const Runtime::EntityRef object) {
        const auto scope = native_.Borrow();
        if (scope.HasError() || scope.Value().scene != object.runtime)
            return std::nullopt;
        const auto scene = scenes_->ActiveScene();
        if (!scene)
            return std::nullopt;
        const auto anchor = scene->Get(object);
        if (anchor.HasError() || !anchor.Value().authoredObject)
            return std::nullopt;
        const auto anchorId = *anchor.Value().authoredObject;
        const auto bindings = scope.Value().bodyBindings;
        std::array<Physics::BodyHandle, Physics::PhysicsQueryEventCapability::MaximumEventBodyFilter> bodies;
        const auto filter = SelectContactBodyFilter(bindings, anchorId, bodies);
        if (!filter)
            return std::nullopt;

        auto &world = scope.Value().physics;
        const auto tick = world.PublishedTick();
        std::array<Physics::PhysicsEventRecord, 32> records;
        const auto copied = ReadAgentContactRecords(world, tick, *filter, records);
        if (!copied)
            return std::nullopt;
        Editor::EditorPlayContactSnapshot review{.anchor = object,
                                                 .structuralRevision = scene->StructuralRevision(),
                                                 .completedTick = tick.eventTick};
        for (const auto &record : std::span{records}.first(*copied))
            if (!AppendAgentContact(review, *scene, anchorId, bindings, record))
                return std::nullopt;
        return review;
    }

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
                if (auto prepared = host->PrepareNpcDialogue(root, jobs, policy); prepared.HasError())
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
        policy.npcDialogue.reset();
        policy.authoredNpcDialogue.reset();
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
