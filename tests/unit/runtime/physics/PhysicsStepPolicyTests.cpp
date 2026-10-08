#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsStepPolicy.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Physics {
    TEST_CASE("Canonical step policy admits explicit pinned defaults", "[physics][settings]") {
        const PhysicsStepPolicy policy;
        REQUIRE(ValidatePhysicsStepPolicy(policy).HasValue());
        REQUIRE(policy.substepsPerTick == 1);
        REQUIRE(policy.velocityIterations == 10);
        REQUIRE(policy.positionIterations == 2);
    }

    TEST_CASE("Step policy rejects malformed independent numeric fields", "[physics][settings]") {
        using Mutation = void (*)(PhysicsStepPolicy &);
        const std::array<Mutation, 7> mutations{
            [](auto &p) {
            p.substepsPerTick = 0;
        },
            [](auto &p) {
            p.velocityIterations = 0;
        },
            [](auto &p) {
            p.positionIterations = 0;
        },
            [](auto &p) {
            p.sleepPointSpeedMetersPerSecond = std::numeric_limits<float>::infinity();
        },
            [](auto &p) {
            p.sleepPointSpeedMetersPerSecond = 0;
        },
            [](auto &p) {
            p.sleepDelaySeconds = std::numeric_limits<float>::quiet_NaN();
        },
            [](auto &p) {
            p.sleepDelaySeconds = -1;
        },
        };
        for (const auto mutate : mutations) {
            PhysicsStepPolicy policy;
            mutate(policy);
            const auto result = ValidatePhysicsStepPolicy(policy);
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == PhysicsErrors::DescriptorInvalid.code.Value());
        }
    }

    TEST_CASE("Step policy rejects unadmitted overrides without modifying them", "[physics][settings]") {
        using Mutation = void (*)(PhysicsStepPolicy &);
        const std::array<Mutation, 6> mutations{
            [](auto &p) {
            p.substepsPerTick = 2;
        },
            [](auto &p) {
            p.velocityIterations = 8;
        },
            [](auto &p) {
            p.positionIterations = 3;
        },
            [](auto &p) {
            p.sleepPointSpeedMetersPerSecond = 0.02F;
        },
            [](auto &p) {
            p.sleepDelaySeconds = 1;
        },
            [](auto &p) {
            p.sleepingEnabled = false;
        },
        };
        for (const auto mutate : mutations) {
            PhysicsStepPolicy policy;
            mutate(policy);
            const auto retained = policy;
            const auto result = ValidatePhysicsStepPolicy(policy);
            REQUIRE(result.HasError());
            REQUIRE(result.ErrorValue().code.Value() == PhysicsErrors::ProfileUnsupported.code.Value());
            REQUIRE(policy == retained);
        }
        PhysicsStepPolicy unknown;
        unknown.defaultMotionQuality = static_cast<PhysicsDefaultMotionQuality>(255);
        const auto result = ValidatePhysicsStepPolicy(unknown);
        REQUIRE(result.HasError());
        REQUIRE(result.ErrorValue().code.Value() == PhysicsErrors::OperationUnsupported.code.Value());
    }
}  // namespace Horo::Physics
