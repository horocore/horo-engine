#include "ColliderGeometryInternal.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace Horo::Editor {
    namespace {
        /** @brief Retain the canonical transaction rejection vocabulary. */
        Error Rejected(const SceneTransactionFailure failure) {
            return MakeError(SceneTransactionError(failure));
        }

        /** @brief Reject malformed detached geometry before projection or inverse-ray arithmetic. */
        bool ValidShape(const Runtime::PhysicsAnalyticCollider &shape) {
            if (const auto *box = std::get_if<Runtime::PhysicsBoxCollider>(&shape))
                return Math::IsFinite(box->halfExtentsMeters) && box->halfExtentsMeters.x > 0 && box->halfExtentsMeters.y > 0 &&
                       box->halfExtentsMeters.z > 0;
            if (const auto *sphere = std::get_if<Runtime::PhysicsSphereCollider>(&shape))
                return std::isfinite(sphere->radiusMeters) && sphere->radiusMeters > 0;
            if (const auto *capsule = std::get_if<Runtime::PhysicsCapsuleCollider>(&shape))
                return std::isfinite(capsule->radiusMeters) && capsule->radiusMeters > 0 &&
                       std::isfinite(capsule->cylindricalHalfHeightMeters) && capsule->cylindricalHalfHeightMeters > 0;
            const auto &plane = std::get<Runtime::PhysicsStaticPlaneCollider>(shape);
            return Math::IsFinite(plane.normal) && Math::NearlyEqual(Math::Dot(plane.normal, plane.normal), 1.0F) &&
                   std::isfinite(plane.signedDistanceMeters);
        }

    }  // namespace

    namespace ColliderEditingDetail {
        /** @copydoc ColliderEditingDetail::ShapeMatrix */
        Result<Math::Mat4> ShapeMatrix(const ColliderPresentation &collider) {
            if (collider.diagnostic)
                return Result<Math::Mat4>::Failure(*collider.diagnostic);
            const auto local = collider.localTransform.TryToMatrix();
            if (local.HasError())
                return Result<Math::Mat4>::Failure(local.ErrorValue());
            if (!ValidShape(collider.shape) || !Math::IsFinite(collider.bodyToWorld) || collider.localTransform.scale.x <= 0 ||
                collider.localTransform.scale.y <= 0 || collider.localTransform.scale.z <= 0)
                return Result<Math::Mat4>::Failure(Rejected(SceneTransactionFailure::Invalid));
            if (const bool uniform = Math::NearlyEqual(collider.localTransform.scale.x, collider.localTransform.scale.y) &&
                                     Math::NearlyEqual(collider.localTransform.scale.y, collider.localTransform.scale.z);
                !uniform && (std::holds_alternative<Runtime::PhysicsSphereCollider>(collider.shape) ||
                             std::holds_alternative<Runtime::PhysicsCapsuleCollider>(collider.shape)))
                return Result<Math::Mat4>::Failure(
                    MakeError(Physics::PhysicsErrors::OperationUnsupported, "Sphere and capsule guides require uniform authored scale."));
            return Result<Math::Mat4>::Success(Math::Multiply(collider.bodyToWorld, local.Value()));
        }

    }  // namespace ColliderEditingDetail

    namespace {
        using ColliderEditingDetail::ShapeMatrix;

        /** @brief Append bounded local geometry after the shared world transform. */
        void Segment(ColliderWireframe &wire, const Math::Mat4 &matrix, const Math::Vec3 first, const Math::Vec3 second) {
            if (wire.count < wire.segments.size())
                wire.segments[wire.count++] = {Math::TransformAffinePoint(matrix, first), Math::TransformAffinePoint(matrix, second)};
        }

        /** @brief Generate one deterministic circle in a local coordinate plane. */
        void Circle(ColliderWireframe &wire, const Math::Mat4 &matrix, const Math::Vec3 center, const Math::Vec3 u, const Math::Vec3 v,
                    const float radius) {
            constexpr int Count = 32;
            for (int index = 0; index < Count; ++index) {
                const float first = static_cast<float>(index) * 2 * std::numbers::pi_v<float> / Count;
                const float second = static_cast<float>(index + 1) * 2 * std::numbers::pi_v<float> / Count;
                Segment(wire, matrix, center + (u * std::cos(first) + v * std::sin(first)) * radius,
                        center + (u * std::cos(second) + v * std::sin(second)) * radius);
            }
        }

        /** @brief Draw the cylindrical seams and exterior hemispheres of a canonical capsule. */
        void CapsuleWire(ColliderWireframe &wire, const Math::Mat4 &matrix, const Runtime::PhysicsCapsuleCollider &capsule) {
            const float height = capsule.cylindricalHalfHeightMeters;
            Circle(wire, matrix, {0, height, 0}, {1, 0, 0}, {0, 0, 1}, capsule.radiusMeters);
            Circle(wire, matrix, {0, -height, 0}, {1, 0, 0}, {0, 0, 1}, capsule.radiusMeters);
            for (const Math::Vec3 axis : {Math::Vec3{1, 0, 0}, Math::Vec3{0, 0, 1}}) {
                for (int index = 0; index < 32; ++index) {
                    // Split at the equators so no segment connects through the capsule interior.
                    const float first = static_cast<float>(index) * 2 * std::numbers::pi_v<float> / 32;
                    const float second = static_cast<float>(index + 1) * 2 * std::numbers::pi_v<float> / 32;
                    const float y = index < 16 ? height : -height;
                    Segment(wire, matrix,
                            axis * (std::cos(first) * capsule.radiusMeters) + Math::Vec3{0, std::sin(first) * capsule.radiusMeters + y, 0},
                            axis * (std::cos(second) * capsule.radiusMeters) +
                                Math::Vec3{0, std::sin(second) * capsule.radiusMeters + y, 0});
                }
                for (const float side : {-1.0F, 1.0F})
                    Segment(wire, matrix, axis * (side * capsule.radiusMeters) + Math::Vec3{0, -height, 0},
                            axis * (side * capsule.radiusMeters) + Math::Vec3{0, height, 0});
            }
        }

        /** @brief Add a ray parameter only inside its requested world-distance interval. */
        void Candidate(std::optional<double> &nearest, const double distance, const Math::Ray &ray) {
            if (std::isfinite(distance) && distance >= ray.minimumDistance && distance <= ray.maximumDistance &&
                (!nearest || distance < *nearest))
                nearest = distance;
        }

        /** @brief Keep quadratic discriminants accurate for small colliders at large admitted world offsets. */
        double DotDouble(const Math::Vec3 first, const Math::Vec3 second) {
            return static_cast<double>(first.x) * second.x + static_cast<double>(first.y) * second.y +
                   static_cast<double>(first.z) * second.z;
        }

        /** @brief Intersect a sphere/hemisphere using an unnormalized local direction, preserving world t. */
        void SphereHit(std::optional<double> &nearest, const Math::Vec3 origin, const Math::Vec3 direction, const float radius,
                       const float centerY, const int hemisphere, const Math::Ray &ray) {
            const Math::Vec3 offset = origin - Math::Vec3{0, centerY, 0};
            const double a = DotDouble(direction, direction);
            const double b = DotDouble(offset, direction);
            const double c = DotDouble(offset, offset) - static_cast<double>(radius) * radius;
            const double discriminant = b * b - a * c;
            if (a <= 0 || discriminant < 0)
                return;
            for (const double distance : {(-b - std::sqrt(discriminant)) / a, (-b + std::sqrt(discriminant)) / a}) {
                const double y = origin.y + direction.y * distance - centerY;
                if (hemisphere == 0 || (hemisphere > 0 && y >= 0) || (hemisphere < 0 && y <= 0))
                    Candidate(nearest, distance, ray);
            }
        }

        /** @brief Intersect the finite cylinder and exterior hemispheres of a canonical +Y capsule. */
        std::optional<double> CapsuleHit(const Math::Vec3 origin, const Math::Vec3 direction,
                                         const Runtime::PhysicsCapsuleCollider &capsule, const Math::Ray &ray) {
            std::optional<double> nearest;
            const double a = static_cast<double>(direction.x) * direction.x + static_cast<double>(direction.z) * direction.z;
            const double b = static_cast<double>(origin.x) * direction.x + static_cast<double>(origin.z) * direction.z;
            const double c = static_cast<double>(origin.x) * origin.x + static_cast<double>(origin.z) * origin.z -
                             static_cast<double>(capsule.radiusMeters) * capsule.radiusMeters;
            if (const double discriminant = b * b - a * c; a > 0 && discriminant >= 0) {
                for (const double distance : {(-b - std::sqrt(discriminant)) / a, (-b + std::sqrt(discriminant)) / a}) {
                    if (const double y = origin.y + direction.y * distance; std::abs(y) <= capsule.cylindricalHalfHeightMeters)
                        Candidate(nearest, distance, ray);
                }
            }
            SphereHit(nearest, origin, direction, capsule.radiusMeters, capsule.cylindricalHalfHeightMeters, 1, ray);
            SphereHit(nearest, origin, direction, capsule.radiusMeters, -capsule.cylindricalHalfHeightMeters, -1, ray);
            return nearest;
        }

        /** @brief Use slab boundaries rather than a conservative world AABB for oriented boxes. */
        std::optional<double> BoxHit(const Math::Vec3 origin, const Math::Vec3 direction, const Math::Vec3 extent, const Math::Ray &ray) {
            double enter = -std::numeric_limits<double>::infinity();
            double leave = std::numeric_limits<double>::infinity();
            const std::array origins{origin.x, origin.y, origin.z};
            const std::array directions{direction.x, direction.y, direction.z};
            const std::array extents{extent.x, extent.y, extent.z};
            for (std::size_t axis = 0; axis < 3; ++axis) {
                if (directions[axis] == 0) {
                    if (std::abs(origins[axis]) > extents[axis])
                        return std::nullopt;
                    continue;
                }
                const double first = (-static_cast<double>(extents[axis]) - origins[axis]) / directions[axis];
                const double second = (static_cast<double>(extents[axis]) - origins[axis]) / directions[axis];
                enter = std::max(enter, std::min(first, second));
                leave = std::min(leave, std::max(first, second));
                if (enter > leave)
                    return std::nullopt;
            }
            std::optional<double> nearest;
            Candidate(nearest, enter, ray);
            Candidate(nearest, leave, ray);
            return nearest;
        }

        /** @brief Intersect a box in shape space while preserving the world ray parameter. */
        std::optional<double> LocalHit(const Runtime::PhysicsBoxCollider &shape, const Math::Vec3 origin, const Math::Vec3 direction,
                                       const Math::Ray &ray) {
            return BoxHit(origin, direction, shape.halfExtentsMeters, ray);
        }

        /** @brief Intersect a sphere using the shared precision-preserving quadratic. */
        std::optional<double> LocalHit(const Runtime::PhysicsSphereCollider &shape, const Math::Vec3 origin, const Math::Vec3 direction,
                                       const Math::Ray &ray) {
            std::optional<double> nearest;
            SphereHit(nearest, origin, direction, shape.radiusMeters, 0, 0, ray);
            return nearest;
        }

        /** @brief Intersect the cylinder and exterior caps as one canonical capsule. */
        std::optional<double> LocalHit(const Runtime::PhysicsCapsuleCollider &shape, const Math::Vec3 origin, const Math::Vec3 direction,
                                       const Math::Ray &ray) {
            return CapsuleHit(origin, direction, shape, ray);
        }

        /** @brief Intersect the analytic plane without imposing a display-grid bound. */
        std::optional<double> LocalHit(const Runtime::PhysicsStaticPlaneCollider &shape, const Math::Vec3 origin,
                                       const Math::Vec3 direction, const Math::Ray &ray) {
            std::optional<double> nearest;
            if (const double denominator = DotDouble(shape.normal, direction); denominator != 0)
                Candidate(nearest, (shape.signedDistanceMeters - DotDouble(shape.normal, origin)) / denominator, ray);
            return nearest;
        }

        /** @brief Intersect supported local geometry without normalizing the inverse-transformed ray direction. */
        std::optional<double> Hit(const ColliderPresentation &collider, const Math::Ray &ray) {
            const auto matrix = ShapeMatrix(collider);
            if (matrix.HasError())
                return std::nullopt;
            const auto inverse = Math::TryInverseAffine(matrix.Value());
            if (inverse.HasError())
                return std::nullopt;
            const Math::Vec3 origin = Math::TransformAffinePoint(inverse.Value(), ray.origin);
            const Math::Vec3 direction = Math::TransformDirection(inverse.Value(), ray.direction);
            return std::visit([origin, direction, &ray](const auto &shape) {
                return LocalHit(shape, origin, direction, ray);
            }, collider.shape);
        }

        /** @brief Generate the twelve oriented box edges without conservative world bounds. */
        Result<ColliderWireframe> ShapeWire(const Math::Mat4 &matrix, const Runtime::PhysicsBoxCollider &box) {
            ColliderWireframe wire;
            std::array<Math::Vec3, 8> points{};
            for (std::size_t index = 0; index < points.size(); ++index)
                points[index] = {box.halfExtentsMeters.x * ((index & 1U) ? 1 : -1), box.halfExtentsMeters.y * ((index & 2U) ? 1 : -1),
                                 box.halfExtentsMeters.z * ((index & 4U) ? 1 : -1)};
            for (std::size_t index = 0; index < points.size(); ++index)
                for (const std::size_t bit : {1U, 2U, 4U})
                    if (!(index & bit))
                        Segment(wire, matrix, points[index], points[index | bit]);
            return Result<ColliderWireframe>::Success(wire);
        }

        /** @brief Generate the sphere's three orthogonal great circles. */
        Result<ColliderWireframe> ShapeWire(const Math::Mat4 &matrix, const Runtime::PhysicsSphereCollider &sphere) {
            ColliderWireframe wire;
            Circle(wire, matrix, {}, {1, 0, 0}, {0, 1, 0}, sphere.radiusMeters);
            Circle(wire, matrix, {}, {1, 0, 0}, {0, 0, 1}, sphere.radiusMeters);
            Circle(wire, matrix, {}, {0, 1, 0}, {0, 0, 1}, sphere.radiusMeters);
            return Result<ColliderWireframe>::Success(wire);
        }

        /** @brief Generate only the capsule's exterior seams and caps. */
        Result<ColliderWireframe> ShapeWire(const Math::Mat4 &matrix, const Runtime::PhysicsCapsuleCollider &capsule) {
            ColliderWireframe wire;
            CapsuleWire(wire, matrix, capsule);
            return Result<ColliderWireframe>::Success(wire);
        }

        /** @brief Generate a bounded plane grid; its analytic picking surface remains infinite. */
        Result<ColliderWireframe> ShapeWire(const Math::Mat4 &matrix, const Runtime::PhysicsStaticPlaneCollider &plane) {
            const auto tangent =
                Math::TryNormalize(Math::Cross(plane.normal, std::abs(plane.normal.y) < 0.9F ? Math::Vec3{0, 1, 0} : Math::Vec3{1, 0, 0}));
            if (tangent.HasError())
                return Result<ColliderWireframe>::Failure(tangent.ErrorValue());
            const Math::Vec3 u = tangent.Value();
            const Math::Vec3 v = Math::Cross(plane.normal, u);
            const Math::Vec3 center = plane.normal * plane.signedDistanceMeters;
            ColliderWireframe wire;
            for (int index = -5; index <= 5; ++index) {
                Segment(wire, matrix, center + u * static_cast<float>(index) - v * 5, center + u * static_cast<float>(index) + v * 5);
                Segment(wire, matrix, center + v * static_cast<float>(index) - u * 5, center + v * static_cast<float>(index) + u * 5);
            }
            return Result<ColliderWireframe>::Success(wire);
        }

    }  // namespace

    /** @copydoc BuildColliderWireframe */
    Result<ColliderWireframe> BuildColliderWireframe(const ColliderPresentation &collider) {
        using Return = Result<ColliderWireframe>;
        const auto matrix = ShapeMatrix(collider);
        if (matrix.HasError())
            return Return::Failure(matrix.ErrorValue());
        auto generated = std::visit([&matrix](const auto &shape) {
            return ShapeWire(matrix.Value(), shape);
        }, collider.shape);
        if (generated.HasError())
            return Return::Failure(generated.ErrorValue());
        const auto &wire = generated.Value();
        for (std::size_t index = 0; index < wire.count; ++index)
            if (!Math::IsFinite(wire.segments[index].first) || !Math::IsFinite(wire.segments[index].second))
                return Return::Failure(Rejected(SceneTransactionFailure::Invalid));
        return generated;
    }

    /** @copydoc PickPhysicsCollider */
    Result<std::optional<ColliderEditTarget>> PickPhysicsCollider(const std::span<const ColliderPresentation> colliders,
                                                                  const Math::Ray &ray) {
        using Return = Result<std::optional<ColliderEditTarget>>;
        if (Math::TryMakeRay(ray.origin, ray.direction, ray.minimumDistance, ray.maximumDistance).HasError() ||
            !Math::NearlyEqual(Math::Dot(ray.direction, ray.direction), 1.0F))
            return Return::Failure(Rejected(SceneTransactionFailure::Invalid));
        std::optional<ColliderEditTarget> target;
        std::optional<double> nearest;
        for (const auto &collider : colliders) {
            if (!collider.visible || collider.diagnostic)
                continue;
            if (!collider.target.object.IsValid() || !collider.target.component.IsValid() || collider.generation == 0)
                return Return::Failure(Rejected(SceneTransactionFailure::Invalid));
            const auto distance = Hit(collider, ray);
            if (distance && (!nearest || *distance < *nearest || (*distance == *nearest && collider.target < *target))) {
                nearest = distance;
                target = collider.target;
            }
        }
        return Return::Success(target);
    }

}  // namespace Horo::Editor
