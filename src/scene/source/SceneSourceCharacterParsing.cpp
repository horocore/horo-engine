#include "SceneSourceInternal.h"

namespace Horo::SceneSource::Detail {
    namespace {
        template <typename T> [[nodiscard]] Result<T> InvalidCharacter() {
            return Result<T>::Failure(PersistenceError(SceneInvalid, "Character component schema or binding is invalid."));
        }

        /** @brief Reads an optional stable typed project UUID without fabricating a default. */
        template <typename T> [[nodiscard]] Result<void> ParseIdentity(const Json &value, const char *name, std::optional<T> &destination) {
            if (!value.contains(name))
                return Result<void>::Success();
            if (!value[name].is_string())
                return InvalidCharacter<void>();
            auto parsed = T::Parse(value[name].get<std::string>());
            if (parsed.HasError())
                return InvalidCharacter<void>();
            destination = std::move(parsed).Value();
            return Result<void>::Success();
        }

        /** @brief Rejects nonnumeric and incomplete capsule records before conversion. */
        [[nodiscard]] Result<Runtime::PhysicsCapsuleCollider> ParseCapsule(const Json &value) {
            if (!HasFields(value, {"radiusMeters", "cylindricalHalfHeightMeters"}) || !value["radiusMeters"].is_number() ||
                !value["cylindricalHalfHeightMeters"].is_number())
                return InvalidCharacter<Runtime::PhysicsCapsuleCollider>();
            return Result<Runtime::PhysicsCapsuleCollider>::Success(
                {value["radiusMeters"].get<float>(), value["cylindricalHalfHeightMeters"].get<float>()});
        }

        /** @brief Parses optional material and exact authored body selectors without retaining JSON storage. */
        [[nodiscard]] Result<void> ParseCollision(const Json &value, Runtime::CharacterSceneCollision &collision) {
            if (!value.is_object())
                return InvalidCharacter<void>();
            if (auto result = ParseIdentity(value, "profile", collision.profile); result.HasError())
                return result;
            if (auto result = ParseIdentity(value, "queryChannel", collision.queryChannel); result.HasError())
                return result;
            if (auto result = ParseIdentity(value, "requiredLayer", collision.requiredLayer); result.HasError())
                return result;
            if (auto result = ParseIdentity(value, "requiredProfile", collision.requiredProfile); result.HasError())
                return result;
            if (value.contains("defaultMaterial")) {
                const auto &material = value["defaultMaterial"];
                if (!HasFields(material, {"asset"}) || !material["asset"].is_string() || !HasUnsignedFields(material, {"slot"}))
                    return InvalidCharacter<void>();
                auto asset = Assets::AssetId::Parse(material["asset"].get<std::string>());
                if (asset.HasError())
                    return InvalidCharacter<void>();
                collision.defaultMaterial = Runtime::PhysicsColliderMaterialBinding{Physics::PhysicsMaterialSlotId::FromValue(
                                                                                        material["slot"].get<std::uint64_t>()),
                                                                                    asset.Value()};
            }
            if (value.contains("excludedBody")) {
                const auto &body = value["excludedBody"];
                if (!HasUnsignedFields(body, {"object", "body"}))
                    return InvalidCharacter<void>();
                collision.excludedBody =
                    Runtime::PhysicsBodyReference{{body["object"].get<std::uint64_t>()}, {body["body"].get<std::uint64_t>()}};
            }
            return Result<void>::Success();
        }

        /** @brief Reads every movement scalar with explicit wire types and bounded enum values. */
        [[nodiscard]] Result<Runtime::CharacterSceneMovement> ParseMovement(const Json &value) {
            if (!HasFields(value,
                           {"skinWidthMeters", "minimumMoveDistanceMeters", "maximumStepHeightMeters", "maximumSlopeDegrees",
                            "preserveHorizontalSpeed", "jumpSpeedMetersPerSecond", "dynamicPlatformSupport", "inheritPlatformHeading"}) ||
                !HasUnsignedFields(value, {"steepSlopePolicy", "maximumContacts", "platformLeaveVelocity"}))
                return InvalidCharacter<Runtime::CharacterSceneMovement>();
            for (const char *name : {"skinWidthMeters", "minimumMoveDistanceMeters", "maximumStepHeightMeters", "maximumSlopeDegrees",
                                     "jumpSpeedMetersPerSecond"})
                if (!value[name].is_number())
                    return InvalidCharacter<Runtime::CharacterSceneMovement>();
            for (const char *name : {"preserveHorizontalSpeed", "dynamicPlatformSupport", "inheritPlatformHeading"})
                if (!value[name].is_boolean())
                    return InvalidCharacter<Runtime::CharacterSceneMovement>();
            const auto slope = value["steepSlopePolicy"].get<std::uint64_t>();
            const auto leave = value["platformLeaveVelocity"].get<std::uint64_t>();
            const auto contacts = value["maximumContacts"].get<std::uint64_t>();
            if (slope > 1 || leave > 2 || contacts > Character::MaximumCharacterContacts)
                return InvalidCharacter<Runtime::CharacterSceneMovement>();
            return Result<Runtime::CharacterSceneMovement>::Success(
                {value["skinWidthMeters"].get<float>(), value["minimumMoveDistanceMeters"].get<float>(),
                 value["maximumStepHeightMeters"].get<float>(), value["maximumSlopeDegrees"].get<float>(),
                 value["preserveHorizontalSpeed"].get<bool>(), static_cast<Character::CharacterSteepSlopePolicy>(slope),
                 static_cast<std::uint32_t>(contacts), value["jumpSpeedMetersPerSecond"].get<float>(),
                 value["dynamicPlatformSupport"].get<bool>(), value["inheritPlatformHeading"].get<bool>(),
                 static_cast<Character::CharacterPlatformLeaveVelocityPolicy>(leave)});
        }

        /** @brief Reads standing and optional crouched geometry and finite basis vectors. */
        [[nodiscard]] Result<Runtime::CharacterSceneGeometry> ParseGeometry(const Json &value) {
            if (!HasFields(value, {"capsule", "up", "gravity"}))
                return InvalidCharacter<Runtime::CharacterSceneGeometry>();
            auto capsule = ParseCapsule(value["capsule"]);
            auto up = ParseVec3(value["up"]);
            auto gravity = ParseVec3(value["gravity"]);
            if (capsule.HasError() || up.HasError() || gravity.HasError())
                return InvalidCharacter<Runtime::CharacterSceneGeometry>();
            Runtime::CharacterSceneGeometry geometry{.capsule = capsule.Value(), .up = up.Value(), .gravity = gravity.Value()};
            if (value.contains("crouchedCapsule")) {
                auto crouched = ParseCapsule(value["crouchedCapsule"]);
                if (crouched.HasError())
                    return InvalidCharacter<Runtime::CharacterSceneGeometry>();
                geometry.crouchedCapsule = crouched.Value();
            }
            return Result<Runtime::CharacterSceneGeometry>::Success(geometry);
        }
    }  // namespace

    Result<Runtime::CharacterControllerComponent> ParseCharacterController(const Json &value) {
        if (!HasUnsignedFields(value, {"id", "schemaVersion"}) || !HasFields(value, {"enabled", "geometry", "movement", "collision"}) ||
            !value["enabled"].is_boolean() || value["schemaVersion"].get<std::uint64_t>() != Runtime::CharacterSceneComponentSchemaVersion)
            return InvalidCharacter<Runtime::CharacterControllerComponent>();
        auto geometry = ParseGeometry(value["geometry"]);
        auto movement = ParseMovement(value["movement"]);
        if (geometry.HasError() || movement.HasError())
            return InvalidCharacter<Runtime::CharacterControllerComponent>();
        Runtime::CharacterControllerComponent component{.id = {value["id"].get<std::uint64_t>()},
                                                        .enabled = value["enabled"].get<bool>(),
                                                        .geometry = geometry.Value(),
                                                        .movement = movement.Value()};
        if (ParseCollision(value["collision"], component.collision).HasError() ||
            Runtime::ValidateCharacterControllerComponent(component).HasError())
            return InvalidCharacter<Runtime::CharacterControllerComponent>();
        return Result<Runtime::CharacterControllerComponent>::Success(std::move(component));
    }
}  // namespace Horo::SceneSource::Detail
