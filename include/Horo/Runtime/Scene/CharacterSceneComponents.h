#pragma once

/** @file CharacterSceneComponents.h
 * @brief Durable Character authoring values without backend handles or runtime generation evidence.
 */
#include "Horo/Physics/CharacterPolicies.h"
#include "Horo/Runtime/Scene/PhysicsSceneComponents.h"

#include <optional>

namespace Horo::Runtime {
    inline constexpr std::uint32_t CharacterSceneComponentSchemaVersion = 1; /**< Durable Character schema. */

    /** @brief Collision geometry and acceleration expressed in canonical meters and world-space basis. */
    struct CharacterSceneGeometry final {
        PhysicsCapsuleCollider capsule{0.5F, 0.5F};
        std::optional<PhysicsCapsuleCollider> crouchedCapsule;
        Math::Vec3 up{0.0F, 1.0F, 0.0F};
        Math::Vec3 gravity{0.0F, -9.81F, 0.0F};
        [[nodiscard]] constexpr bool operator==(const CharacterSceneGeometry &) const noexcept = default;
    };

    /** @brief Fixed-capacity movement and moving-platform policies, independent of the chosen backend. */
    struct CharacterSceneMovement final {
        float skinWidthMeters{0.02F};
        float minimumMoveDistanceMeters{0.001F};
        float maximumStepHeightMeters{0.3F};
        float maximumSlopeDegrees{45.0F};
        bool preserveHorizontalSpeed{true};
        Character::CharacterSteepSlopePolicy steepSlopePolicy{Character::CharacterSteepSlopePolicy::Stop};
        std::uint32_t maximumContacts{16};
        float jumpSpeedMetersPerSecond{5.0F};
        bool dynamicPlatformSupport{};
        bool inheritPlatformHeading{};
        Character::CharacterPlatformLeaveVelocityPolicy platformLeaveVelocity{Character::CharacterPlatformLeaveVelocityPolicy::None};
        [[nodiscard]] constexpr bool operator==(const CharacterSceneMovement &) const noexcept = default;
    };

    /** @brief Stable project and asset bindings; absent values express an unconfigured disabled component. */
    struct CharacterSceneCollision final {
        std::optional<Physics::CollisionProfileId> profile;
        std::optional<Physics::PhysicsQueryChannelId> queryChannel;
        std::optional<PhysicsColliderMaterialBinding> defaultMaterial;
        std::optional<Physics::CollisionLayerId> requiredLayer;
        std::optional<Physics::CollisionProfileId> requiredProfile;
        std::optional<PhysicsBodyReference> excludedBody;
        [[nodiscard]] constexpr bool operator==(const CharacterSceneCollision &) const noexcept = default;
    };

    /** @brief Persistent Character producer. Its collision root and heading come from the candidate Scene world pose.
     * @details Absent legacy data stays absent. Newly added components start disabled and unconfigured;
     * enabling requires explicit real project bindings. Runtime generations and native handles are never serialized.
     */
    struct CharacterControllerComponent final {
        PhysicsComponentId id;
        std::uint32_t schemaVersion{CharacterSceneComponentSchemaVersion};
        bool enabled{};
        CharacterSceneGeometry geometry;
        CharacterSceneMovement movement;
        CharacterSceneCollision collision;
        [[nodiscard]] constexpr bool operator==(const CharacterControllerComponent &) const noexcept = default;
    };

    /** @brief Validates inert geometry, policies, optional identities and enabled binding completeness.
     * @param component Owned authored value; no asset resolution or service access occurs.
     * @return Success or DescriptorInvalid without mutating the component.
     */
    [[nodiscard]] Result<void> ValidateCharacterControllerComponent(const CharacterControllerComponent &component);
}  // namespace Horo::Runtime
