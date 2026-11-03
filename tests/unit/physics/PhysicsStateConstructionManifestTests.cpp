#include "AllocationProbe.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsStateConstructionManifest.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::Physics {
    namespace {
        constexpr PhysicsStateFpControl ModelFp{PhysicsStateFpArchitecture::ArmFpcr, 0};

        struct ManifestFixture final {
            std::array<PhysicsStateShapeRecipe, 1> nodes{{{ModelFp, PhysicsShapeDescriptor{PhysicsBoxShape{}}}}};
            std::array<PhysicsStateShapeRecipeRoot, 1> roots{{{42, 0}}};
            std::array<PhysicsStateConstructionBody, 1> bodies{};
            std::array<PhysicsStateConstructionJoint, 1> joints{};
            std::array<std::uint32_t, 2> scratch{};
            PhysicsStateConstructionManifestLimits limits{{1, 0, 1, 1, 8192}, 1, 0, 0, 1, 0, 0, 8192, 8192};

            ManifestFixture() {
                auto &body = bodies[0];
                body.floatingPoint = ModelFp;
                body.massArithmeticSubprofile = ModelFp;
                body.id = 8;
                body.shape = 42;
                body.policy.motion = PhysicsMotionType::Dynamic;
                body.policy.mass = PhysicsMass{1};
                body.hasMotion = true;
                body.motionStorageReserved = true;
                body.inverseMass = 1;
                body.inverseInertia = {6, 6, 6};
                body.inertiaRotation = {0, 0, 0, 1};
                auto &joint = joints[0];
                joint.id = 3;
                joint.first = 8;
                joint.floatingPoint = ModelFp;
            }

            PhysicsStateConstructionManifest View() const noexcept {
                return {{nodes, {}, roots}, bodies, {}, {}, joints, {}, {}};
            }

            Result<void> Validate() {
                return ValidatePhysicsStateConstructionManifest(View(), limits, ModelFp, scratch);
            }
        };
    }  // namespace

    TEST_CASE("Full constructor manifest validates exact body and original world-anchor joint without allocation",
              "[unit][physics][state][construction-manifest]") {
        ManifestFixture f;
        const auto before = Tests::AllocationProbe::Count();
        auto valid = f.Validate();
        const auto after = Tests::AllocationProbe::Count();
        INFO((valid.HasError() ? valid.ErrorValue().message : "success"));
        REQUIRE(valid.HasValue());
        CHECK(after == before);
    }

    TEST_CASE("Constructor manifest rejects routing a body or joint to missing stable provenance",
              "[unit][physics][state][construction-manifest]") {
        ManifestFixture f;
        SECTION("shape") {
            f.bodies[0].shape = 99;
        }
        SECTION("first endpoint") {
            f.joints[0].first = 99;
        }
        SECTION("self endpoint") {
            f.joints[0].second = 8;
        }
        SECTION("missing second endpoint") {
            f.joints[0].second = 99;
        }
        CHECK(f.Validate().HasError());
    }

    TEST_CASE("Constructor manifest rejects inverse mass inertia and original COM corruption",
              "[unit][physics][state][construction-manifest]") {
        ManifestFixture f;
        SECTION("mass") {
            f.bodies[0].inverseMass = -1;
        }
        SECTION("inertia") {
            f.bodies[0].inverseInertia[1] = std::numeric_limits<float>::infinity();
        }
        SECTION("rotation") {
            f.bodies[0].inertiaRotation = {0, 0, 0, 0};
        }
        SECTION("COM rotation") {
            f.joints[0].firstCom.rotation = {0, 0, 0, 0};
        }
        SECTION("constructor FP") {
            f.joints[0].floatingPoint.value = 1;
        }
        CHECK(f.Validate().HasError());
    }

    TEST_CASE("Constructor manifest labels historical velocity omission explicitly", "[unit][physics][state][construction-manifest]") {
        ManifestFixture f;
        f.bodies[0].policy.initialLinearVelocity = {3, 4, 5};
        CHECK(f.Validate().HasError());
        f.bodies[0].policy.initialLinearVelocity = {};
        f.bodies[0].policy.initialAngularVelocity = {0, 2, 0};
        CHECK(f.Validate().HasError());
    }

    TEST_CASE("Constructor manifest uses same canonical constraint parameter admission", "[unit][physics][state][construction-manifest]") {
        ManifestFixture f;
        SECTION("distance") {
            f.joints[0].parameters = PhysicsDistanceConstraint{-1, 2};
        }
        SECTION("hinge") {
            f.joints[0].parameters = PhysicsHingeConstraint{1, 2};
        }
        SECTION("slider") {
            f.joints[0].parameters = PhysicsSliderConstraint{1, 2};
        }
        SECTION("policy") {
            f.joints[0].collisionPolicy = static_cast<PhysicsJointCollisionPolicy>(7);
        }
        CHECK(f.Validate().HasError());
    }

    TEST_CASE("Constructor manifest requires exact allocated motion and omitted inactive inertia",
              "[unit][physics][state][construction-manifest]") {
        ManifestFixture f;
        auto &body = f.bodies[0];
        body.policy = {};
        body.hasMotion = true;
        body.motionStorageReserved = true;
        body.inverseInertia = {};
        body.inertiaRotation = {};
        REQUIRE(f.Validate().HasValue());
        body.inverseInertia[0] = 1;
        CHECK(f.Validate().HasError());
    }

    TEST_CASE("Full construction budget accounts row payload and actual validation scratch",
              "[unit][physics][state][construction-manifest]") {
        ManifestFixture f;
        f.limits.maximumRetainedBytes = sizeof(PhysicsStateShapeRecipe) + sizeof(PhysicsStateShapeRecipeRoot) +
                                        sizeof(PhysicsStateConstructionBody) + sizeof(PhysicsStateConstructionJoint);
        CHECK(f.Validate().HasError());
        f.limits.maximumRetainedBytes += sizeof(std::uint32_t) * f.scratch.size();
        CHECK(f.Validate().HasValue());
    }

    TEST_CASE("Constructor manifest rejects static-only geometry motion before any native constructor",
              "[unit][physics][state][construction-manifest]") {
        ManifestFixture fixture;
        fixture.nodes[0].geometry = PhysicsShapeDescriptor{PhysicsStaticPlaneShape{}};
        CHECK(fixture.Validate().HasError());
        auto &body = fixture.bodies[0];
        body.policy = {};
        body.motionStorageReserved = false;
        body.hasMotion = false;
        body.inverseMass = 0;
        body.inverseInertia = {};
        body.inertiaRotation = {};
        REQUIRE(fixture.Validate().HasValue());
        body.motionStorageReserved = true;
        CHECK(fixture.Validate().HasError());
    }

    TEST_CASE("World-anchor construction metadata cannot invent ignored singleton state or disagree with the actual frame",
              "[unit][physics][state][construction-manifest]") {
        ManifestFixture fixture;
        SECTION("fixed COM") {
            fixture.joints[0].secondCom.position[0] = 1;
        }
        SECTION("fixed rotation") {
            fixture.joints[0].secondCom.rotation = {0, 0, 1, 0};
        }
        SECTION("world frame") {
            fixture.joints[0].secondConstructorFrame.translation.x = 1;
        }
        SECTION("world frame signed zero") {
            fixture.joints[0].secondConstructorFrame.translation.x = -0.0F;
        }
        CHECK(fixture.Validate().HasError());
    }
}  // namespace Horo::Physics
