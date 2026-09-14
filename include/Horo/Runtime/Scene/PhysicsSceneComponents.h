#pragma once

/**
 * @file PhysicsSceneComponents.h
 * @brief Backend-neutral serializable rigid-body, collider, and constraint Scene component values.
 */

#include "Horo/Assets/AssetId.h"
#include "Horo/Foundation/Result.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Physics/PhysicsFilterIdentity.h"
#include "Horo/Runtime/Scene/SceneIdentity.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

namespace Horo::Runtime {
    inline constexpr std::uint32_t PhysicsSceneComponentSchemaVersion = 1;       /**< Current durable authored schema version. */
    inline constexpr std::size_t MaximumPhysicsCollidersPerBody = 64;            /**< Maximum contributors to one body. */
    inline constexpr std::size_t MaximumPhysicsColliderMaterialBindings = 4'096; /**< Maximum authored slot mappings. */
    inline constexpr std::size_t MaximumPhysicsConstraintsPerObject = 4'096;     /**< Maximum constraints authored by one object. */

    /** @brief Non-interchangeable persistent scalar identity used by an authored Physics component schema. */
    template <typename Tag> struct AuthoredPhysicsId final {
        std::uint64_t value{};

        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const AuthoredPhysicsId &) const noexcept = default;
    };

    struct PhysicsComponentIdentityTag;
    struct PhysicsBodySlotIdentityTag;
    struct PhysicsColliderSlotIdentityTag;
    struct PhysicsConstraintSlotIdentityTag;
    struct PhysicsShapeSubresourceIdentityTag;
    using PhysicsComponentId = AuthoredPhysicsId<PhysicsComponentIdentityTag>;
    using PhysicsBodySlotId = AuthoredPhysicsId<PhysicsBodySlotIdentityTag>;
    using PhysicsColliderSlotId = AuthoredPhysicsId<PhysicsColliderSlotIdentityTag>;
    using PhysicsConstraintSlotId = AuthoredPhysicsId<PhysicsConstraintSlotIdentityTag>;
    using PhysicsShapeSubresourceId = AuthoredPhysicsId<PhysicsShapeSubresourceIdentityTag>;

    /** @brief Owned scale-free pose in body-local or explicit world space. */
    struct AuthoredPhysicsPose final {
        Math::Vec3 translation{};
        Math::Quaternion rotation{};
        [[nodiscard]] constexpr bool operator==(const AuthoredPhysicsPose &) const noexcept = default;
    };

    /** @brief Exact authored body reference; never resolves by hierarchy, display name, or runtime handle. */
    struct PhysicsBodyReference final {
        SceneObjectId object;
        PhysicsBodySlotId body;
        [[nodiscard]] constexpr auto operator<=>(const PhysicsBodyReference &) const noexcept = default;
    };

    /** @brief Durable authored body motion policy independent of a native solver. */
    enum class AuthoredPhysicsMotionType : std::uint8_t {
        Static,
        Kinematic,
        Dynamic,
        Count
    };

    /** @brief Explicit absence of mass for static and kinematic bodies. */
    struct AuthoredPhysicsNoMass final {
        [[nodiscard]] constexpr bool operator==(const AuthoredPhysicsNoMass &) const noexcept = default;
    };

    /** @brief Explicit dynamic-body mass in canonical kilograms. */
    struct AuthoredPhysicsMass final {
        float kilograms{1.0F};
        [[nodiscard]] constexpr bool operator==(const AuthoredPhysicsMass &) const noexcept = default;
    };

    /** @brief Dynamic-body density used to derive mass from resolved geometry. */
    struct AuthoredPhysicsDensity final {
        float kilogramsPerCubicMeter{1'000.0F};
        [[nodiscard]] constexpr bool operator==(const AuthoredPhysicsDensity &) const noexcept = default;
    };

    using AuthoredPhysicsMassPolicy = std::variant<AuthoredPhysicsNoMass, AuthoredPhysicsMass, AuthoredPhysicsDensity>;

    /** @brief Serializable rigid-body producer with stable component and body-slot identities. */
    struct RigidBodyComponent final {
        PhysicsComponentId id;
        PhysicsBodySlotId body;
        std::uint32_t schemaVersion{PhysicsSceneComponentSchemaVersion};
        std::uint64_t generation{1};
        AuthoredPhysicsMotionType motion{AuthoredPhysicsMotionType::Static};
        AuthoredPhysicsMassPolicy mass;
        Math::Vec3 initialLinearVelocity{};
        Math::Vec3 initialAngularVelocity{};
        float linearDampingPerSecond{};
        float angularDampingPerSecond{};
        float maximumLinearSpeed{500.0F};
        float maximumAngularSpeed{100.0F};
        bool enabled{true};
        [[nodiscard]] bool operator==(const RigidBodyComponent &) const noexcept = default;
    };

    /** @brief Body-local analytic box geometry. */
    struct PhysicsBoxCollider final {
        Math::Vec3 halfExtentsMeters{0.5F, 0.5F, 0.5F};
        [[nodiscard]] constexpr bool operator==(const PhysicsBoxCollider &) const noexcept = default;
    };

    /** @brief Body-local analytic sphere geometry. */
    struct PhysicsSphereCollider final {
        float radiusMeters{0.5F};
        [[nodiscard]] constexpr bool operator==(const PhysicsSphereCollider &) const noexcept = default;
    };

    /** @brief Body-local analytic capsule geometry aligned to the canonical vertical axis. */
    struct PhysicsCapsuleCollider final {
        float radiusMeters{0.5F};
        float cylindricalHalfHeightMeters{0.5F};
        [[nodiscard]] constexpr bool operator==(const PhysicsCapsuleCollider &) const noexcept = default;
    };

    /** @brief Infinite analytic plane supported only by explicitly static bodies. */
    struct PhysicsStaticPlaneCollider final {
        Math::Vec3 normal{0.0F, 1.0F, 0.0F};
        float signedDistanceMeters{};
        [[nodiscard]] constexpr bool operator==(const PhysicsStaticPlaneCollider &) const noexcept = default;
    };

    using PhysicsAnalyticCollider =
        std::variant<PhysicsBoxCollider, PhysicsSphereCollider, PhysicsCapsuleCollider, PhysicsStaticPlaneCollider>;

    /** @brief Exact cooked shape-asset source; loading and solver realization remain outside this inert value. */
    struct PhysicsShapeAssetReference final {
        Assets::AssetId asset;
        PhysicsShapeSubresourceId subresource;
        [[nodiscard]] constexpr bool operator==(const PhysicsShapeAssetReference &) const noexcept = default;
    };

    using PhysicsColliderSource = std::variant<PhysicsAnalyticCollider, PhysicsShapeAssetReference>;

    /** @brief Stable shape material-slot to physical-material asset mapping. */
    struct PhysicsColliderMaterialBinding final {
        Physics::PhysicsMaterialSlotId slot;
        Assets::AssetId material;
        [[nodiscard]] constexpr auto operator<=>(const PhysicsColliderMaterialBinding &) const noexcept = default;
    };

    /** @brief Serializable collider contributor bound to one explicit authored body. */
    struct ColliderComponent final {
        PhysicsComponentId id;
        PhysicsColliderSlotId collider;
        std::uint32_t schemaVersion{PhysicsSceneComponentSchemaVersion};
        std::uint64_t generation{1};
        PhysicsBodyReference body;
        PhysicsColliderSource source{PhysicsAnalyticCollider{PhysicsBoxCollider{}}};
        AuthoredPhysicsPose localPose;
        Math::Vec3 scale{1.0F, 1.0F, 1.0F};
        Physics::CollisionProfileId collisionProfile;
        std::vector<PhysicsColliderMaterialBinding> materials;
        bool sensor{};
        bool enabled{true};
        [[nodiscard]] bool operator==(const ColliderComponent &) const noexcept = default;
    };

    /** @brief Explicit body endpoint and body-local constraint frame. */
    struct PhysicsConstraintBodyEndpoint final {
        PhysicsBodyReference body;
        AuthoredPhysicsPose localFrame;
        [[nodiscard]] constexpr bool operator==(const PhysicsConstraintBodyEndpoint &) const noexcept = default;
    };

    /** @brief Explicit world-anchor endpoint and world-space frame. */
    struct PhysicsConstraintWorldEndpoint final {
        AuthoredPhysicsPose frame;
        [[nodiscard]] constexpr bool operator==(const PhysicsConstraintWorldEndpoint &) const noexcept = default;
    };

    using PhysicsConstraintSecondEndpoint = std::variant<PhysicsConstraintBodyEndpoint, PhysicsConstraintWorldEndpoint>;

    /** @brief Fixed relative-frame constraint policy. */
    struct PhysicsFixedConstraint final {
        [[nodiscard]] constexpr bool operator==(const PhysicsFixedConstraint &) const noexcept = default;
    };

    /** @brief Bounded distance interval in canonical meters. */
    struct PhysicsDistanceConstraint final {
        float minimumMeters{};
        float maximumMeters{1.0F};
        [[nodiscard]] constexpr bool operator==(const PhysicsDistanceConstraint &) const noexcept = default;
    };

    /** @brief Serializable constraint producer linking explicit stable authored body slots. */
    struct PhysicsConstraintComponent final {
        PhysicsComponentId id;
        PhysicsConstraintSlotId constraint;
        std::uint32_t schemaVersion{PhysicsSceneComponentSchemaVersion};
        std::uint64_t generation{1};
        PhysicsConstraintBodyEndpoint first;
        PhysicsConstraintSecondEndpoint second{PhysicsConstraintWorldEndpoint{}};
        std::variant<PhysicsFixedConstraint, PhysicsDistanceConstraint> parameters;
        bool enabled{true};
        [[nodiscard]] bool operator==(const PhysicsConstraintComponent &) const noexcept = default;
    };

    /** @brief Non-owning immutable projection of one object's Physics producers for aggregate validation. */
    struct PhysicsSceneComponentView final {
        SceneObjectId owner;
        const RigidBodyComponent *rigidBody{};
        std::span<const ColliderComponent> colliders;
        std::span<const PhysicsConstraintComponent> constraints;
    };

    /** @brief Validates one inert rigid-body producer. @param component Value to inspect. @return Success or DescriptorInvalid. */
    [[nodiscard]] Result<void> ValidateRigidBodyComponent(const RigidBodyComponent &component);
    /** @brief Validates one inert collider contributor. @param component Value to inspect. @return Success or DescriptorInvalid. */
    [[nodiscard]] Result<void> ValidateColliderComponent(const ColliderComponent &component);
    /** @brief Validates one inert constraint producer. @param component Value to inspect. @return Success or DescriptorInvalid. */
    [[nodiscard]] Result<void> ValidatePhysicsConstraintComponent(const PhysicsConstraintComponent &component);
    /**
     * @brief Validates stable identities, explicit references, compatibility, and producer cardinality.
     * @param components Immutable projections whose pointed-to values remain alive for the call.
     * @return Success or DescriptorInvalid without retaining any borrowed storage.
     */
    [[nodiscard]] Result<void> ValidatePhysicsSceneComponentViews(std::span<const PhysicsSceneComponentView> components);
}  // namespace Horo::Runtime
