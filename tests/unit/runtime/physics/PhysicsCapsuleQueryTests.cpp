#include "Horo/Physics/PhysicsQuery.h"
#include "Horo/Physics/PhysicsWorldSettings.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Physics {
    TEST_CASE("Analytic capsule overlap rejects malformed geometry before native construction", "[physics][query][descriptor]") {
        const auto world = PhysicsWorldId::Create(17).Value();
        PhysicsQueryDescriptor descriptor;
        descriptor.world = world;
        descriptor.sceneGeneration = 9;
        descriptor.filter.channel = PhysicsQueryChannelId::Parse("03000000-0000-0000-0000-000000000003").Value();
        PhysicsCapsuleOverlapQuery capsule{{0.5F, 0.5F}, {1, 2, 3}, {1, 0, 0}};
        descriptor.geometry = capsule;
        REQUIRE(ValidatePhysicsQueryDescriptor(descriptor, world, 9).HasValue());
        SECTION("zero radius") {
            capsule.capsule.radiusMeters = 0;
        }
        SECTION("zero cylindrical height") {
            capsule.capsule.cylindricalHalfHeightMeters = 0;
        }
        SECTION("non-finite height") {
            capsule.capsule.cylindricalHalfHeightMeters = std::numeric_limits<float>::infinity();
        }
        SECTION("invalid up") {
            capsule.up = {0, 2, 0};
        }
        SECTION("non-finite position") {
            capsule.position.z = std::numeric_limits<float>::quiet_NaN();
        }
        SECTION("outside origin envelope") {
            capsule.position.y = MaximumPhysicsLocalHalfExtentMeters + 1;
        }
        SECTION("outside shape envelope") {
            capsule.capsule.radiusMeters = MaximumPhysicsLocalHalfExtentMeters;
        }
        descriptor.geometry = capsule;
        REQUIRE(ValidatePhysicsQueryDescriptor(descriptor, world, 9).HasError());
    }

}  // namespace Horo::Physics
