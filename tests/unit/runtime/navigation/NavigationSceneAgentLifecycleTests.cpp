#include "navigation/NavigationSceneLifecycleFixture.h"

namespace Horo::Navigation {
    using namespace SceneLifecycleTestSupport;

    TEST_CASE("Default lifecycle admission identity rejects missing Character revision without registration mutation",
              "[unit][navigation][scene][lifecycle][admission]") {
        Fixture fixture;
        const NavigationSceneAgentLifecycleDescriptor empty;
        REQUIRE(empty.characterRevision == 0);
        const auto scene = *fixture.scene.ActiveScene();
        const auto missingIdentity =
            NavigationSceneAgentLifecycle::Create(fixture.registry, fixture.world, fixture.coordinator, scene, empty);
        REQUIRE(missingIdentity.HasError());
        REQUIRE(missingIdentity.ErrorValue().code.Value() == NavigationErrors::AgentRegistryStale.code.Value());
        const NavigationSceneAgentLifecycleDescriptor missingRevision{fixture.entity, fixture.agent, fixture.caller};
        const auto rejected =
            NavigationSceneAgentLifecycle::Create(fixture.registry, fixture.world, fixture.coordinator, scene, missingRevision);
        REQUIRE(rejected.HasError());
        REQUIRE(rejected.ErrorValue().code.Value() == NavigationErrors::AgentDescriptorInvalid.code.Value());
        REQUIRE(fixture.registry.Find(fixture.agent).HasValue());
        REQUIRE(fixture.lifecycle->ReconcileAtSafePoint(fixture.scene.ActiveScene()).HasValue());
        REQUIRE(fixture.coordinator.IsDrained());
    }

    TEST_CASE("Scene disable destruction and unload retire queued navigation reservations", "[unit][navigation][scene][lifecycle]") {
        for (const bool dispatch : {false, true}) {
            for (int cause = 0; cause < 3; ++cause) {
                Fixture fixture;
                fixture.StartQuery();
                if (dispatch)
                    REQUIRE(fixture.coordinator.Dispatch(0) == 1);
                if (cause == 0)
                    fixture.lifecycle->DisableAtSafePoint();
                else if (cause == 1)
                    fixture.DestroyEntity();
                else {
                    REQUIRE(fixture.scene.QueueUnload().HasValue());
                    fixture.CommitScene();
                    REQUIRE_FALSE(fixture.scene.ActiveScene().has_value());
                }
                REQUIRE(fixture.lifecycle->ReconcileAtSafePoint(fixture.scene.ActiveScene()).HasError());
                REQUIRE(fixture.registry.Find(fixture.agent).HasError());
                fixture.DrainRevokedWorld(1);
                REQUIRE(fixture.fence->destructions == 1);
            }
        }
    }

    TEST_CASE("Destroyed Scene cannot reactivate from late real corridor completion and retains provider until fence",
              "[unit][navigation][scene][lifecycle][jobs][provider]") {
        Fixture fixture{1, true};
        fixture.StartQuery();
        REQUIRE(fixture.coordinator.Dispatch(0) == 1);
        REQUIRE(fixture.fence->WaitEntered());
        fixture.DestroyEntity();
        REQUIRE(fixture.lifecycle->ReconcileAtSafePoint(fixture.scene.ActiveScene()).HasError());
        REQUIRE(fixture.world.Unload(fixture.binding.world).HasValue());
        REQUIRE(fixture.coordinator.Commit({.tick = 1}) == 0);
        fixture.RequireRetainedProvider();
        fixture.FinishHeldProvider();
        REQUIRE(fixture.coordinator.Commit({.tick = 2}) == 0);
        REQUIRE(fixture.coordinator.IsDrained());
        REQUIRE(fixture.world.CollectRetired() == NavigationWorldLifecycleState::Empty);
        REQUIRE(fixture.fence->destructions == 1);
        REQUIRE(fixture.lifecycle->Update(*fixture.scene.ActiveScene(), fixture.Now(2)).HasError());
    }

    TEST_CASE("Scene teleport discards old corridor and never converts displacement into crowd velocity",
              "[unit][navigation][scene][lifecycle][teleport]") {
        Fixture fixture;
        fixture.StartQuery();
        REQUIRE(fixture.coordinator.Dispatch(0) == 1);
        fixture.PublishCompletedQuery(fixture.Now(), 1);
        const auto intent = fixture.lifecycle->Update(*fixture.scene.ActiveScene(), fixture.Now(1));
        REQUIRE(intent.HasValue());
        REQUIRE(intent.Value().state == NavigationMovementState::Following);
        REQUIRE(intent.Value().path.IsValid());
        auto teleported = fixture.Now(2);
        teleported.position = {8, 0, 1};
        REQUIRE(fixture.lifecycle->TeleportAtSafePoint(*fixture.scene.ActiveScene(), teleported, 2).HasValue());
        REQUIRE(fixture.lifecycle->CaptureMotion(*fixture.scene.ActiveScene(), teleported, {70, 0, 0}, 2).HasError());
        REQUIRE(fixture.lifecycle->CaptureMotion(*fixture.scene.ActiveScene(), teleported, {}, 1).HasError());
        const auto motion = fixture.lifecycle->CaptureMotion(*fixture.scene.ActiveScene(), teleported, {}, 2);
        REQUIRE(motion.HasValue());
        REQUIRE(motion.Value().position == teleported.position);
        REQUIRE(motion.Value().velocity == Math::Vec3{});
        const auto next = fixture.lifecycle->Update(*fixture.scene.ActiveScene(), teleported);
        REQUIRE(next.HasValue());
        REQUIRE_FALSE(next.Value().path.IsValid());
        REQUIRE(next.Value().desiredVelocity == Math::Vec3{});
    }

    TEST_CASE("Compatible world and profile rebind preserve accepted Follow identity and goal on real corridors",
              "[unit][navigation][scene][lifecycle][reload][provider]") {
        for (const bool replaceWorld : {false, true}) {
            Fixture fixture;
            const auto [accepted, current] = fixture.BeginFollow();
            fixture.Reload(replaceWorld);
            auto rebound = fixture.Now(1);
            rebound.target = current.target;
            REQUIRE(
                fixture.lifecycle->RebindAtSafePoint(*fixture.scene.ActiveScene(), fixture.agent, fixture.caller, rebound, 2).HasValue());
            const auto pending = fixture.lifecycle->Update(*fixture.scene.ActiveScene(), rebound);
            REQUIRE(pending.HasValue());
            REQUIRE(pending.Value().operation.sequence == accepted.accepted.sequence);
            REQUIRE(pending.Value().operation.agent == fixture.agent);
            REQUIRE_FALSE(pending.Value().path.IsValid());
            REQUIRE(pending.Value().desiredVelocity == Math::Vec3{});
            REQUIRE(fixture.coordinator.Dispatch(1) == 1);
            fixture.PublishCompletedQuery(rebound, 2);
            rebound.path.tick = 2;
            const auto intent = fixture.lifecycle->Update(*fixture.scene.ActiveScene(), rebound);
            RequireFollowingIntent(intent);
            REQUIRE(fixture.coordinator.IsDrained());
            static_cast<void>(fixture.world.CollectRetired());
            REQUIRE(fixture.world.RetiredCount() == 0);
        }
    }

    TEST_CASE("Compatible reload cannot replenish an accepted movement deadline or query ceiling",
              "[unit][navigation][scene][lifecycle][reload][budget]") {
        for (const bool deadline : {false, true}) {
            Fixture fixture;
            auto request = fixture.Move();
            request.timeoutTicks = deadline ? 2 : 20;
            request.maximumQueries = deadline ? 4 : 1;
            REQUIRE(fixture.lifecycle->Start(*fixture.scene.ActiveScene(), request, fixture.Now()).HasValue());
            REQUIRE(fixture.lifecycle->Update(*fixture.scene.ActiveScene(), fixture.Now()).HasValue());
            fixture.Reload();
            auto current = fixture.Now(deadline ? 2 : 1);
            REQUIRE(
                fixture.lifecycle->RebindAtSafePoint(*fixture.scene.ActiveScene(), fixture.agent, fixture.caller, current, 2).HasValue());
            const auto intent = fixture.lifecycle->Update(*fixture.scene.ActiveScene(), current);
            REQUIRE(intent.HasValue());
            REQUIRE(intent.Value().state == (deadline ? NavigationMovementState::TimedOut : NavigationMovementState::Failed));
            REQUIRE(intent.Value().desiredVelocity == Math::Vec3{});
            REQUIRE_FALSE(intent.Value().path.IsValid());
            REQUIRE(fixture.coordinator.IsDrained());
        }
    }

    TEST_CASE("Committed Follow target destruction overrides stale host facts before late path consumption",
              "[unit][navigation][scene][lifecycle][follow]") {
        Fixture fixture;
        auto [accepted, current] = fixture.BeginFollow();
        static_cast<void>(accepted);
        REQUIRE(fixture.coordinator.Dispatch(0) == 1);
        fixture.PublishCompletedQuery(current, 1);
        fixture.Destroy(fixture.target);
        current.path.tick = 1;  // Deliberately retain the old apparently valid target fact.
        const auto intent = fixture.lifecycle->Update(*fixture.scene.ActiveScene(), current);
        REQUIRE(intent.HasValue());
        REQUIRE(intent.Value().state == NavigationMovementState::TargetLost);
        REQUIRE_FALSE(intent.Value().path.IsValid());
        REQUIRE(intent.Value().desiredVelocity == Math::Vec3{});
        REQUIRE(fixture.coordinator.Commit({.tick = 2}) == 0);
        REQUIRE_FALSE(fixture.coordinator.IsDrained());  // The live consumer still owns its terminal retirement receipt.
        fixture.ReconcileLiveRetirement();
    }

    TEST_CASE("Inactive live Scene owner reconciliation drains TargetLost work only after the real worker fence",
              "[unit][navigation][scene][lifecycle][follow][jobs][retirement]") {
        Fixture fixture{1, true};
        auto [accepted, current] = fixture.BeginFollow();
        static_cast<void>(accepted);
        REQUIRE(fixture.coordinator.Dispatch(0) == 1);
        REQUIRE(fixture.fence->WaitEntered());
        fixture.Destroy(fixture.target);
        current.path.tick = 1;
        const auto lost = fixture.lifecycle->Update(*fixture.scene.ActiveScene(), current);
        REQUIRE(lost.HasValue());
        REQUIRE(lost.Value().state == NavigationMovementState::TargetLost);
        REQUIRE(lost.Value().desiredVelocity == Math::Vec3{});
        REQUIRE(fixture.lifecycle->ReconcileAtSafePoint(fixture.scene.ActiveScene()).HasValue());
        REQUIRE(fixture.world.Unload(fixture.binding.world).HasValue());
        fixture.RequireRetainedProvider();
        fixture.FinishHeldProvider();
        // No Update, Commit, disable or owner destruction after TargetLost: the canonical reconcile phase owns drain.
        fixture.ReconcileLiveRetirement();
        REQUIRE(fixture.world.CollectRetired() == NavigationWorldLifecycleState::Empty);
        REQUIRE(fixture.fence->destructions == 1);
    }

    TEST_CASE("Idle Scene owner reconciliation does not poll unrelated coordinator partitions",
              "[unit][navigation][scene][lifecycle][retirement]") {
        Fixture fixture;
        auto request = Request(fixture.binding.world, fixture.activation.topology);
        request.start = {1, 0, 1};
        request.destination = {9, 0, 9};
        const auto submitted = fixture.coordinator.Submit(fixture.world,
                                                          {.caller = fixture.caller,
                                                           .request = request,
                                                           .source = fixture.Now().path.provenance,
                                                           .capabilityRevision = 1,
                                                           .targetTick = 1,
                                                           .deadlineTick = 10},
                                                          0);
        REQUIRE(submitted.HasValue());
        REQUIRE(fixture.coordinator.Dispatch(0) == 1);
        REQUIRE(fixture.coordinator.Abandon(submitted.Value()));
        fixture.jobs.Shutdown(ShutdownPolicy::Drain);
        REQUIRE(fixture.coordinator.IsRetained(submitted.Value()));
        REQUIRE(fixture.lifecycle->ReconcileAtSafePoint(fixture.scene.ActiveScene()).HasValue());
        REQUIRE(fixture.coordinator.IsRetained(submitted.Value()));
        REQUIRE_FALSE(fixture.coordinator.IsDrained());
        fixture.coordinator.CollectRetiredAtSafePoint();
        REQUIRE_FALSE(fixture.coordinator.IsRetained(submitted.Value()));
        REQUIRE(fixture.coordinator.IsDrained());
    }
}  // namespace Horo::Navigation
