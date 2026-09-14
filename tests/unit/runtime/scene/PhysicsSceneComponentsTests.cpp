#include "Horo/Runtime/Scene/PhysicsSceneComponents.h"
#include "Horo/Runtime/Scene/RuntimeSceneDefinition.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
    using namespace Horo;

    [[nodiscard]] Assets::AssetId Asset(const std::uint8_t suffix) {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = suffix;
        return Assets::AssetId::FromBytes(bytes);
    }

    [[nodiscard]] Physics::CollisionProfileId Profile(const std::uint8_t suffix = 1) {
        std::array<std::uint8_t, 16> bytes{};
        bytes.back() = suffix;
        return Physics::CollisionProfileId::FromBytes(bytes);
    }

    [[nodiscard]] Runtime::RigidBodyComponent Body(const std::uint64_t component = 1, const std::uint64_t slot = 10) {
        return {.id = {component}, .body = {slot}};
    }

    [[nodiscard]] Runtime::PhysicsBodyReference BodyReference(const std::uint64_t object, const std::uint64_t slot = 10) {
        return {.object = {object}, .body = {slot}};
    }

    [[nodiscard]] Runtime::ColliderComponent Collider(const std::uint64_t owner, const std::uint64_t component = 2,
                                                      const std::uint64_t slot = 20) {
        return {
            .id = {component},
            .collider = {slot},
            .body = BodyReference(owner),
            .collisionProfile = Profile(),
            .materials = {{.slot = Physics::PhysicsMaterialSlotId::FromValue(1), .material = Asset(2)}},
        };
    }

    [[nodiscard]] Runtime::PhysicsConstraintComponent Constraint(const std::uint64_t owner, const std::uint64_t other,
                                                                 const std::uint64_t component = 3, const std::uint64_t slot = 30) {
        return {
            .id = {component},
            .constraint = {slot},
            .first = {.body = BodyReference(owner)},
            .second = Runtime::PhysicsConstraintBodyEndpoint{.body = BodyReference(other)},
        };
    }

    TEST_CASE("Authored Physics Scene components validate typed schemas and bounded payloads", "[unit][physics][scene]") {
        auto body = Body();
        REQUIRE(Runtime::ValidateRigidBodyComponent(body).HasValue());
        body.schemaVersion = 2;
        REQUIRE(Runtime::ValidateRigidBodyComponent(body).HasError());
        body = Body();
        body.motion = Runtime::AuthoredPhysicsMotionType::Dynamic;
        body.mass = Runtime::AuthoredPhysicsNoMass{};
        REQUIRE(Runtime::ValidateRigidBodyComponent(body).HasError());

        auto collider = Collider(1);
        REQUIRE(Runtime::ValidateColliderComponent(collider).HasValue());
        collider.materials.push_back(collider.materials.front());
        REQUIRE(Runtime::ValidateColliderComponent(collider).HasError());
        collider = Collider(1);
        collider.scale.x = 0.0F;
        REQUIRE(Runtime::ValidateColliderComponent(collider).HasError());
        collider = Collider(1);
        collider.source = Runtime::PhysicsAnalyticCollider{Runtime::PhysicsSphereCollider{}};
        collider.scale.y += Math::DefaultEpsilon * 0.5F;
        REQUIRE(Runtime::ValidateColliderComponent(collider).HasValue());
        collider = Collider(1);
        collider.source = Runtime::PhysicsShapeAssetReference{.asset = Asset(4), .subresource = {7}};
        REQUIRE(Runtime::ValidateColliderComponent(collider).HasValue());
        std::get<Runtime::PhysicsShapeAssetReference>(collider.source).subresource = {};
        REQUIRE(Runtime::ValidateColliderComponent(collider).HasError());

        auto constraint = Constraint(1, 2);
        REQUIRE(Runtime::ValidatePhysicsConstraintComponent(constraint).HasValue());
        std::get<Runtime::PhysicsConstraintBodyEndpoint>(constraint.second).body = constraint.first.body;
        REQUIRE(Runtime::ValidatePhysicsConstraintComponent(constraint).HasError());
        constraint = Constraint(1, 2);
        constraint.parameters = Runtime::PhysicsDistanceConstraint{.minimumMeters = 2.0F, .maximumMeters = 1.0F};
        REQUIRE(Runtime::ValidatePhysicsConstraintComponent(constraint).HasError());
    }

    TEST_CASE("Physics Scene validation rejects implicit bodies and incompatible shape motion", "[unit][physics][scene]") {
        const auto firstBody = Body();
        const auto firstCollider = Collider(1);
        const auto secondBody = Body(1, 10);
        const auto secondCollider = Collider(2);
        const auto link = Constraint(1, 2);
        const std::array views{
            Runtime::PhysicsSceneComponentView{.owner = {1},
                                               .rigidBody = &firstBody,
                                               .colliders = {&firstCollider, 1},
                                               .constraints = {&link, 1}},
            Runtime::PhysicsSceneComponentView{.owner = {2}, .rigidBody = &secondBody, .colliders = {&secondCollider, 1}},
        };
        REQUIRE(Runtime::ValidatePhysicsSceneComponentViews(views).HasValue());

        const std::array missingBody{
            Runtime::PhysicsSceneComponentView{.owner = {1}, .colliders = {&firstCollider, 1}},
        };
        REQUIRE(Runtime::ValidatePhysicsSceneComponentViews(missingBody).HasError());

        auto planeCollider = Collider(1);
        planeCollider.source = Runtime::PhysicsAnalyticCollider{Runtime::PhysicsStaticPlaneCollider{}};
        auto dynamicBody = Body();
        dynamicBody.motion = Runtime::AuthoredPhysicsMotionType::Dynamic;
        dynamicBody.mass = Runtime::AuthoredPhysicsMass{};
        const std::array incompatible{
            Runtime::PhysicsSceneComponentView{.owner = {1}, .rigidBody = &dynamicBody, .colliders = {&planeCollider, 1}},
        };
        REQUIRE(Runtime::ValidatePhysicsSceneComponentViews(incompatible).HasError());
    }

    TEST_CASE("Runtime Scene definitions retain stable Physics producer identities", "[unit][physics][scene]") {
        Runtime::SceneDefinitionBuilder builder{{4}, {9}};
        auto collider = Collider(1);
        builder.Add({.object = {1}, .components = {.rigidBody = Body(), .colliders = {collider}}});
        auto definition = std::move(builder).Build();
        REQUIRE(definition.HasValue());
        const auto &components = definition.Value().Entities().front().components;
        REQUIRE(components.rigidBody->body.value == 10);
        REQUIRE(components.colliders.front().collider.value == 20);
        REQUIRE(components.colliders.front().schemaVersion == Runtime::PhysicsSceneComponentSchemaVersion);

        Runtime::SceneDefinitionBuilder missingCollider{{4}, {10}};
        missingCollider.Add({.object = {1}, .components = {.rigidBody = Body()}});
        REQUIRE(std::move(missingCollider).Build().HasError());
    }
}  // namespace
