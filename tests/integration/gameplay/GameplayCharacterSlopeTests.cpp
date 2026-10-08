#include "CharacterGameplayTestSupport.h"
#include "GameplayModuleTestSupport.h"
#include "GameplayRuntimeTestSupport.h"
#include "GameplayWorldComposition.h"
#include "Horo/Physics/CharacterWorld.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsTestUtils.h"

#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>

#if HORO_TEST_PHYSICS_NATIVE
namespace {
    using namespace Horo;
    using namespace Horo::Application::Internal;
    using namespace Horo::Character;
    using namespace Horo::Gameplay;
    using namespace Horo::Physics;

    using CanonicalSlopeHost = Tests::CharacterIntegration::CanonicalCharacterHost;

    TEST_CASE("Canonical Gameplay Scene ramp traversal preserves intended horizontal speed and copied native support",
              "[gameplay-physics][character][slope][native]") {
        const float degrees = GENERATE(15.0F, 30.0F, 45.0F);
        CanonicalSlopeHost host{degrees};
        const auto client = host.gameplay->PhysicsContext()->Acquire("game.tests", 7, 1).Value();
        PhysicsQueryDescriptor query{.world = host.physics->Identity(),
                                     .sceneGeneration = 1,
                                     .geometry = PhysicsCapsuleSweepQuery{host.descriptor.capsule,
                                                                          host.descriptor.collisionRootPosition,
                                                                          {0, 1, 0},
                                                                          {1, 0, 0},
                                                                          1},
                                     .filter = {.channel = host.descriptor.queryChannel,
                                                .requiredLayer = host.descriptor.selectors.requiredLayer,
                                                .blockingOnly = true},
                                     .collection = PhysicsQueryCollection::All,
                                     .maximumHitCount = 4};
        std::array<PhysicsQueryHit, 4> hits{};
        const auto observed = client.Submit({client.Identity(), host.physics->PublishedTick().publicationRevision, query}, hits);
        REQUIRE(observed.HasValue());
        REQUIRE(observed.Value().result.hitCount == 1);
        REQUIRE(hits[0].body == host.ramp.body);
        REQUIRE(hits[0].normal.has_value());
        for (std::uint64_t tick = 1; tick <= 12; ++tick) {
            const auto snapshot = host.Move(tick, {3, 0, 1});
            REQUIRE(snapshot.movement.achievedVelocityMetersPerSecond.x == Catch::Approx(3).margin(1.0e-3F));
            REQUIRE(snapshot.movement.achievedVelocityMetersPerSecond.z == Catch::Approx(1).margin(1.0e-3F));
            REQUIRE(snapshot.movement.grounded);
            REQUIRE(snapshot.movement.groundBody == host.ramp.body);
            REQUIRE(snapshot.movement.groundNormal.x == Catch::Approx(hits[0].normal->x).margin(1.0e-5F));
            REQUIRE(snapshot.selectors.requiredLayer == host.descriptor.selectors.requiredLayer);
        }
    }

    TEST_CASE("Canonical Gameplay Scene steep stop prevents projection ascent and gravity slide retains physical continuation",
              "[gameplay-physics][character][slope][native]") {
        const auto policy = GENERATE(CharacterSteepSlopePolicy::Stop, CharacterSteepSlopePolicy::Slide);
        CanonicalSlopeHost host{60, policy};
        const auto initial = host.character->ControllerTransform(host.controller).Value().position;
        const auto first = host.Move(1, {3, 0, 0});
        REQUIRE(first.movement.finalPosition.y <= initial.y + 1.0e-5F);
        REQUIRE_FALSE(first.movement.grounded);
        REQUIRE_FALSE(first.movement.platformAttached);
        if (policy == CharacterSteepSlopePolicy::Stop) {
            REQUIRE(first.movement.gravityVelocityMetersPerSecond == Math::Vec3{});
            const auto stopped = host.Move(2, {});
            REQUIRE(stopped.movement.finalPosition == first.movement.finalPosition);
        } else {
            REQUIRE(first.movement.gravityVelocityMetersPerSecond.y < 0);
            const auto second = host.Move(2, {});
            REQUIRE(second.movement.finalPosition.y < first.movement.finalPosition.y);
            REQUIRE(second.movement.gravityVelocityMetersPerSecond.y ==
                    Catch::Approx(first.movement.gravityVelocityMetersPerSecond.y * 2).margin(1.0e-4F));
            host.gameplay->Shutdown();
            host.character->Shutdown();
            REQUIRE(ValidateCharacterLocomotionSnapshot(second, host.descriptor).HasValue());
            REQUIRE(host.character->ControllerLocomotionSnapshot(host.controller).HasError());
        }
    }

    TEST_CASE("Canonical steep slide agrees across equal time fixed tick partitions",
              "[gameplay-physics][character][slope][native][continuation]") {
        CharacterLocomotionSnapshot actual;
        {
            CanonicalSlopeHost partitioned{60, CharacterSteepSlopePolicy::Slide};
            for (std::uint64_t tick = 1; tick <= 4; ++tick)
                actual = partitioned.Move(tick, {}, 25'000'000);
        }
        CanonicalSlopeHost reference{60, CharacterSteepSlopePolicy::Slide};
        const auto expected = reference.Move(1, {}, 100'000'000);
        REQUIRE(actual.movement.finalPosition.x == Catch::Approx(expected.movement.finalPosition.x).margin(1.0e-5F));
        REQUIRE(actual.movement.finalPosition.y == Catch::Approx(expected.movement.finalPosition.y).margin(1.0e-5F));
        REQUIRE(actual.movement.gravityVelocityMetersPerSecond.y ==
                Catch::Approx(expected.movement.gravityVelocityMetersPerSecond.y).margin(1.0e-5F));
    }
}  // namespace
#endif
