#include "editor/document/SceneDocumentPersistenceInternal.h"

#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace Horo::Editor::ScenePersistenceDetail {
    [[nodiscard]] Result<Runtime::AuthoredPhysicsPose> ParsePhysicsPose(const Json &value) {
        if (!value.is_object() || !value.contains("translation") || !value.contains("rotation"))
            return Result<Runtime::AuthoredPhysicsPose>::Failure(PersistenceError(SceneInvalid, "Physics pose is incomplete."));
        auto translation = ParseVec3(value["translation"]);
        auto rotation = ParseQuaternion(value["rotation"]);
        if (translation.HasError() || rotation.HasError())
            return Result<Runtime::AuthoredPhysicsPose>::Failure(PersistenceError(SceneInvalid, "Physics pose is invalid."));
        return Result<Runtime::AuthoredPhysicsPose>::Success({translation.Value(), rotation.Value()});
    }

    [[nodiscard]] Result<Runtime::PhysicsBodyReference> ParsePhysicsBodyReference(const Json &value) {
        if (!value.is_object() || !value.contains("object") || !value["object"].is_number_unsigned() || !value.contains("body") ||
            !value["body"].is_number_unsigned())
            return Result<Runtime::PhysicsBodyReference>::Failure(PersistenceError(SceneInvalid, "Physics body reference is incomplete."));
        Runtime::PhysicsBodyReference reference{{value["object"].get<std::uint64_t>()}, {value["body"].get<std::uint64_t>()}};
        if (!reference.object.IsValid() || !reference.body.IsValid())
            return Result<Runtime::PhysicsBodyReference>::Failure(PersistenceError(SceneInvalid, "Physics body reference is invalid."));
        return Result<Runtime::PhysicsBodyReference>::Success(reference);
    }

    [[nodiscard]] Result<Runtime::AuthoredPhysicsMotionType> ParsePhysicsMotion(const std::string_view name) {
        using enum Runtime::AuthoredPhysicsMotionType;
        if (name == "static")
            return Result<Runtime::AuthoredPhysicsMotionType>::Success(Static);
        if (name == "kinematic")
            return Result<Runtime::AuthoredPhysicsMotionType>::Success(Kinematic);
        if (name == "dynamic")
            return Result<Runtime::AuthoredPhysicsMotionType>::Success(Dynamic);
        return Result<Runtime::AuthoredPhysicsMotionType>::Failure(PersistenceError(SceneInvalid, "Rigid body motion is invalid."));
    }

    [[nodiscard]] Result<Runtime::AuthoredPhysicsMassPolicy> ParsePhysicsMass(const Json &value) {
        const std::string kind = value.value("kind", "");
        if (kind == "none")
            return Result<Runtime::AuthoredPhysicsMassPolicy>::Success(Runtime::AuthoredPhysicsNoMass{});
        if (kind == "mass" && value.contains("kilograms") && value["kilograms"].is_number())
            return Result<Runtime::AuthoredPhysicsMassPolicy>::Success(Runtime::AuthoredPhysicsMass{value["kilograms"].get<float>()});
        if (kind == "density" && value.contains("kilogramsPerCubicMeter") && value["kilogramsPerCubicMeter"].is_number())
            return Result<Runtime::AuthoredPhysicsMassPolicy>::Success(
                Runtime::AuthoredPhysicsDensity{value["kilogramsPerCubicMeter"].get<float>()});
        return Result<Runtime::AuthoredPhysicsMassPolicy>::Failure(PersistenceError(SceneInvalid, "Rigid body mass policy is invalid."));
    }

    [[nodiscard]] Result<Runtime::RigidBodyComponent> ParseRigidBody(const Json &value) {
        if (!HasFields(value, {"motion", "mass"}) || !HasUnsignedFields(value, {"id", "body", "schemaVersion", "generation"}) ||
            !value["motion"].is_string() || !value["mass"].is_object())
            return Result<Runtime::RigidBodyComponent>::Failure(PersistenceError(SceneInvalid, "Rigid body schema is incomplete."));
        auto motion = ParsePhysicsMotion(value["motion"].get<std::string>());
        auto mass = ParsePhysicsMass(value["mass"]);
        auto linearVelocity = ParseVec3(value.value("initialLinearVelocity", Json::array({0.0F, 0.0F, 0.0F})));
        auto angularVelocity = ParseVec3(value.value("initialAngularVelocity", Json::array({0.0F, 0.0F, 0.0F})));
        if (!AllSucceeded({motion.HasValue(), mass.HasValue(), linearVelocity.HasValue(), angularVelocity.HasValue()}))
            return Result<Runtime::RigidBodyComponent>::Failure(PersistenceError(SceneInvalid, "Rigid body payload is invalid."));
        Runtime::RigidBodyComponent body{.id = {value["id"].get<std::uint64_t>()},
                                         .body = {value["body"].get<std::uint64_t>()},
                                         .schemaVersion = value["schemaVersion"].get<std::uint32_t>(),
                                         .generation = value["generation"].get<std::uint64_t>(),
                                         .motion = motion.Value(),
                                         .mass = std::move(mass).Value(),
                                         .initialLinearVelocity = linearVelocity.Value(),
                                         .initialAngularVelocity = angularVelocity.Value(),
                                         .linearDampingPerSecond = value.value("linearDampingPerSecond", 0.0F),
                                         .angularDampingPerSecond = value.value("angularDampingPerSecond", 0.0F),
                                         .maximumLinearSpeed = value.value("maximumLinearSpeed", 500.0F),
                                         .maximumAngularSpeed = value.value("maximumAngularSpeed", 100.0F),
                                         .enabled = value.value("enabled", true)};
        if (Runtime::ValidateRigidBodyComponent(body).HasError())
            return Result<Runtime::RigidBodyComponent>::Failure(PersistenceError(SceneInvalid, "Rigid body payload is invalid."));
        return Result<Runtime::RigidBodyComponent>::Success(std::move(body));
    }

    [[nodiscard]] Result<Runtime::PhysicsColliderSource> ParsePhysicsShapeAssetSource(const Json &value) {
        if (!HasFields(value, {"asset"}) || !HasUnsignedFields(value, {"subresource"}) || !value["asset"].is_string())
            return Result<Runtime::PhysicsColliderSource>::Failure(PersistenceError(SceneInvalid, "Collider asset is incomplete."));
        auto asset = Assets::AssetId::Parse(value["asset"].get<std::string>());
        if (asset.HasError())
            return Result<Runtime::PhysicsColliderSource>::Failure(PersistenceError(SceneInvalid, "Collider asset is invalid."));
        return Result<Runtime::PhysicsColliderSource>::Success(
            Runtime::PhysicsShapeAssetReference{asset.Value(), {value["subresource"].get<std::uint64_t>()}});
    }

    [[nodiscard]] Result<Runtime::PhysicsColliderSource> ParsePhysicsBoxSource(const Json &value) {
        auto halfExtents = ParseVec3(value.value("halfExtentsMeters", Json{}));
        if (halfExtents.HasError())
            return Result<Runtime::PhysicsColliderSource>::Failure(halfExtents.ErrorValue());
        return Result<Runtime::PhysicsColliderSource>::Success(
            Runtime::PhysicsAnalyticCollider{Runtime::PhysicsBoxCollider{halfExtents.Value()}});
    }

    [[nodiscard]] Result<Runtime::PhysicsColliderSource> ParsePhysicsPlaneSource(const Json &value) {
        if (!HasFields(value, {"normal", "signedDistanceMeters"}))
            return Result<Runtime::PhysicsColliderSource>::Failure(PersistenceError(SceneInvalid, "Static plane is incomplete."));
        auto normal = ParseVec3(value["normal"]);
        if (normal.HasError())
            return Result<Runtime::PhysicsColliderSource>::Failure(normal.ErrorValue());
        return Result<Runtime::PhysicsColliderSource>::Success(Runtime::PhysicsAnalyticCollider{
            Runtime::PhysicsStaticPlaneCollider{normal.Value(), value["signedDistanceMeters"].get<float>()}});
    }

    [[nodiscard]] Result<Runtime::PhysicsColliderSource> ParsePhysicsSphereSource(const Json &value) {
        if (!HasFields(value, {"radiusMeters"}))
            return Result<Runtime::PhysicsColliderSource>::Failure(PersistenceError(SceneInvalid, "Sphere collider is incomplete."));
        return Result<Runtime::PhysicsColliderSource>::Success(
            Runtime::PhysicsAnalyticCollider{Runtime::PhysicsSphereCollider{value["radiusMeters"].get<float>()}});
    }

    [[nodiscard]] Result<Runtime::PhysicsColliderSource> ParsePhysicsCapsuleSource(const Json &value) {
        if (!HasFields(value, {"radiusMeters", "cylindricalHalfHeightMeters"}))
            return Result<Runtime::PhysicsColliderSource>::Failure(PersistenceError(SceneInvalid, "Capsule collider is incomplete."));
        return Result<Runtime::PhysicsColliderSource>::Success(Runtime::PhysicsAnalyticCollider{
            Runtime::PhysicsCapsuleCollider{value["radiusMeters"].get<float>(), value["cylindricalHalfHeightMeters"].get<float>()}});
    }

    [[nodiscard]] Result<Runtime::PhysicsColliderSource> ParsePhysicsColliderSource(const Json &value) {
        if (!value.is_object() || !value.contains("kind") || !value["kind"].is_string())
            return Result<Runtime::PhysicsColliderSource>::Failure(PersistenceError(SceneInvalid, "Collider source is incomplete."));
        const std::string kind = value["kind"].get<std::string>();
        if (kind == "asset")
            return ParsePhysicsShapeAssetSource(value);
        if (kind == "box")
            return ParsePhysicsBoxSource(value);
        if (kind == "sphere")
            return ParsePhysicsSphereSource(value);
        if (kind == "capsule")
            return ParsePhysicsCapsuleSource(value);
        if (kind == "static_plane")
            return ParsePhysicsPlaneSource(value);
        return Result<Runtime::PhysicsColliderSource>::Failure(PersistenceError(SceneInvalid, "Collider source kind is invalid."));
    }

    [[nodiscard]] Result<std::vector<Runtime::PhysicsColliderMaterialBinding>> ParsePhysicsMaterials(const Json &value) {
        std::vector<Runtime::PhysicsColliderMaterialBinding> materials;
        materials.reserve(value.size());
        for (const Json &entry : value) {
            if (!HasFields(entry, {"material"}) || !HasUnsignedFields(entry, {"slot"}) || !entry["material"].is_string())
                return Result<std::vector<Runtime::PhysicsColliderMaterialBinding>>::Failure(
                    PersistenceError(SceneInvalid, "Collider material is incomplete."));
            auto material = Assets::AssetId::Parse(entry["material"].get<std::string>());
            if (material.HasError())
                return Result<std::vector<Runtime::PhysicsColliderMaterialBinding>>::Failure(
                    PersistenceError(SceneInvalid, "Collider material is invalid."));
            materials.emplace_back(Physics::PhysicsMaterialSlotId::FromValue(entry["slot"].get<std::uint64_t>()), material.Value());
        }
        return Result<std::vector<Runtime::PhysicsColliderMaterialBinding>>::Success(std::move(materials));
    }

    [[nodiscard]] Result<Runtime::ColliderComponent> ParseCollider(const Json &value) {
        if (!HasFields(value, {"body", "source", "localPose", "scale", "collisionProfile", "materials"}) ||
            !HasUnsignedFields(value, {"id", "collider", "schemaVersion", "generation"}) || !value["collisionProfile"].is_string() ||
            !value["materials"].is_array())
            return Result<Runtime::ColliderComponent>::Failure(PersistenceError(SceneInvalid, "Collider schema is incomplete."));
        auto body = ParsePhysicsBodyReference(value["body"]);
        auto source = ParsePhysicsColliderSource(value["source"]);
        auto pose = ParsePhysicsPose(value["localPose"]);
        auto scale = ParseVec3(value["scale"]);
        auto profile = Physics::CollisionProfileId::Parse(value["collisionProfile"].get<std::string>());
        if (!AllSucceeded({body.HasValue(), source.HasValue(), pose.HasValue(), scale.HasValue(), profile.HasValue()}))
            return Result<Runtime::ColliderComponent>::Failure(PersistenceError(SceneInvalid, "Collider payload is invalid."));
        auto materials = ParsePhysicsMaterials(value["materials"]);
        if (materials.HasError())
            return Result<Runtime::ColliderComponent>::Failure(materials.ErrorValue());
        Runtime::ColliderComponent collider{.id = {value["id"].get<std::uint64_t>()},
                                            .collider = {value["collider"].get<std::uint64_t>()},
                                            .schemaVersion = value["schemaVersion"].get<std::uint32_t>(),
                                            .generation = value["generation"].get<std::uint64_t>(),
                                            .body = body.Value(),
                                            .source = std::move(source).Value(),
                                            .localPose = pose.Value(),
                                            .scale = scale.Value(),
                                            .collisionProfile = profile.Value(),
                                            .materials = std::move(materials).Value(),
                                            .sensor = value.value("sensor", false),
                                            .enabled = value.value("enabled", true)};
        if (Runtime::ValidateColliderComponent(collider).HasError())
            return Result<Runtime::ColliderComponent>::Failure(PersistenceError(SceneInvalid, "Collider payload is invalid."));
        return Result<Runtime::ColliderComponent>::Success(std::move(collider));
    }

    [[nodiscard]] Result<std::vector<Runtime::ColliderComponent>> ParseColliders(const Json &value) {
        if (!value.is_array() || value.size() > Runtime::MaximumPhysicsCollidersPerBody)
            return Result<std::vector<Runtime::ColliderComponent>>::Failure(PersistenceError(SceneInvalid, "Collider list is invalid."));
        std::vector<Runtime::ColliderComponent> result;
        result.reserve(value.size());
        for (const Json &entry : value) {
            auto collider = ParseCollider(entry);
            if (collider.HasError())
                return Result<std::vector<Runtime::ColliderComponent>>::Failure(collider.ErrorValue());
            result.push_back(std::move(collider).Value());
        }
        return Result<std::vector<Runtime::ColliderComponent>>::Success(std::move(result));
    }

    [[nodiscard]] Result<Runtime::PhysicsConstraintBodyEndpoint> ParseConstraintBodyEndpoint(const Json &value) {
        if (!value.is_object() || !value.contains("body") || !value.contains("frame"))
            return Result<Runtime::PhysicsConstraintBodyEndpoint>::Failure(
                PersistenceError(SceneInvalid, "Constraint endpoint is incomplete."));
        auto body = ParsePhysicsBodyReference(value["body"]);
        auto frame = ParsePhysicsPose(value["frame"]);
        if (body.HasError() || frame.HasError())
            return Result<Runtime::PhysicsConstraintBodyEndpoint>::Failure(
                PersistenceError(SceneInvalid, "Constraint endpoint is invalid."));
        return Result<Runtime::PhysicsConstraintBodyEndpoint>::Success({body.Value(), frame.Value()});
    }

    [[nodiscard]] Result<Runtime::PhysicsConstraintSecondEndpoint> ParseConstraintSecondEndpoint(const Json &value) {
        const std::string kind = value.value("kind", "");
        if (kind == "body") {
            auto endpoint = ParseConstraintBodyEndpoint(value);
            if (endpoint.HasError())
                return Result<Runtime::PhysicsConstraintSecondEndpoint>::Failure(endpoint.ErrorValue());
            return Result<Runtime::PhysicsConstraintSecondEndpoint>::Success(endpoint.Value());
        }
        if (kind == "world" && value.contains("frame")) {
            auto frame = ParsePhysicsPose(value["frame"]);
            if (frame.HasError())
                return Result<Runtime::PhysicsConstraintSecondEndpoint>::Failure(frame.ErrorValue());
            return Result<Runtime::PhysicsConstraintSecondEndpoint>::Success(Runtime::PhysicsConstraintWorldEndpoint{frame.Value()});
        }
        return Result<Runtime::PhysicsConstraintSecondEndpoint>::Failure(
            PersistenceError(SceneInvalid, "Constraint second endpoint is invalid."));
    }

    [[nodiscard]] Result<std::variant<Runtime::PhysicsFixedConstraint, Runtime::PhysicsDistanceConstraint>> ParseConstraintParameters(
        const Json &value) {
        const std::string kind = value.value("kind", "");
        if (kind == "fixed")
            return Result<std::variant<Runtime::PhysicsFixedConstraint, Runtime::PhysicsDistanceConstraint>>::Success(
                Runtime::PhysicsFixedConstraint{});
        if (kind == "distance" && HasFields(value, {"minimumMeters", "maximumMeters"}))
            return Result<std::variant<Runtime::PhysicsFixedConstraint, Runtime::PhysicsDistanceConstraint>>::Success(
                Runtime::PhysicsDistanceConstraint{value["minimumMeters"].get<float>(), value["maximumMeters"].get<float>()});
        return Result<std::variant<Runtime::PhysicsFixedConstraint, Runtime::PhysicsDistanceConstraint>>::Failure(
            PersistenceError(SceneInvalid, "Constraint parameters are invalid."));
    }

    [[nodiscard]] Result<Runtime::PhysicsConstraintComponent> ParsePhysicsConstraint(const Json &value) {
        if (!HasFields(value, {"first", "second", "parameters"}) ||
            !HasUnsignedFields(value, {"id", "constraint", "schemaVersion", "generation"}) || !value["second"].is_object() ||
            !value["parameters"].is_object())
            return Result<Runtime::PhysicsConstraintComponent>::Failure(
                PersistenceError(SceneInvalid, "Physics constraint schema is incomplete."));
        auto first = ParseConstraintBodyEndpoint(value["first"]);
        auto second = ParseConstraintSecondEndpoint(value["second"]);
        auto parameters = ParseConstraintParameters(value["parameters"]);
        if (!AllSucceeded({first.HasValue(), second.HasValue(), parameters.HasValue()}))
            return Result<Runtime::PhysicsConstraintComponent>::Failure(
                PersistenceError(SceneInvalid, "Physics constraint payload is invalid."));
        Runtime::PhysicsConstraintComponent constraint{.id = {value["id"].get<std::uint64_t>()},
                                                       .constraint = {value["constraint"].get<std::uint64_t>()},
                                                       .schemaVersion = value["schemaVersion"].get<std::uint32_t>(),
                                                       .generation = value["generation"].get<std::uint64_t>(),
                                                       .first = first.Value(),
                                                       .second = std::move(second).Value(),
                                                       .parameters = std::move(parameters).Value(),
                                                       .enabled = value.value("enabled", true)};
        if (Runtime::ValidatePhysicsConstraintComponent(constraint).HasError())
            return Result<Runtime::PhysicsConstraintComponent>::Failure(
                PersistenceError(SceneInvalid, "Physics constraint payload is invalid."));
        return Result<Runtime::PhysicsConstraintComponent>::Success(std::move(constraint));
    }

    [[nodiscard]] Result<std::vector<Runtime::PhysicsConstraintComponent>> ParsePhysicsConstraints(const Json &value) {
        if (!value.is_array() || value.size() > Runtime::MaximumPhysicsConstraintsPerObject)
            return Result<std::vector<Runtime::PhysicsConstraintComponent>>::Failure(
                PersistenceError(SceneInvalid, "Physics constraint list is invalid."));
        std::vector<Runtime::PhysicsConstraintComponent> result;
        result.reserve(value.size());
        for (const Json &entry : value) {
            auto constraint = ParsePhysicsConstraint(entry);
            if (constraint.HasError())
                return Result<std::vector<Runtime::PhysicsConstraintComponent>>::Failure(constraint.ErrorValue());
            result.push_back(std::move(constraint).Value());
        }
        return Result<std::vector<Runtime::PhysicsConstraintComponent>>::Success(std::move(result));
    }

    [[nodiscard]] Result<Gameplay::BehaviorComponent> ParseSingleBehavior(const Json &behavior) {
        if (!behavior.is_object() || !behavior.contains("instanceId") || !behavior["instanceId"].is_number_unsigned() ||
            !behavior.contains("typeId") || !behavior["typeId"].is_string() || !behavior.contains("schemaVersion") ||
            !behavior["schemaVersion"].is_number_unsigned() || !behavior.contains("enabled") || !behavior["enabled"].is_boolean() ||
            !behavior.contains("fields") || !behavior["fields"].is_array()) {
            return Result<Gameplay::BehaviorComponent>::Failure(PersistenceError(SceneInvalid, "Behavior entry is invalid."));
        }
        auto typeId = Gameplay::BehaviorTypeId::Parse(behavior["typeId"].get<std::string>());
        if (typeId.HasError()) {
            return Result<Gameplay::BehaviorComponent>::Failure(PersistenceError(SceneInvalid, "Behavior type ID is invalid."));
        }
        Gameplay::BehaviorComponent parsed{
            .instanceId = Gameplay::BehaviorInstanceId{behavior["instanceId"].get<std::uint64_t>()},
            .typeId = std::move(typeId).Value(),
            .schemaVersion = behavior["schemaVersion"].get<std::uint32_t>(),
            .enabled = behavior["enabled"].get<bool>(),
        };
        parsed.fields.reserve(behavior["fields"].size());
        for (const Json &fieldEntry : behavior["fields"]) {
            if (!fieldEntry.is_object() || !fieldEntry.contains("name") || !fieldEntry["name"].is_string() ||
                !fieldEntry.contains("value")) {
                return Result<Gameplay::BehaviorComponent>::Failure(PersistenceError(SceneInvalid, "Behavior field entry is invalid."));
            }
            auto field = ParseBehaviorFieldValue(fieldEntry["value"]);
            if (field.HasError()) {
                return Result<Gameplay::BehaviorComponent>::Failure(field.ErrorValue());
            }
            parsed.fields.emplace_back(fieldEntry["name"].get<std::string>(), std::move(field).Value());
        }
        if (Gameplay::ValidateBehaviorComponent(parsed).HasError()) {
            return Result<Gameplay::BehaviorComponent>::Failure(PersistenceError(SceneInvalid, "Behavior payload is invalid."));
        }
        return Result<Gameplay::BehaviorComponent>::Success(std::move(parsed));
    }

    [[nodiscard]] Result<std::vector<Gameplay::BehaviorComponent>> ParseBehaviors(const Json &behaviors) {
        if (!behaviors.is_array() || behaviors.size() > 128) {
            return Result<std::vector<Gameplay::BehaviorComponent>>::Failure(PersistenceError(SceneInvalid, "Behavior list is invalid."));
        }
        std::vector<Gameplay::BehaviorComponent> parsedList;
        parsedList.reserve(behaviors.size());
        for (const Json &behavior : behaviors) {
            auto parsed = ParseSingleBehavior(behavior);
            if (parsed.HasError()) {
                return Result<std::vector<Gameplay::BehaviorComponent>>::Failure(parsed.ErrorValue());
            }
            parsedList.push_back(std::move(parsed).Value());
        }
        return Result<std::vector<Gameplay::BehaviorComponent>>::Success(std::move(parsedList));
    }

}  // namespace Horo::Editor::ScenePersistenceDetail
