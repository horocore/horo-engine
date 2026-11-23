#include "Horo/Gameplay/GameplayFormationMovement.h"
#include "navigation/NavigationRuntimeQualificationFixture.h"
#include "navigation/NavigationTestAssertions.h"

#include <algorithm>
#include <cmath>
#include <iostream>

namespace Horo::Navigation {
    namespace {
        using namespace Qualification;

        /** @brief Terminal expectations are authored per corpus; a timeout is never accepted as successful arrival. */
        NavigationMovementState RunToTerminal(Host &host, const std::uint64_t firstTick = 1) {
            for (auto tick = firstTick; tick <= 601; ++tick) {
                const auto result = host.Advance(tick);
                REQUIRE(result.HasValue());
                CHECK(host.commitAllocations.requests == 0);
                const auto root = host.character->ControllerTransform(host.controller).Value();
                CHECK(std::abs(root.position.y - RootHeight) < 0.001F);
                if ((result.Value().intent.state >= NavigationMovementState::Arrived))
                    return result.Value().intent.state;
            }
            FAIL("bounded corpus did not reach a terminal typed outcome");
            return NavigationMovementState::Failed;
        }

        /** @brief Assert logical owners and physical worker/provider pins have all drained after the host boundary. */
        void RequireClosed(Host &host) {
            host.Close();
            REQUIRE(host.movement->IsDrained());
            REQUIRE(host.coordinator->IsDrained());
            REQUIRE(host.navigation.CollectRetired() == NavigationWorldLifecycleState::Closed);
            REQUIRE(host.navigation.RetiredCount() == 0);
            REQUIRE(host.character->State() == Character::CharacterWorldState::Destroyed);
            REQUIRE(host.character->ActiveControllerCount() == 0);
            REQUIRE(host.dynamic.IsShutdown());
            REQUIRE(host.agents.ActiveBinding().HasError());
            REQUIRE(host.physics->State() == Physics::PhysicsWorldState::Destroyed);
        }

        /** @brief Borrow caller-owned scope values and admit the two real pending jobs at monotonic ticks. */
        NavRequestHandle AdmitReloadQuery(Host &host, const NavigationPathSubmission &pending) {
            const auto handle = host.coordinator->Submit(host.navigation, pending, 1);
            if (handle.HasError()) {
                UNSCOPED_INFO(handle.ErrorValue().domain.Value());
                UNSCOPED_INFO(handle.ErrorValue().code.Value());
                UNSCOPED_INFO(handle.ErrorValue().message);
            }
            REQUIRE(handle.HasValue());
            REQUIRE(host.coordinator->Dispatch(2) == 2);
            return handle.Value();
        }

        /** @brief Drain cancelled native results without acquiring or copying any world-generation lease. */
        void RequireCancelledReloadDrain(NavigationCoordinator &retired, Host &host, const NavRequestHandle handle) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            std::optional<NavigationPathCompletion> cancelled;
            do {
                static_cast<void>(retired.Commit({host.activation, host.Source(), host.callers, 2}));
                if (!cancelled)
                    cancelled = retired.Take(handle);
                retired.CollectRetiredAtSafePoint();
                if (cancelled && retired.IsDrained())
                    break;
                std::this_thread::yield();
            } while (std::chrono::steady_clock::now() < deadline);
            REQUIRE(cancelled);
            TestSupport::RequireError(cancelled->result, NavigationErrors::QueryCancelled);
            REQUIRE(retired.IsDrained());
            REQUIRE_FALSE(retired.Take(handle));
            REQUIRE_FALSE(retired.IsRetained(handle));
        }

        /** @brief Reject old agent/controller/capability/bridge values without copying their caller-owned grants. */
        void RequireOldCharacterScopeRejected(Host &host, const CrowdAgentHandle oldAgent,
                                              const Character::CharacterControllerHandle oldController,
                                              const Character::CharacterCapability &oldGrant, const NavigationCharacterMotion &oldMotion) {
            TestSupport::RequireError(oldGrant.Validate(), Character::CharacterErrors::CapabilityRevoked);
            REQUIRE(host.agents.Find(oldAgent).HasError());
            REQUIRE(host.character->ControllerTransform(oldController).HasError());
            REQUIRE(host.bridge.Submit(oldMotion, host.Now(1)).HasError());
        }

    }  // namespace

    TEST_CASE("Reference floor reaches its goal through actual Detour crowd and canonical Character fixed ticks",
              "[integration][navigation][qualification][native][character]") {
        Host host;
        host.Start(host.RequestTo());
        std::array<std::int64_t, 601> latency{};
        std::size_t samples{};
        bool arrived{};
        std::size_t captureRequests{};
        std::size_t captureBytes{};
        for (std::uint64_t tick = 1; tick <= latency.size(); ++tick) {
            const auto result = host.Advance(tick);
            REQUIRE(result.HasValue());
            CHECK(host.commitAllocations.requests == 0);
            CHECK(result.Value().binding == host.binding);
            CHECK(result.Value().characterRevision == 1);
            CHECK(result.Value().intent.tick == tick);
            latency[samples++] = host.commitLatency.count();
            captureRequests += host.captureAllocations.requests;
            captureBytes += host.captureAllocations.requestedBytes;
            const auto root = host.character->ControllerTransform(host.controller).Value();
            CHECK(root.sourceTick == tick);
            CHECK(std::abs(root.position.y - RootHeight) < 0.001F);
            if (result.Value().intent.state == NavigationMovementState::Following) {
                CHECK(result.Value().disposition == NavigationAvoidanceDisposition::Sampled);
                CHECK(result.Value().stopReason == NavigationAvoidanceStopReason::None);
                CHECK(result.Value().intent.path.IsValid());
            }
            if ((result.Value().intent.state >= NavigationMovementState::Arrived)) {
                REQUIRE(result.Value().intent.state == NavigationMovementState::Arrived);
                arrived = true;
                break;
            }
        }
        REQUIRE(arrived);
        const auto root = host.Now(host.lastTick).position;
        CHECK(std::abs(root.x - 9) <= 0.2F);
        CHECK(std::abs(root.z - 5) <= 0.2F);
        std::sort(latency.begin(), latency.begin() + static_cast<std::ptrdiff_t>(samples));
        std::cout << "NAV-004.12 profile=RuntimeQualificationSmall provider=RecastDetour physics=CanonicalV1 build="
                  << HORO_QUALIFICATION_BUILD_TYPE << " platform=" << HORO_QUALIFICATION_PLATFORM
                  << " compiler=" << HORO_QUALIFICATION_COMPILER << " samples=" << samples << " commit_p50_ns=" << latency[samples / 2]
                  << " commit_p95_ns=" << latency[(samples - 1) * 95 / 100]
                  << " owner_cpp_commit_allocations=0 capture_cpp_allocations=" << captureRequests << " capture_cpp_bytes=" << captureBytes
                  << " direct_native_allocations=unobserved\n";
        RequireClosed(host);
    }

    TEST_CASE("Native crowd density slows Character inside a narrow corridor and releases the cap after passing the neighbor",
              "[integration][navigation][qualification][native][character][density]") {
        const CrowdCorridor corridor;
        Host host(false, false, corridor);
        host.Start(host.RequestTo({7, RootHeight, 5}));
        bool advancedUnderCap{};
        bool resumedFullSpeed{};
        bool arrived{};
        for (std::uint64_t tick = 1; tick <= 600; ++tick) {
            const auto publication = host.Advance(tick);
            REQUIRE(publication.HasValue());
            const auto &intent = publication.Value().intent;
            const auto position = host.Now(tick).position;
            CHECK(position.z >= corridor.minimumZ);
            CHECK(position.z <= corridor.maximumZ);
            CHECK(position.x >= 0.0F);
            CHECK(position.x <= 10.0F);
            const auto speed = std::hypot(intent.desiredVelocity.x, intent.desiredVelocity.z);
            CHECK(speed <= 2.001F);
            if (intent.state == NavigationMovementState::Following && position.x < 2.5F) {
                CHECK(speed <= 1.001F);
                advancedUnderCap = advancedUnderCap || position.x > 1.5F;
            }
            if (position.x > 5.0F && speed > 1.1F)
                resumedFullSpeed = true;
            if (intent.state >= NavigationMovementState::Arrived) {
                REQUIRE(intent.state == NavigationMovementState::Arrived);
                arrived = true;
                break;
            }
        }
        CHECK(advancedUnderCap);
        CHECK(resumedFullSpeed);
        REQUIRE(arrived);
        CHECK(std::abs(host.Now(host.lastTick).position.x - 7.0F) <= 0.2F);
        RequireClosed(host);
    }

    TEST_CASE("Formation movement carries a Character toward its native slot and stops when the layout dissolves",
              "[integration][navigation][qualification][native][character][formation]") {
        using namespace Gameplay;
        const CrowdCorridor corridor;
        Host host(false, false, corridor);
        const auto leased = host.navigation.Acquire(host.activation.world);
        REQUIRE(leased.HasValue());
        const auto snapshot = host.agents.Snapshot().Value();
        const std::array members{GameplayFormationMember{host.agent, host.Now(0).position},
                                 GameplayFormationMember{host.neighbor,
                                                         host.character->ControllerTransform(host.neighborController).Value().position}};
        const auto group = GameplayFormationId::Create(1).Value();
        const std::array offsets{Math::Vec3{3, 0, 0}};
        const auto plan = PrepareGameplayFormation({.group = group,
                                                    .revision = 1,
                                                    .shape = GameplayFormationShape::Custom,
                                                    .preferredLeader = host.neighbor,
                                                    .customSlots = offsets},
                                                   snapshot, members);
        REQUIRE(plan.HasValue());
        const GameplayFormationQueryPolicy queries{.projection = {NavigationQueryKind::NearestPoint,
                                                                  NavigationQualityLevel::Balanced,
                                                                  {64, 16, 100}},
                                                   .path = Request().requirement,
                                                   .projectionHalfExtents = {1, 2, 1}};
        const auto resolved = ResolveGameplayFormation(plan.Value(), snapshot, leased.Value(), queries);
        REQUIRE(resolved.HasValue());
        REQUIRE(resolved.Value().goals.size() == 2);

        NavigationPathSubmission policy{.caller = host.callers.front(), .request = Request(), .capabilityRevision = 1};
        const auto context = GameplayNavigationContext::Create({"game.formation", host.binding, false, true}, &host.navigation,
                                                               host.coordinator.get(), host.movement.get(), policy);
        REQUIRE(context.HasValue());
        REQUIRE(context.Value()->Refresh(host.Now(0)).HasValue());
        const std::array grants{GameplayFormationMovementBinding{host.agent, context.Value().get()}};
        GameplayFormationMovement movement{group};
        const auto applied = movement.Apply(resolved.Value(), 1, host.navigation, grants);
        REQUIRE(applied.HasValue());
        CHECK(applied.Value().availability == GameplayFormationAvailability::Degraded);
        REQUIRE(applied.Value().started == std::vector{host.agent});
        REQUIRE(applied.Value().unassigned == std::vector{host.neighbor});
        REQUIRE(movement.Apply(resolved.Value(), 1, host.navigation, grants).HasError());

        for (std::uint64_t tick = 1; tick <= 45; ++tick)
            REQUIRE(host.Advance(tick).HasValue());
        const auto before = host.Now(45).position;
        CHECK(before.x > 1.25F);
        CHECK(before.z >= corridor.minimumZ);
        CHECK(before.z <= corridor.maximumZ);

        const GameplayFormationResolution empty{.group = group, .revision = 2, .source = host.activation};
        const auto stopped = movement.Apply(empty, 2, host.navigation, grants);
        REQUIRE(stopped.HasValue());
        CHECK(stopped.Value().availability == GameplayFormationAvailability::Dissolved);
        const auto next = host.Advance(46);
        REQUIRE(next.HasValue());
        CHECK(next.Value().intent.state == NavigationMovementState::Cancelled);
        CHECK(host.Now(46).position == before);
        context.Value()->Revoke();
        movement.Retire();
    }

    TEST_CASE("Disconnected reference floor preserves an explicit unreachable outcome without physical movement",
              "[integration][navigation][qualification][native][character]") {
        Host host(true);
        const auto before = host.Now(0).position;
        host.Start(host.RequestTo());
        REQUIRE(RunToTerminal(host) == NavigationMovementState::Unreachable);
        CHECK(host.Now(host.lastTick).position == before);
        RequireClosed(host);
    }

    TEST_CASE("Physics obstacle prevents a navigable corridor from bypassing canonical collision ownership",
              "[integration][navigation][qualification][native][character]") {
        Host host(false, true);
        host.Start(host.RequestTo());
        bool physicallyBlocked{};
        std::uint32_t repaths{};
        for (std::uint64_t tick = 1; tick <= 180; ++tick) {
            const auto result = host.Advance(tick);
            REQUIRE(result.HasValue());
            CHECK(result.Value().intent.state != NavigationMovementState::Arrived);
            if (result.Value().recovery == NavigationCrowdRecovery::RepathRequested)
                ++repaths;
            const auto root = host.Now(tick).position;
            CHECK(root.x < 3.75F);
            const auto motion = host.character->ControllerLocomotionSnapshot(host.controller);
            REQUIRE(motion.HasValue());
            physicallyBlocked = physicallyBlocked || (static_cast<std::uint16_t>(motion.Value().movement.collisions) &
                                                      static_cast<std::uint16_t>(Character::CharacterCollisionFlags::Sides)) != 0;
        }
        REQUIRE(physicallyBlocked);
        CHECK(repaths > 0);
        CHECK(repaths <= 18);
        RequireClosed(host);
    }

    TEST_CASE("Off-plane goal is rejected explicitly while the collision root stays at its admitted height",
              "[integration][navigation][qualification][native][character]") {
        Host host;
        host.Start(host.RequestTo({9, 1.2F, 5}));
        REQUIRE(host.Advance(1).HasValue());
        const auto before = host.Now(1).position;
        TestSupport::RequireError(host.Advance(2), NavigationErrors::OperationUnsupported);
        CHECK(host.Now(1).position == before);
        RequireClosed(host);
    }

    TEST_CASE("Rapid retargeting retires actual native requests and only the last operation can arrive",
              "[integration][navigation][qualification][native][character][lifecycle]") {
        Host host;
        for (std::uint64_t tick = 1; tick <= 24; ++tick) {
            host.Start(host.RequestTo({tick % 2 == 0 ? 8.0F : 2.0F, RootHeight, 5}));
            const auto operation = host.movement->Status().id;
            const auto publication = host.Advance(tick);
            REQUIRE(publication.HasValue());
            CHECK(publication.Value().intent.operation == operation);
        }
        host.Start(host.RequestTo());
        const auto finalOperation = host.movement->Status().id;
        REQUIRE(RunToTerminal(host, 25) == NavigationMovementState::Arrived);
        CHECK(host.movement->Status().id == finalOperation);
        RequireClosed(host);
    }

    TEST_CASE("Scene reload revokes old Character and agent authority while pending native queries drain",
              "[integration][navigation][qualification][native][character][lifecycle]") {
        Host host;
        auto lease = host.navigation.Acquire(World()).Value();
        const auto oldAgent = host.agent;
        const auto oldController = host.controller;
        const auto oldGrant = host.grant;
        const auto oldBinding = host.binding;
        const auto oldSource = host.Source();
        const auto oldCallers = host.callers;
        host.Start(host.RequestTo());
        const auto root = host.character->ControllerTransform(host.controller).Value();
        const auto capture = host.Capture(1);
        const auto work =
            host.velocity->Prepare(host.Now(1), capture, {.maximumAccelerationMetersPerSecondSquared = 20, .stepSeconds = StepSeconds});
        REQUIRE(work.HasValue());
        const auto sampled = host.crowd->Solve(capture, work.Value().request);
        REQUIRE(sampled.HasValue());
        const auto publication = host.velocity->Commit(work.Value(), sampled.Value(), capture, host.Now(1));
        REQUIRE(publication.HasValue());
        const auto oldMotion = host.bridge.Prepare(publication.Value(), host.Now(1), 101);
        REQUIRE(oldMotion.HasValue());
        host.RequireSameRoot(root);
        auto request = Request();
        request.start = {1, RootHeight, 5};
        request.destination = {9, RootHeight, 5};
        const NavigationPathSubmission pending{.caller = oldCallers[0],
                                               .request = request,
                                               .source = oldSource,
                                               .capabilityRevision = 1,
                                               .targetTick = 2,
                                               .deadlineTick = 100};
        const auto handle = AdmitReloadQuery(host, pending);
        host.RequireSameRoot(root);
        auto retired = host.Reload();
        REQUIRE(host.binding.sceneGeneration.Value() == oldBinding.sceneGeneration.Value() + 1);
        REQUIRE(host.activation.topology != oldSource.topology);
        REQUIRE(lease.IsRevoked());
        RequireOldCharacterScopeRejected(host, oldAgent, oldController, oldGrant, oldMotion.Value());
        // World-incarnation validation precedes topology comparison: the retired world is no longer acquirable.
        TestSupport::RequireError(host.coordinator->Submit(host.navigation, pending, 0), NavigationErrors::InvalidWorld);
        RequireCancelledReloadDrain(*retired, host, handle);
        REQUIRE(host.navigation.RetiredCount() == 1);
        lease = {};
        REQUIRE(host.navigation.CollectRetired() == NavigationWorldLifecycleState::Active);
        REQUIRE(host.navigation.RetiredCount() == 0);
        host.Start(host.RequestTo());
        REQUIRE(RunToTerminal(host) == NavigationMovementState::Arrived);
        RequireClosed(host);
        REQUIRE(retired->IsDrained());
    }
}  // namespace Horo::Navigation
