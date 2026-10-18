#include "SceneSourceInternal.h"

namespace Horo::SceneSource::Detail {
    namespace {
        /** @brief Encodes optional project bindings as explicit absence rather than invalid synthetic IDs. */
        [[nodiscard]] WireJson CollisionJson(const Runtime::CharacterSceneCollision &collision) {
            WireJson value = WireJson::object();
            if (collision.profile)
                value["profile"] = collision.profile->ToString();
            if (collision.queryChannel)
                value["queryChannel"] = collision.queryChannel->ToString();
            if (collision.defaultMaterial)
                value["defaultMaterial"] = {{"asset", collision.defaultMaterial->material.ToString()},
                                            {"slot", collision.defaultMaterial->slot.Value()}};
            if (collision.requiredLayer)
                value["requiredLayer"] = collision.requiredLayer->ToString();
            if (collision.requiredProfile)
                value["requiredProfile"] = collision.requiredProfile->ToString();
            if (collision.excludedBody)
                value["excludedBody"] = {{"object", collision.excludedBody->object.value}, {"body", collision.excludedBody->body.value}};
            return value;
        }

        /** @brief Emits all motion policy fields so source round trips do not infer runtime defaults. */
        [[nodiscard]] WireJson MovementJson(const Runtime::CharacterSceneMovement &movement) {
            return {{"skinWidthMeters", movement.skinWidthMeters},
                    {"minimumMoveDistanceMeters", movement.minimumMoveDistanceMeters},
                    {"maximumStepHeightMeters", movement.maximumStepHeightMeters},
                    {"maximumSlopeDegrees", movement.maximumSlopeDegrees},
                    {"preserveHorizontalSpeed", movement.preserveHorizontalSpeed},
                    {"steepSlopePolicy", static_cast<unsigned>(movement.steepSlopePolicy)},
                    {"maximumContacts", movement.maximumContacts},
                    {"jumpSpeedMetersPerSecond", movement.jumpSpeedMetersPerSecond},
                    {"dynamicPlatformSupport", movement.dynamicPlatformSupport},
                    {"inheritPlatformHeading", movement.inheritPlatformHeading},
                    {"platformLeaveVelocity", static_cast<unsigned>(movement.platformLeaveVelocity)}};
        }

        /** @brief Writes canonical meter geometry without applying visual transform scale to collision dimensions. */
        [[nodiscard]] WireJson CapsuleJson(const Runtime::PhysicsCapsuleCollider &capsule) {
            return {{"radiusMeters", capsule.radiusMeters}, {"cylindricalHalfHeightMeters", capsule.cylindricalHalfHeightMeters}};
        }
    }  // namespace

    void AppendCharacterController(WireJson &value, const Runtime::CharacterControllerComponent &component) {
        WireJson geometry = {{"capsule", CapsuleJson(component.geometry.capsule)},
                             {"up", Vec3Json(component.geometry.up)},
                             {"gravity", Vec3Json(component.geometry.gravity)}};
        if (component.geometry.crouchedCapsule)
            geometry["crouchedCapsule"] = CapsuleJson(*component.geometry.crouchedCapsule);
        value["characterController"] = {{"id", component.id.value},
                                        {"schemaVersion", component.schemaVersion},
                                        {"enabled", component.enabled},
                                        {"geometry", std::move(geometry)},
                                        {"movement", MovementJson(component.movement)},
                                        {"collision", CollisionJson(component.collision)}};
    }
}  // namespace Horo::SceneSource::Detail
