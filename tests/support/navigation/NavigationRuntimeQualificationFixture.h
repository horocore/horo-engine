#pragma once

/** @file NavigationRuntimeQualificationFixture.h
 * @brief Test-host composition of real Detour query/crowd providers and canonical Physics-backed Character movement.
 */
#include "AllocationProbe.h"
#include "Horo/Navigation/Backends/RecastDetourCrowdProvider.h"
#include "Horo/Navigation/Backends/RecastDetourProvider.h"
#include "Horo/Navigation/NavigationCharacterAdapter.h"
#include "Horo/Physics/CharacterWorld.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "navigation/NavigationRuntimeTestFixtures.h"
#include "navigation/NavigationTestAssertions.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

namespace Horo::Navigation::Qualification {
    using namespace TestSupport;
    inline constexpr float RootHeight = 0.77F;
    inline constexpr float StepSeconds = 1.0F / 60.0F;

    /** @brief One finite workload slice below ADR-109 CrowdSmall capacities, independent of renderer selection. */
    inline NavigationPathBatchLimits QueryLimits() {
        return {.requestSlots = 64,
                .maximumJobs = 4,
                .requestsPerJob = 4,
                .requestsPerTick = 8,
                .requestsPerCaller = 2,
                .maximumPendingPerCaller = 16,
                .nodeExpansionsPerTick = 512,
                .nodeExpansionsPerCaller = 128,
                .selectionProbesPerTick = 4096,
                .priorityAgingTicks = 4};
    }

    /** @brief Build actual Detour polygons; disconnected mode leaves a deliberate physical gap in the path topology. */
    inline std::unique_ptr<INavigationQueryBackend> Provider(const NavigationGeneration topology = Topology(),
                                                             const bool disconnected = false, const NavigationWorldId worldId = World()) {
        const std::array<Math::Vec3, 8> vertices{
            {{0, 0, 0}, {5, 0, 0}, {5, 0, 10}, {0, 0, 10}, {6, 0, 0}, {10, 0, 0}, {10, 0, 10}, {6, 0, 10}}};
        const auto area = Id<NavigationAreaId>(1);
        const auto surface = Id<SurfaceId>(1);
        std::array<GroundedNavigationPolygon, 2> polygons{
            {{.vertexIndices = {0, 1, 2, 3, 0, 0}, .vertexCount = 4, .area = area, .surface = surface},
             {.vertexIndices = {4, 5, 6, 7, 0, 0}, .vertexCount = 4, .area = area, .surface = surface}}};
        // A connected corpus uses one convex floor, avoiding an invented adapter or baked fixture implementation.
        if (!disconnected)
            polygons[0].vertexIndices = {0, 5, 6, 3, 0, 0};
        const NavigationDescriptorSource source{NavigationDescriptorSourceKind::Project, Id<NavigationDescriptorSourceId>(1)};
        const std::array areas{NavigationAreaDescriptor{.id = area, .source = source, .traversalCost = 1, .flags = {1}}};
        const std::array filters{NavigationQueryFilterDescriptor{.id = Id<NavigationFilterId>(1), .source = source}};
        auto created = CreateRecastDetourNavigationQueryBackend({.world = worldId,
                                                                 .topology = topology,
                                                                 .vertices = vertices,
                                                                 .polygons = std::span(polygons).first(disconnected ? 2 : 1),
                                                                 .maximumQueryNodes = 64,
                                                                 .maximumResultPoints = 16,
                                                                 .maximumConcurrentQueries = 4,
                                                                 .areas = areas,
                                                                 .filters = filters});
        REQUIRE(created.HasValue());
        return std::move(created).Value();
    }

    /** @brief Canonical world settings with bounded scratch and enough fixture geometry for this corpus. */
    inline Physics::PhysicsWorldSettings PhysicsSettings() {
        Physics::PhysicsWorldSettingsDescriptor settings;
        settings.world.capacity = {16, 32, 16, 4096};
        settings.budgets.maximumContactPairs = 32;
        settings.budgets.maximumContactConstraints = 16;
        settings.budgets.maximumInFlightPairs = 8;
        settings.budgets.scratchBytes = 1024 * 1024;
        return Physics::PhysicsWorldSettings::Capture(settings).Value();
    }

    /** @brief Test host owns services in dependency order and drains jobs before provider/world destruction. */
    struct Host final {
        JobSystem jobs{{.workerCount = 1}};
        NavigationWorldLifecycle navigation{NavigationWorldLifecycle::Create(4).Value()};
        std::unique_ptr<NavigationCoordinator> coordinator{
            std::make_unique<NavigationCoordinator>(NavigationCoordinator::Create(jobs, QueryLimits()).Value())};
        NavigationAgentRegistry agents{NavigationAgentRegistry::Create({.maximumAgents = 4}).Value()};
        NavigationDynamicRegistry dynamic{NavigationDynamicRegistry::Create().Value()};
        NavigationWorldActivationDescriptor activation{Activation()};
        NavigationAgentSceneBinding binding{World(), activation.scene, activation.sceneGeneration};
        std::array<NavigationPathCaller, 1> callers{{{Id<NavigationDynamicOwnerId>(1), Id<NavigationDynamicOwnerGeneration>(1)}}};
        std::unique_ptr<Physics::PhysicsRuntime> physicsRuntime;
        std::unique_ptr<Physics::PhysicsWorld> physics;
        std::unique_ptr<Character::CharacterWorld> character;
        Character::CharacterControllerHandle controller;
        std::unique_ptr<NavigationAgentSceneCandidate> scene;
        CrowdAgentHandle agent;
        std::unique_ptr<NavigationMovementController> movement;
        std::unique_ptr<NavigationVelocityPipeline> velocity;
        std::unique_ptr<INavigationCrowdBackend> crowd;
        Character::CharacterCapability grant;
        NavigationCharacterAdapter bridge;
        std::uint32_t dispatched{};
        std::uint64_t lastTick{};
        std::uint64_t physicsTick{};
        std::uint64_t characterRevision{1};
        bool closed{};
        Tests::AllocationProbe::Measurement captureAllocations{};
        Tests::AllocationProbe::Measurement commitAllocations{};
        std::chrono::nanoseconds commitLatency{};

        explicit Host(const bool disconnected = false, const bool wall = false) {
            REQUIRE(navigation.Stage(activation, Provider(activation.topology, disconnected, activation.world)).HasValue());
            REQUIRE(navigation.CommitAtSafePoint(activation.scene, activation.sceneGeneration).HasValue());
            physicsRuntime = Physics::PhysicsRuntime::Create(Physics::PhysicsRuntimeMode::Canonical).Value();
            physics = physicsRuntime->PrepareWorld(PhysicsSettings()).Value();
            REQUIRE(physics->Activate(physicsRuntime->IssueWorldIdentity().Value()).HasValue());
            PrepareCharacter();
            AddShape(Physics::PhysicsStaticPlaneShape{{0, 1, 0}, 0}, {});
            if (wall)
                AddShape(Physics::PhysicsBoxShape{{0.05F, 2, 4}}, {4, 1, 5});
            AdvancePhysics();
            ActivateOwners(false);
        }

        void PrepareCharacter() {
            Character::CharacterWorldSettingsDescriptor limits;
            limits.capacities.maximumControllers = 1;
            limits.capacities.maximumQueuedCommands = 4;
            limits.work.maximumCommandsPerTick = 4;
            character = Character::CharacterWorld::Prepare({activation.sceneGeneration.Value(), physics->Identity(), 1, 1},
                                                           Character::CharacterWorldSettings::Capture(limits).Value())
                            .Value();
        }

        void AdvancePhysics() {
            REQUIRE(physics
                        ->AdvanceFixedTick({.simulationTick = ++physicsTick,
                                            .sceneGeneration = activation.sceneGeneration.Value(),
                                            .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                        .HasValue());
        }

        void ActivateOwners(const bool replacement) {
            REQUIRE(character->RefreshPhysicsSnapshot(physics->Identity(), physics->PublishedTick().publicationRevision).HasValue());
            controller = character->CreateController(Controller()).Value();
            REQUIRE(character->Activate().HasValue());
            Character::CharacterPhysicsQueryAdapter queries(*physics);
            REQUIRE(character->SpawnController(controller, queries.Context(Expectations(0))).HasValue());
            const std::array descriptors{NavigationAgentDescriptor{.owner = {binding.scene, 1, 1},
                                                                   .profile = Id<NavigationAgentProfileId>(1),
                                                                   .filter = Id<NavigationFilterId>(1),
                                                                   .radiusOverride = 0.25F}};
            scene = agents.PrepareScene(binding, descriptors).Value();
            scene->Publish();
            agent = agents.Snapshot().Value().Agents().front().handle;
            if (replacement)
                REQUIRE(dynamic.ReplaceSceneAtSafePoint(activation, 1).HasValue());
            else
                REQUIRE(dynamic.CommitAtSafePoint(activation, 1).HasValue());
            movement = NavigationMovementController::Create(agents, navigation, *coordinator, agent, callers[0]).Value();
            velocity = NavigationVelocityPipeline::Create(*movement, binding, agent, characterRevision).Value();
            crowd = CreateRecastDetourCrowdBackend().Value();
            grant = character->IssueCapability().Value();
            bridge = NavigationCharacterAdapter::Create(*velocity, grant,
                                                        {{1, 0, CharacterInput::DesiredMotionSource::ExternalIntent, true},
                                                         controller,
                                                         binding,
                                                         characterRevision})
                         .Value();
        }

        /** @brief Replace actual Scene-generation owners while old native jobs retain only revoked immutable leases. */
        std::unique_ptr<NavigationCoordinator> Reload() {
            bridge.Shutdown();
            velocity->Retire();
            movement->BeginShutdown();
            movement->CollectRetiredAtSafePoint();
            scene->Shutdown();
            character->Shutdown();
            velocity.reset();
            movement.reset();
            scene.reset();
            character.reset();
            auto retiredCoordinator = std::move(coordinator);
            retiredCoordinator->BeginShutdown();
            coordinator = std::make_unique<NavigationCoordinator>(NavigationCoordinator::Create(jobs, QueryLimits()).Value());
            activation.world = World(activation.world.Value() + 1);
            activation.sceneGeneration = Id<NavigationSceneGeneration>(activation.sceneGeneration.Value() + 1);
            activation.topology = Topology(activation.topology.Value() + 1);
            binding = {activation.world, activation.scene, activation.sceneGeneration};
            callers[0].generation = Id<NavigationDynamicOwnerGeneration>(callers[0].generation.Value() + 1);
            ++characterRevision;
            REQUIRE(navigation.Stage(activation, Provider(activation.topology, false, activation.world)).HasValue());
            REQUIRE(navigation.CommitAtSafePoint(activation.scene, activation.sceneGeneration).HasValue());
            PrepareCharacter();
            AdvancePhysics();
            ActivateOwners(true);
            lastTick = 0;
            dispatched = 0;
            return retiredCoordinator;
        }

        ~Host() {
            Close();
        }

        Host(const Host &) = delete;
        Host &operator=(const Host &) = delete;

        Character::CharacterControllerDescriptor Controller() const {
            Character::CharacterControllerDescriptor descriptor;
            descriptor.sceneGeneration = activation.sceneGeneration.Value();
            descriptor.characterWorld = character->Descriptor().identity;
            descriptor.physicsWorld = physics->Identity();
            descriptor.collisionProfile = Physics::CollisionProfileId::Parse("22345678-1234-4234-8234-123456789abc").Value();
            descriptor.queryChannel = Physics::PhysicsQueryChannelId::Parse("32345678-1234-4234-8234-123456789abc").Value();
            descriptor.defaultMaterial = {Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc").Value(), 3,
                                          Physics::PhysicsMaterialSlotId::FromValue(5)};
            descriptor.capsule = {0.25F, 0.5F};
            descriptor.collisionRootPosition = {1, RootHeight, 5};
            descriptor.gravity = {};
            return descriptor;
        }

        void AddShape(const Physics::PhysicsQueryFixtureShape &shape, const Math::Vec3 position) {
            const auto descriptor = Controller();
            REQUIRE(physics
                        ->CreateQueryFixture({.shape = shape,
                                              .pose = {.translation = position},
                                              .layer = Physics::CollisionLayerId::Parse("42345678-1234-4234-8234-123456789abc").Value(),
                                              .profile = descriptor.collisionProfile,
                                              .channel = descriptor.queryChannel})
                        .HasValue());
        }

        Character::CharacterPhysicsQueryExpectations Expectations(const std::uint64_t tick) const {
            const auto &d = character->Descriptor();
            return {d.sceneGeneration,  d.identity, d.physicsWorld,           d.collisionFilterGeneration,
                    d.originGeneration, tick,       d.physicsSnapshotRevision};
        }

        NavigationOutcomeProvenance Source() const {
            return {Id<NavigationSnapshotToken>(characterRevision),
                    binding.world,
                    activation.topology,
                    dynamic.Revision().Value(),
                    1,
                    1,
                    1,
                    0};
        }

        NavigationMovementObservation Now(const std::uint64_t tick) const {
            return {.binding = binding,
                    .position = character->ControllerTransform(controller).Value().position,
                    .stepSeconds = StepSeconds,
                    .path = {.provenance = Source(), .linkRevision = 1, .goalRevision = 1, .tick = tick}};
        }

        NavigationMovementRequest RequestTo(const Math::Vec3 goal = {9, RootHeight, 5}) const {
            return {.destination = goal,
                    .acceptanceRadiusMeters = 0.2F,
                    .speedMetersPerSecond = 2,
                    .timeoutTicks = 600,
                    .maximumQueries = 16,
                    .query = Request().requirement,
                    .capabilityRevision = 1};
        }

        void Start(NavigationMovementRequest request) {
            REQUIRE(movement->Start(request, Now(lastTick)).HasValue());
        }

        NavigationCrowdSnapshot Capture(const std::uint64_t tick) const {
            const auto snapshot = character->ControllerLocomotionSnapshot(controller);
            const std::array motions{NavigationCrowdMotionSample{.handle = agent,
                                                                 .position = Now(tick).position,
                                                                 .velocity = snapshot.HasValue()
                                                                                 ? snapshot.Value().movement.achievedVelocityMetersPerSecond
                                                                                 : Math::Vec3{}}};
            const std::array profiles{NavigationCrowdProfileFacts{.profile = Id<NavigationAgentProfileId>(1),
                                                                  .radiusMeters = 0.25F,
                                                                  .neighborRadiusMeters = 4,
                                                                  .maximumNeighbors = 16,
                                                                  .maximumBoundarySegments = 16}};
            const NavigationCrowdSnapshotLimits limits{.maximumAgents = 4,
                                                       .maximumBoundarySegments = 16,
                                                       .maximumCellEntries = 32,
                                                       .maximumNeighborFacts = 16,
                                                       .maximumBoundaryFacts = 16,
                                                       .maximumPairChecks = 16,
                                                       .maximumBoundaryChecks = 64};
            return BuildNavigationCrowdSnapshot(agents.Snapshot().Value(), dynamic.Snapshot().Value(), motions, profiles, limits, tick)
                .Value();
        }

        /** @brief Test-only finite synchronization between simulation ticks; the runtime APIs remain nonblocking. */
        void CommitQueries(const std::uint64_t tick) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            std::uint32_t published{};
            do {
                published += coordinator->Commit({activation, Source(), callers, tick});
                coordinator->CollectRetiredAtSafePoint();
                if (published >= dispatched || coordinator->IsDrained())
                    break;
                std::this_thread::yield();
            } while (std::chrono::steady_clock::now() < deadline);
            REQUIRE((published >= dispatched || coordinator->IsDrained()));
            dispatched = 0;
        }

        /** @brief Runs real owner phases and proves neither query nor safe-velocity admission mutates the Character root. */
        Result<NavigationVelocityPublication> Advance(const std::uint64_t tick) {
            const auto before = character->ControllerTransform(controller).Value();
            CommitQueries(tick);
            RequireSameRoot(before);
            const auto capture = [&] {
                Tests::AllocationProbe::ScopedMeasurement window;
                auto result = Capture(tick);
                captureAllocations = window.Snapshot();
                return result;
            }();
            const auto current = Now(tick);
            const auto work =
                velocity->Prepare(current, capture, {.maximumAccelerationMetersPerSecondSquared = 20, .stepSeconds = StepSeconds});
            RequireSameRoot(before);
            if (work.HasError())
                return Result<NavigationVelocityPublication>::Failure(work.ErrorValue());
            const auto sampled = crowd->Solve(capture, work.Value().request);
            REQUIRE(sampled.HasValue());
            const auto commitStart = std::chrono::steady_clock::now();
            auto publication = [&] {
                Tests::AllocationProbe::ScopedMeasurement window;
                auto result = velocity->Commit(work.Value(), sampled.Value(), capture, current);
                commitAllocations = window.Snapshot();
                return result;
            }();
            commitLatency = std::chrono::steady_clock::now() - commitStart;
            REQUIRE(publication.HasValue());
            auto motion = bridge.Prepare(publication.Value(), current, tick + 100);
            REQUIRE(motion.HasValue());
            const auto admitted = bridge.Submit(motion.Value(), current);
            REQUIRE(admitted.HasValue());
            RequireSameRoot(before);
            AdvancePhysics();
            RequireSameRoot(before);
            REQUIRE(character->RefreshPhysicsSnapshot(physics->Identity(), physics->PublishedTick().publicationRevision).HasValue());
            Character::CharacterPhysicsQueryAdapter queries(*physics);
            REQUIRE(character
                        ->AdvanceFixedTick({.tick = tick,
                                            .sceneGeneration = activation.sceneGeneration.Value(),
                                            .fixedDelta = Duration::FromNanoseconds(16'666'667),
                                            .query = queries.Context(Expectations(tick))})
                        .HasValue());
            const auto afterCharacter = character->ControllerTransform(controller).Value();
            REQUIRE(bridge.ObserveFeedback().HasValue());
            RequireSameRoot(afterCharacter);
            dispatched = coordinator->Dispatch(tick);
            RequireSameRoot(afterCharacter);
            lastTick = tick;
            return publication;
        }

        void RequireSameRoot(const Character::CharacterTransformPublication &before) const {
            const auto after = character->ControllerTransform(controller).Value();
            CHECK(after.position == before.position);
            CHECK(after.sourceTick == before.sourceTick);
            CHECK(after.publicationRevision == before.publicationRevision);
        }

        void Close() noexcept {
            if (closed)
                return;
            closed = true;
            bridge.Shutdown();
            if (velocity)
                velocity->Retire();
            if (movement) {
                movement->BeginShutdown();
                movement->CollectRetiredAtSafePoint();
            }
            coordinator->BeginShutdown();
            navigation.BeginShutdown();
            jobs.Shutdown(ShutdownPolicy::Cancel);
            if (movement)
                movement->CollectRetiredAtSafePoint();
            coordinator->CollectRetiredAtSafePoint();
            if (scene)
                scene->Shutdown();
            agents.BeginShutdown();
            dynamic.BeginShutdown();
            if (character)
                character->Shutdown();
            if (physics)
                physics->Shutdown();
            if (physicsRuntime)
                physicsRuntime->Shutdown();
        }
    };
}  // namespace Horo::Navigation::Qualification
