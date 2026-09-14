#include "Horo/Runtime/Scene/PhysicsSceneComponents.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

namespace Horo::Runtime {
    namespace {
        struct ObjectLocalKey final {
            std::uint64_t object{};
            std::uint64_t local{};
            [[nodiscard]] constexpr bool operator==(const ObjectLocalKey &) const noexcept = default;
        };

        struct ObjectLocalKeyHash final {
            [[nodiscard]] std::size_t operator()(const ObjectLocalKey key) const noexcept {
                const std::size_t first = std::hash<std::uint64_t>{}(key.object);
                const std::size_t second = std::hash<std::uint64_t>{}(key.local);
                return first ^ (second + 0x9e3779b97f4a7c15ULL + (first << 6U) + (first >> 2U));
            }
        };

        [[nodiscard]] Result<void> Failure(std::string message) {
            return Result<void>::Failure(MakeError(Physics::PhysicsErrors::DescriptorInvalid, std::move(message)));
        }

        [[nodiscard]] bool IsValid(const PhysicsBodyReference reference) noexcept {
            return reference.object.IsValid() && reference.body.IsValid();
        }

        [[nodiscard]] bool IsValid(const AuthoredPhysicsPose &pose) noexcept {
            if (!Math::IsFinite(pose.translation) || !Math::IsFinite(pose.rotation))
                return false;
            const double norm =
                static_cast<double>(pose.rotation.x) * pose.rotation.x + static_cast<double>(pose.rotation.y) * pose.rotation.y +
                static_cast<double>(pose.rotation.z) * pose.rotation.z + static_cast<double>(pose.rotation.w) * pose.rotation.w;
            return std::abs(norm - 1.0) <= 1.0e-6;
        }

        [[nodiscard]] ObjectLocalKey Key(const SceneObjectId object, const std::uint64_t local) noexcept {
            return {object.value, local};
        }

        [[nodiscard]] ObjectLocalKey Key(const PhysicsBodyReference reference) noexcept {
            return Key(reference.object, reference.body.value);
        }

        [[nodiscard]] bool IsValidMass(const RigidBodyComponent &component) noexcept {
            if (component.motion == AuthoredPhysicsMotionType::Dynamic) {
                if (const auto *mass = std::get_if<AuthoredPhysicsMass>(&component.mass))
                    return std::isfinite(mass->kilograms) && mass->kilograms >= 0.001F && mass->kilograms <= 1.0e9F;
                if (const auto *density = std::get_if<AuthoredPhysicsDensity>(&component.mass))
                    return std::isfinite(density->kilogramsPerCubicMeter) && density->kilogramsPerCubicMeter >= 0.001F &&
                           density->kilogramsPerCubicMeter <= 1.0e7F;
                return false;
            }
            return std::holds_alternative<AuthoredPhysicsNoMass>(component.mass);
        }

        [[nodiscard]] bool IsValid(const PhysicsBoxCollider &shape) noexcept {
            return Math::IsFinite(shape.halfExtentsMeters) && shape.halfExtentsMeters.x > 0.0F && shape.halfExtentsMeters.y > 0.0F &&
                   shape.halfExtentsMeters.z > 0.0F;
        }

        [[nodiscard]] bool IsValid(const PhysicsSphereCollider &shape) noexcept {
            return std::isfinite(shape.radiusMeters) && shape.radiusMeters > 0.0F;
        }

        [[nodiscard]] bool IsValid(const PhysicsCapsuleCollider &shape) noexcept {
            return std::isfinite(shape.radiusMeters) && shape.radiusMeters > 0.0F && std::isfinite(shape.cylindricalHalfHeightMeters) &&
                   shape.cylindricalHalfHeightMeters >= 0.0F;
        }

        [[nodiscard]] bool IsValid(const PhysicsStaticPlaneCollider &shape) noexcept {
            if (!Math::IsFinite(shape.normal) || !std::isfinite(shape.signedDistanceMeters))
                return false;
            const double norm = static_cast<double>(shape.normal.x) * shape.normal.x +
                                static_cast<double>(shape.normal.y) * shape.normal.y + static_cast<double>(shape.normal.z) * shape.normal.z;
            return std::abs(norm - 1.0) <= 1.0e-6;
        }

        [[nodiscard]] bool IsValidAnalytic(const PhysicsAnalyticCollider &shape) noexcept {
            return std::visit([](const auto &value) {
                return IsValid(value);
            }, shape);
        }

        [[nodiscard]] bool IsStaticPlane(const PhysicsColliderSource &source) noexcept {
            const auto *analytic = std::get_if<PhysicsAnalyticCollider>(&source);
            return analytic != nullptr && std::holds_alternative<PhysicsStaticPlaneCollider>(*analytic);
        }

        [[nodiscard]] bool HasValidRigidBodyIdentity(const RigidBodyComponent &component) noexcept {
            return component.id.IsValid() && component.body.IsValid() && component.schemaVersion == PhysicsSceneComponentSchemaVersion &&
                   component.generation != 0 && component.motion < AuthoredPhysicsMotionType::Count;
        }

        [[nodiscard]] bool HasValidRigidBodyMotion(const RigidBodyComponent &component) noexcept {
            return Math::IsFinite(component.initialLinearVelocity) && Math::IsFinite(component.initialAngularVelocity) &&
                   std::isfinite(component.linearDampingPerSecond) && std::isfinite(component.angularDampingPerSecond) &&
                   component.linearDampingPerSecond >= 0.0F && component.angularDampingPerSecond >= 0.0F;
        }

        [[nodiscard]] bool HasValidRigidBodySpeedLimits(const RigidBodyComponent &component) noexcept {
            return std::isfinite(component.maximumLinearSpeed) && component.maximumLinearSpeed > 0.0F &&
                   component.maximumLinearSpeed <= 500.0F && std::isfinite(component.maximumAngularSpeed) &&
                   component.maximumAngularSpeed > 0.0F && component.maximumAngularSpeed <= 100.0F;
        }

        [[nodiscard]] bool HasValidColliderIdentity(const ColliderComponent &component) noexcept {
            return component.id.IsValid() && component.collider.IsValid() &&
                   component.schemaVersion == PhysicsSceneComponentSchemaVersion && component.generation != 0 && IsValid(component.body) &&
                   component.collisionProfile.IsValid();
        }

        [[nodiscard]] bool HasValidColliderTransform(const ColliderComponent &component) noexcept {
            return IsValid(component.localPose) && Math::IsFinite(component.scale) && component.scale.x > 0.0F &&
                   component.scale.y > 0.0F && component.scale.z > 0.0F;
        }

        [[nodiscard]] bool HasValidConstraintIdentity(const PhysicsConstraintComponent &component) noexcept {
            return component.id.IsValid() && component.constraint.IsValid() &&
                   component.schemaVersion == PhysicsSceneComponentSchemaVersion && component.generation != 0 &&
                   IsValid(component.first.body) && IsValid(component.first.localFrame);
        }

        [[nodiscard]] Result<void> ValidateColliderSource(const ColliderComponent &component) {
            if (const auto *analytic = std::get_if<PhysicsAnalyticCollider>(&component.source)) {
                if (!IsValidAnalytic(*analytic))
                    return Failure("Physics collider analytic geometry is invalid.");
                const bool uniform =
                    Math::NearlyEqual(component.scale.x, component.scale.y) && Math::NearlyEqual(component.scale.y, component.scale.z);
                if (!uniform &&
                    (std::holds_alternative<PhysicsSphereCollider>(*analytic) || std::holds_alternative<PhysicsCapsuleCollider>(*analytic)))
                    return Failure("Sphere and capsule colliders require uniform authored scale.");
                return Result<void>::Success();
            }
            const auto &asset = std::get<PhysicsShapeAssetReference>(component.source);
            if (!asset.asset.IsValid() || !asset.subresource.IsValid())
                return Failure("Physics collider shape assets require stable asset and subresource identities.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateConstraintEndpoint(const PhysicsConstraintComponent &component) {
            if (const auto *body = std::get_if<PhysicsConstraintBodyEndpoint>(&component.second)) {
                if (!IsValid(body->body) || !IsValid(body->localFrame) || body->body == component.first.body)
                    return Failure("Physics constraint body endpoints must be valid, explicit, and distinct.");
            } else if (!IsValid(std::get<PhysicsConstraintWorldEndpoint>(component.second).frame)) {
                return Failure("Physics constraint world endpoints require a valid explicit world-frame pose.");
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateConstraintParameters(const PhysicsConstraintComponent &component) {
            const auto *distance = std::get_if<PhysicsDistanceConstraint>(&component.parameters);
            if (distance != nullptr && (!std::isfinite(distance->minimumMeters) || !std::isfinite(distance->maximumMeters) ||
                                        distance->minimumMeters < 0.0F || distance->maximumMeters < distance->minimumMeters))
                return Failure("Physics distance constraints require a finite non-negative ordered interval.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateMaterialBindings(const std::span<const PhysicsColliderMaterialBinding> materials) {
            if (materials.empty() || materials.size() > MaximumPhysicsColliderMaterialBindings)
                return Failure("Physics colliders require a bounded, non-empty material-slot mapping.");
            std::unordered_set<std::uint64_t> slots;
            slots.reserve(materials.size());
            for (const PhysicsColliderMaterialBinding &binding : materials) {
                if (!binding.slot.IsValid() || !binding.material.IsValid() || !slots.insert(binding.slot.Value()).second)
                    return Failure("Physics collider material slots must have unique stable identities and valid material assets.");
            }
            return Result<void>::Success();
        }

        using BodyMap = std::unordered_map<ObjectLocalKey, const RigidBodyComponent *, ObjectLocalKeyHash>;

        [[nodiscard]] Result<void> AddComponentIdentity(const SceneObjectId owner, const PhysicsComponentId component,
                                                        std::unordered_set<ObjectLocalKey, ObjectLocalKeyHash> &identities) {
            if (!component.IsValid() || !identities.insert(Key(owner, component.value)).second)
                return Failure("Physics component identities must be non-zero and unique on their owning Scene object.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> CollectBodies(const std::span<const PhysicsSceneComponentView> components, BodyMap &bodies,
                                                 std::unordered_set<ObjectLocalKey, ObjectLocalKeyHash> &componentIds) {
            for (const PhysicsSceneComponentView &view : components) {
                if (!view.owner.IsValid())
                    return Failure("Physics component projections require a valid owning Scene object identity.");
                if (view.rigidBody == nullptr)
                    continue;
                if (Result<void> valid = ValidateRigidBodyComponent(*view.rigidBody); valid.HasError())
                    return valid;
                if (Result<void> unique = AddComponentIdentity(view.owner, view.rigidBody->id, componentIds); unique.HasError())
                    return unique;
                if (!bodies.emplace(Key(view.owner, view.rigidBody->body.value), view.rigidBody).second)
                    return Failure("Authored Physics body slots must be unique within one Scene object.");
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateColliderReference(const ColliderComponent &collider, const BodyMap &bodies,
                                                             std::unordered_map<ObjectLocalKey, std::size_t, ObjectLocalKeyHash> &counts) {
            const auto body = bodies.find(Key(collider.body));
            if (body == bodies.end())
                return Failure("Physics colliders must reference an exact authored body in the same Scene definition.");
            if (IsStaticPlane(collider.source) && body->second->motion != AuthoredPhysicsMotionType::Static)
                return Failure("Static-plane colliders may bind only to an explicitly static authored body.");
            if (++counts[body->first] > MaximumPhysicsCollidersPerBody)
                return Failure("An authored Physics body exceeds the bounded collider contributor limit.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateEndpoint(const PhysicsConstraintBodyEndpoint &endpoint, const BodyMap &bodies) {
            if (!bodies.contains(Key(endpoint.body)))
                return Failure("Physics constraints must reference exact authored bodies in the same Scene definition.");
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateColliders(const PhysicsSceneComponentView &view, const BodyMap &bodies,
                                                     std::unordered_set<ObjectLocalKey, ObjectLocalKeyHash> &componentIds,
                                                     std::unordered_set<ObjectLocalKey, ObjectLocalKeyHash> &colliderIds,
                                                     std::unordered_map<ObjectLocalKey, std::size_t, ObjectLocalKeyHash> &counts) {
            for (const ColliderComponent &collider : view.colliders) {
                if (Result<void> valid = ValidateColliderComponent(collider); valid.HasError())
                    return valid;
                if (Result<void> unique = AddComponentIdentity(view.owner, collider.id, componentIds); unique.HasError())
                    return unique;
                if (!colliderIds.insert(Key(view.owner, collider.collider.value)).second)
                    return Failure("Authored Physics collider slots must be unique within one Scene object.");
                if (Result<void> reference = ValidateColliderReference(collider, bodies, counts); reference.HasError())
                    return reference;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateConstraints(const PhysicsSceneComponentView &view, const BodyMap &bodies,
                                                       std::unordered_set<ObjectLocalKey, ObjectLocalKeyHash> &componentIds,
                                                       std::unordered_set<ObjectLocalKey, ObjectLocalKeyHash> &constraintIds) {
            for (const PhysicsConstraintComponent &constraint : view.constraints) {
                if (Result<void> valid = ValidatePhysicsConstraintComponent(constraint); valid.HasError())
                    return valid;
                if (Result<void> unique = AddComponentIdentity(view.owner, constraint.id, componentIds); unique.HasError())
                    return unique;
                if (!constraintIds.insert(Key(view.owner, constraint.constraint.value)).second)
                    return Failure("Authored Physics constraint slots must be unique within one Scene object.");
                if (Result<void> first = ValidateEndpoint(constraint.first, bodies); first.HasError())
                    return first;
                if (const auto *second = std::get_if<PhysicsConstraintBodyEndpoint>(&constraint.second)) {
                    if (Result<void> endpoint = ValidateEndpoint(*second, bodies); endpoint.HasError())
                        return endpoint;
                }
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> ValidateReferences(const std::span<const PhysicsSceneComponentView> components, const BodyMap &bodies,
                                                      std::unordered_set<ObjectLocalKey, ObjectLocalKeyHash> &componentIds) {
            std::unordered_set<ObjectLocalKey, ObjectLocalKeyHash> colliderIds;
            std::unordered_set<ObjectLocalKey, ObjectLocalKeyHash> constraintIds;
            std::unordered_map<ObjectLocalKey, std::size_t, ObjectLocalKeyHash> colliderCounts;
            for (const PhysicsSceneComponentView &view : components) {
                if (Result<void> colliders = ValidateColliders(view, bodies, componentIds, colliderIds, colliderCounts);
                    colliders.HasError())
                    return colliders;
                if (Result<void> constraints = ValidateConstraints(view, bodies, componentIds, constraintIds); constraints.HasError())
                    return constraints;
            }
            if (std::ranges::any_of(bodies, [&colliderCounts](const auto &body) {
                return !colliderCounts.contains(body.first);
            }))
                return Failure("Every authored rigid body must have at least one explicit collider contributor.");
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidateRigidBodyComponent */
    Result<void> ValidateRigidBodyComponent(const RigidBodyComponent &component) {
        if (!HasValidRigidBodyIdentity(component) || !HasValidRigidBodyMotion(component) || !HasValidRigidBodySpeedLimits(component) ||
            !IsValidMass(component))
            return Failure("Rigid bodies require stable identities, supported schema, finite bounded motion, and compatible mass policy.");
        return Result<void>::Success();
    }

    /** @copydoc ValidateColliderComponent */
    Result<void> ValidateColliderComponent(const ColliderComponent &component) {
        if (!HasValidColliderIdentity(component) || !HasValidColliderTransform(component))
            return Failure(
                "Physics colliders require stable identities, schema, body/profile references, pose, and positive finite scale.");
        if (Result<void> source = ValidateColliderSource(component); source.HasError())
            return source;
        return ValidateMaterialBindings(component.materials);
    }

    /** @copydoc ValidatePhysicsConstraintComponent */
    Result<void> ValidatePhysicsConstraintComponent(const PhysicsConstraintComponent &component) {
        if (!HasValidConstraintIdentity(component))
            return Failure("Physics constraints require stable identities, schema, generation, and a valid first body endpoint.");
        if (Result<void> endpoint = ValidateConstraintEndpoint(component); endpoint.HasError())
            return endpoint;
        return ValidateConstraintParameters(component);
    }

    /** @copydoc ValidatePhysicsSceneComponentViews */
    Result<void> ValidatePhysicsSceneComponentViews(const std::span<const PhysicsSceneComponentView> components) {
        BodyMap bodies;
        std::unordered_set<ObjectLocalKey, ObjectLocalKeyHash> componentIds;
        bodies.reserve(components.size());
        componentIds.reserve(components.size() * 3);
        if (Result<void> valid = CollectBodies(components, bodies, componentIds); valid.HasError())
            return valid;
        return ValidateReferences(components, bodies, componentIds);
    }
}  // namespace Horo::Runtime
