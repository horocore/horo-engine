#include "CharacterGameplayTestSupport.h"

#include <catch2/catch_approx.hpp>

#if HORO_TEST_PHYSICS_NATIVE
namespace {
    using namespace Horo;
    using namespace Horo::Character;
    using namespace Horo::Physics;
    using Tests::CharacterIntegration::CanonicalCharacterHost;

    TEST_CASE("Canonical Physics Scene Gameplay jump hits a ceiling then lands exactly once",
              "[gameplay-physics][character][airborne][native]") {
        CanonicalCharacterHost host;
        REQUIRE(host.Move(1, {}).movement.grounded);
        const PhysicsQueryFixtureDescriptor ceiling{.shape = PhysicsStaticPlaneShape{{0, -1, 0}, -1.7F},
                                                    .layer = *host.descriptor.selectors.requiredLayer,
                                                    .profile = host.descriptor.collisionProfile,
                                                    .channel = host.descriptor.queryChannel};
        REQUIRE(host.physics->CreateQueryFixture(ceiling).HasValue());
        const auto jump = host.Move(2, {1, 0, 0}, 16'666'667, true);
        REQUIRE(jump.movement.jumpApplied);
        REQUIRE(jump.movement.groundTransition == CharacterGroundTransition::LeftGround);
        std::uint32_t landings{};
        bool ceilingHit{};
        for (std::uint64_t tick = 3; tick <= 70; ++tick) {
            const auto snapshot = host.Move(tick, {1, 0, 0}, 16'666'667, tick < 8);
            REQUIRE_FALSE(snapshot.movement.jumpApplied);
            if ((static_cast<std::uint16_t>(snapshot.movement.collisions) & static_cast<std::uint16_t>(CharacterCollisionFlags::Ceiling)) !=
                0) {
                ceilingHit = true;
                REQUIRE(snapshot.movement.gravityVelocityMetersPerSecond.y <= 0.0F);
                REQUIRE(snapshot.movement.achievedVelocityMetersPerSecond.x == Catch::Approx(1.0F).margin(1.0e-4F));
            }
            if (snapshot.movement.groundTransition == CharacterGroundTransition::Landed)
                ++landings;
        }
        REQUIRE(ceilingHit);
        REQUIRE(landings == 1);
    }

    TEST_CASE("Canonical airborne outcomes depend on fixed ticks rather than render partitions",
              "[gameplay-physics][character][airborne][native][determinism]") {
        // Each partition is a presentation frame containing this many attempted fixed ticks.
        const auto simulate = [](const std::uint64_t ticksPerFrame) {
            CanonicalCharacterHost host;
            static_cast<void>(host.Move(1, {}));
            CharacterLocomotionSnapshot result;
            for (std::uint64_t frameStart = 2; frameStart <= 61; frameStart += ticksPerFrame) {
                for (std::uint64_t offset = 0; offset < ticksPerFrame && frameStart + offset <= 61; ++offset) {
                    const std::uint64_t tick = frameStart + offset;
                    result = host.Move(tick, {2, 0, 0}, 16'666'667, tick == 2);
                }
                // Presentation reads cannot advance or mutate Character simulation.
                REQUIRE(host.character->ControllerLocomotionSnapshot(host.controller).Value().stateRevision == result.stateRevision);
            }
            return result;
        };
        const auto frequent = simulate(1);
        const auto catchup = simulate(5);
        REQUIRE(frequent.movement.finalPosition == catchup.movement.finalPosition);
        REQUIRE(frequent.movement.gravityVelocityMetersPerSecond == catchup.movement.gravityVelocityMetersPerSecond);
        REQUIRE(frequent.movement.groundTransition == catchup.movement.groundTransition);
        REQUIRE(frequent.movement.grounded == catchup.movement.grounded);
    }
}  // namespace
#endif
