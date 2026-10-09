#include "CharacterGameplayTestSupport.h"

#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>

#if HORO_TEST_PHYSICS_NATIVE
namespace {
    using namespace Horo;
    using namespace Horo::Character;
    using namespace Horo::Physics;
    using Tests::CharacterIntegration::CanonicalCharacterHost;

    /** @brief Installs actual native analytic geometry with the admitted Character filter. */
    PhysicsQueryFixture Box(CanonicalCharacterHost &host, const Math::Vec3 center, const Math::Vec3 halfExtents) {
        const PhysicsQueryFixtureDescriptor descriptor{.shape = PhysicsBoxShape{halfExtents},
                                                       .pose = {.translation = center},
                                                       .layer = *host.descriptor.selectors.requiredLayer,
                                                       .profile = host.descriptor.collisionProfile,
                                                       .channel = host.descriptor.queryChannel};
        const auto result = host.physics->CreateQueryFixture(descriptor);
        REQUIRE(result.HasValue());
        return result.Value();
    }

    /** @brief Rechecks the complete actual capsule against native overlap evidence, not merely its center or endpoint. */
    void RequireClear(CanonicalCharacterHost &host, const CharacterLocomotionSnapshot &snapshot) {
        CharacterPhysicsQueryAdapter adapter{*host.physics};
        const auto context = adapter.Context(host.Expectations(snapshot.tick));
        const auto &owner = host.character->Descriptor();
        const CharacterOverlapProbeRequest request{host.controller,
                                                   owner.sceneGeneration,
                                                   owner.identity,
                                                   owner.physicsWorld,
                                                   snapshot.capsule,
                                                   snapshot.transform.position,
                                                   snapshot.transform.up,
                                                   host.descriptor.collisionProfile,
                                                   host.descriptor.queryChannel,
                                                   0,
                                                   host.descriptor.selectors};
        const auto overlap = context.overlap(context.context, request);
        REQUIRE(overlap.HasValue());
        REQUIRE(overlap.Value().overlapCount == 0);
    }

    TEST_CASE("Canonical fixed-tick capsule cannot cross a thin native obstacle at admitted speed",
              "[gameplay-physics][character][geometry][thin][native]") {
        const float thickness = GENERATE(0.002F, 0.02F, 0.08F);
        const float speed = GENERATE(60.0F, 600.0F);
        CanonicalCharacterHost host;
        static_cast<void>(Box(host, {1, 1, 0}, {thickness * 0.5F, 2, 100}));
        CharacterLocomotionSnapshot snapshot;
        for (std::uint64_t tick = 1; tick <= 4; ++tick) {
            snapshot = host.Move(tick, {speed, 0, speed});
            REQUIRE(snapshot.transform.position.x <= 1 - thickness * 0.5F - snapshot.capsule.radiusMeters + 1.0e-4F);
            REQUIRE(snapshot.transform.position.z == Catch::Approx(speed * static_cast<float>(tick) / 60).margin(0.002F));
            RequireClear(host, snapshot);
        }
        host.character->Shutdown();
        REQUIRE(ValidateCharacterLocomotionSnapshot(snapshot, host.descriptor).HasValue());
    }

    TEST_CASE("Canonical native corners preserve crease movement and deterministic fixture-order results",
              "[gameplay-physics][character][geometry][corner][native][determinism]") {
        const auto simulate = [](const bool reverse) {
            CanonicalCharacterHost host;
            const auto addX = [&] {
                static_cast<void>(Box(host, {1.5F, 1, 0}, {0.5F, 3, 100}));
            };
            const auto addZ = [&] {
                static_cast<void>(Box(host, {0, 1, 1.5F}, {100, 3, 0.5F}));
            };
            if (reverse) {
                addZ();
                addX();
            } else {
                addX();
                addZ();
            }
            CharacterLocomotionSnapshot snapshot;
            for (std::uint64_t tick = 1; tick <= 10; ++tick) {
                snapshot = host.Move(tick, {60, 5, 60});
                REQUIRE(snapshot.transform.position.x <= 0.75F + 1.0e-4F);
                REQUIRE(snapshot.transform.position.z <= 0.75F + 1.0e-4F);
                REQUIRE(snapshot.transform.position.y > host.descriptor.collisionRootPosition.y);
                RequireClear(host, snapshot);
            }
            return snapshot.transform.position;
        };
        const auto forward = simulate(false);
        const auto reverse = simulate(true);
        REQUIRE(forward == reverse);
    }

    TEST_CASE("Canonical coplanar adjacent-box seams permit sustained fixed-tick travel",
              "[gameplay-physics][character][geometry][seam][native]") {
        CanonicalCharacterHost host;
        REQUIRE(host.physics->DestroyQueryFixture(host.ramp).HasValue());
        // Flush analytic tops meet at x=0; the capsule crosses both sides rather than only tracing one primitive.
        static_cast<void>(Box(host, {-5, -0.5F, 0}, {5, 0.5F, 4}));
        static_cast<void>(Box(host, {5, -0.5F, 0}, {5, 0.5F, 4}));
        CharacterPhysicsQueryAdapter adapter{*host.physics};
        const CharacterTeleportRequest teleport{host.controller, 1, {-2, 0.77F, 0}, Math::Quaternion::Identity()};
        REQUIRE(host.character->TeleportController(teleport, adapter.Context(host.Expectations(1))).HasValue());
        REQUIRE(host.character->AdvanceFixedTick({.tick = 1, .sceneGeneration = 1, .fixedDelta = Duration::FromNanoseconds(16'666'667)})
                    .HasValue());
        CharacterLocomotionSnapshot snapshot;
        for (std::uint64_t tick = 2; tick <= 81; ++tick) {
            snapshot = host.Move(tick, {3, 0, 0});
            RequireClear(host, snapshot);
        }
        REQUIRE(snapshot.transform.position.x > 1.9F);
        REQUIRE(snapshot.movement.grounded);
        REQUIRE(snapshot.transform.position.y == Catch::Approx(0.77F).margin(0.002F));
    }
}  // namespace
#endif
