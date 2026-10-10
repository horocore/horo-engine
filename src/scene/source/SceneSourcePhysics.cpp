#include "SceneSourceInternal.h"

#include <utility>
#include <variant>

namespace Horo::SceneSource::Detail {
    [[nodiscard]] WireJson PhysicsPoseJson(const Runtime::AuthoredPhysicsPose &pose) {
        return {{"translation", Vec3Json(pose.translation)},
                {"rotation", WireJson::array({pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w})}};
    }

    [[nodiscard]] WireJson PhysicsBodyReferenceJson(const Runtime::PhysicsBodyReference &reference) {
        return {{"object", reference.object.value}, {"body", reference.body.value}};
    }

    [[nodiscard]] const char *PhysicsMotionName(const Runtime::AuthoredPhysicsMotionType motion) noexcept {
        using enum Runtime::AuthoredPhysicsMotionType;
        switch (motion) {
            case Static:
                return "static";
            case Kinematic:
                return "kinematic";
            case Dynamic:
                return "dynamic";
            case Count:
                break;
        }
        return "dynamic";
    }

    [[nodiscard]] WireJson PhysicsMassJson(const Runtime::AuthoredPhysicsMassPolicy &policy) {
        return std::visit([]<typename Mass>(const Mass &mass) -> WireJson {
            if constexpr (std::is_same_v<Mass, Runtime::AuthoredPhysicsNoMass>)
                return {{"kind", "none"}};
            else if constexpr (std::is_same_v<Mass, Runtime::AuthoredPhysicsMass>)
                return {{"kind", "mass"}, {"kilograms", mass.kilograms}};
            else
                return {{"kind", "density"}, {"kilogramsPerCubicMeter", mass.kilogramsPerCubicMeter}};
        }, policy);
    }

    [[nodiscard]] WireJson PhysicsColliderSourceJson(const Runtime::PhysicsColliderSource &source) {
        if (const auto *asset = std::get_if<Runtime::PhysicsShapeAssetReference>(&source))
            return {{"kind", "asset"}, {"asset", asset->asset.ToString()}, {"subresource", asset->subresource.value}};
        const Runtime::PhysicsAnalyticCollider &analytic = std::get<Runtime::PhysicsAnalyticCollider>(source);
        if (const auto *box = std::get_if<Runtime::PhysicsBoxCollider>(&analytic))
            return {{"kind", "box"}, {"halfExtentsMeters", Vec3Json(box->halfExtentsMeters)}};
        if (const auto *sphere = std::get_if<Runtime::PhysicsSphereCollider>(&analytic))
            return {{"kind", "sphere"}, {"radiusMeters", sphere->radiusMeters}};
        if (const auto *capsule = std::get_if<Runtime::PhysicsCapsuleCollider>(&analytic))
            return {{"kind", "capsule"},
                    {"radiusMeters", capsule->radiusMeters},
                    {"cylindricalHalfHeightMeters", capsule->cylindricalHalfHeightMeters}};
        const auto &plane = std::get<Runtime::PhysicsStaticPlaneCollider>(analytic);
        return {{"kind", "static_plane"}, {"normal", Vec3Json(plane.normal)}, {"signedDistanceMeters", plane.signedDistanceMeters}};
    }

    void AppendRigidBody(WireJson &value, const Runtime::RigidBodyComponent &body) {
        value["rigidBody"] = {{"id", body.id.value},
                              {"body", body.body.value},
                              {"schemaVersion", body.schemaVersion},
                              {"generation", body.generation},
                              {"motion", PhysicsMotionName(body.motion)},
                              {"mass", PhysicsMassJson(body.mass)},
                              {"initialLinearVelocity", Vec3Json(body.initialLinearVelocity)},
                              {"initialAngularVelocity", Vec3Json(body.initialAngularVelocity)},
                              {"linearDampingPerSecond", body.linearDampingPerSecond},
                              {"angularDampingPerSecond", body.angularDampingPerSecond},
                              {"maximumLinearSpeed", body.maximumLinearSpeed},
                              {"maximumAngularSpeed", body.maximumAngularSpeed},
                              {"enabled", body.enabled}};
    }

    void AppendColliders(WireJson &value, const std::vector<Runtime::ColliderComponent> &components) {
        WireJson colliders = WireJson::array();
        for (const Runtime::ColliderComponent &collider : components) {
            WireJson materials = WireJson::array();
            for (const Runtime::PhysicsColliderMaterialBinding &binding : collider.materials)
                materials.push_back({{"slot", binding.slot.Value()}, {"material", binding.material.ToString()}});
            colliders.push_back({{"id", collider.id.value},
                                 {"collider", collider.collider.value},
                                 {"schemaVersion", collider.schemaVersion},
                                 {"generation", collider.generation},
                                 {"body", PhysicsBodyReferenceJson(collider.body)},
                                 {"source", PhysicsColliderSourceJson(collider.source)},
                                 {"localPose", PhysicsPoseJson(collider.localPose)},
                                 {"scale", Vec3Json(collider.scale)},
                                 {"collisionProfile", collider.collisionProfile.ToString()},
                                 {"materials", std::move(materials)},
                                 {"sensor", collider.sensor},
                                 {"enabled", collider.enabled}});
        }
        if (!colliders.empty())
            value["colliders"] = std::move(colliders);
    }

    [[nodiscard]] WireJson PhysicsConstraintEndpointJson(const Runtime::PhysicsConstraintSecondEndpoint &endpoint) {
        if (const auto *body = std::get_if<Runtime::PhysicsConstraintBodyEndpoint>(&endpoint))
            return {{"kind", "body"}, {"body", PhysicsBodyReferenceJson(body->body)}, {"frame", PhysicsPoseJson(body->localFrame)}};
        return {{"kind", "world"}, {"frame", PhysicsPoseJson(std::get<Runtime::PhysicsConstraintWorldEndpoint>(endpoint).frame)}};
    }

    [[nodiscard]] WireJson PhysicsConstraintParametersJson(
        const std::variant<Runtime::PhysicsFixedConstraint, Runtime::PhysicsDistanceConstraint> &parameters) {
        if (const auto *distance = std::get_if<Runtime::PhysicsDistanceConstraint>(&parameters))
            return {{"kind", "distance"}, {"minimumMeters", distance->minimumMeters}, {"maximumMeters", distance->maximumMeters}};
        return {{"kind", "fixed"}};
    }

    void AppendPhysicsConstraints(WireJson &value, const std::vector<Runtime::PhysicsConstraintComponent> &components) {
        WireJson constraints = WireJson::array();
        for (const Runtime::PhysicsConstraintComponent &constraint : components) {
            constraints.push_back(
                {{"id", constraint.id.value},
                 {"constraint", constraint.constraint.value},
                 {"schemaVersion", constraint.schemaVersion},
                 {"generation", constraint.generation},
                 {"first",
                  {{"body", PhysicsBodyReferenceJson(constraint.first.body)}, {"frame", PhysicsPoseJson(constraint.first.localFrame)}}},
                 {"second", PhysicsConstraintEndpointJson(constraint.second)},
                 {"parameters", PhysicsConstraintParametersJson(constraint.parameters)},
                 {"enabled", constraint.enabled}});
        }
        if (!constraints.empty())
            value["physicsConstraints"] = std::move(constraints);
    }

    void AppendPhysicsComponents(WireJson &value, const SceneObjectComponentSet &components) {
        if (components.rigidBody)
            AppendRigidBody(value, *components.rigidBody);
        AppendColliders(value, components.colliders);
        AppendPhysicsConstraints(value, components.physicsConstraints);
    }

}  // namespace Horo::SceneSource::Detail
