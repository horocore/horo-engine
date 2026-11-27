#include "Horo/Physics/PhysicsShapeDescriptor.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <cmath>
#include <limits>

namespace Horo::Physics {
    namespace {
        /** @brief Checks one positive finite dimension without imposing a motion-specific size profile. */
        bool PositiveFinite(const float value) {
            return std::isfinite(value) && value > 0;
        }

        /** @brief Reports malformed authored scale or a non-finite folded dimension. */
        Result<ResolvedPhysicsPrimitiveShape> InvalidScale(const char *message) {
            return Result<ResolvedPhysicsPrimitiveShape>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, message));
        }

        /** @brief Multiplies positive finite authored values without overflowing binary32 output. */
        bool CheckedScale(const float value, const float factor, float &scaled) {
            const double candidate = static_cast<double>(value) * factor;
            if (!std::isfinite(candidate) || candidate > std::numeric_limits<float>::max())
                return false;
            scaled = static_cast<float>(candidate);
            return PositiveFinite(scaled);
        }

        /** @brief Folds component-wise scale into box dimensions. */
        Result<PhysicsShapeDescriptor> ResolveShape(const PhysicsBoxShape &box, const Math::Vec3 scale) {
            PhysicsBoxShape resolved;
            if (!CheckedScale(box.halfExtentsMeters.x, scale.x, resolved.halfExtentsMeters.x) ||
                !CheckedScale(box.halfExtentsMeters.y, scale.y, resolved.halfExtentsMeters.y) ||
                !CheckedScale(box.halfExtentsMeters.z, scale.z, resolved.halfExtentsMeters.z))
                return Result<PhysicsShapeDescriptor>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Scaled box half extents must remain positive and finite."));
            return Result<PhysicsShapeDescriptor>::Success(resolved);
        }

        /** @brief Folds geometry-preserving uniform scale into a sphere radius. */
        Result<PhysicsShapeDescriptor> ResolveShape(const PhysicsSphereShape &sphere, const Math::Vec3 scale) {
            if (scale.x != scale.y || scale.x != scale.z)
                return Result<PhysicsShapeDescriptor>::Failure(
                    MakeError(PhysicsErrors::OperationUnsupported, "Sphere scale must be uniform to preserve analytic geometry."));
            PhysicsSphereShape resolved;
            if (!CheckedScale(sphere.radiusMeters, scale.x, resolved.radiusMeters))
                return Result<PhysicsShapeDescriptor>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Scaled sphere radius must remain positive and finite."));
            return Result<PhysicsShapeDescriptor>::Success(resolved);
        }

        /** @brief Folds geometry-preserving uniform scale into capsule dimensions. */
        Result<PhysicsShapeDescriptor> ResolveShape(const PhysicsCapsuleShape &capsule, const Math::Vec3 scale) {
            if (scale.x != scale.y || scale.x != scale.z)
                return Result<PhysicsShapeDescriptor>::Failure(
                    MakeError(PhysicsErrors::OperationUnsupported, "Capsule scale must be uniform to preserve analytic geometry."));
            PhysicsCapsuleShape resolved;
            if (!CheckedScale(capsule.radiusMeters, scale.x, resolved.radiusMeters) ||
                !CheckedScale(capsule.cylindricalHalfHeightMeters, scale.x, resolved.cylindricalHalfHeightMeters))
                return Result<PhysicsShapeDescriptor>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Scaled capsule dimensions must remain positive and finite."));
            return Result<PhysicsShapeDescriptor>::Success(resolved);
        }

        /** @brief Applies the inverse-transpose scale transform to an exact analytic plane equation. */
        Result<PhysicsShapeDescriptor> ResolveShape(const PhysicsStaticPlaneShape &plane, const Math::Vec3 scale) {
            const double transformedX = static_cast<double>(plane.normal.x) / scale.x;
            const double transformedY = static_cast<double>(plane.normal.y) / scale.y;
            const double transformedZ = static_cast<double>(plane.normal.z) / scale.z;
            const double squaredLength = transformedX * transformedX + transformedY * transformedY + transformedZ * transformedZ;
            if (!std::isfinite(squaredLength) || squaredLength <= 0)
                return Result<PhysicsShapeDescriptor>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Scaled plane normal must remain finite and non-degenerate."));
            const double length = std::sqrt(squaredLength);
            const double signedDistance = static_cast<double>(plane.signedDistanceMeters) / length;
            const PhysicsStaticPlaneShape resolved{{static_cast<float>(transformedX / length), static_cast<float>(transformedY / length),
                                                    static_cast<float>(transformedZ / length)},
                                                   static_cast<float>(signedDistance)};
            if (!Math::IsFinite(resolved.normal) || !std::isfinite(resolved.signedDistanceMeters))
                return Result<PhysicsShapeDescriptor>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Scaled plane equation must remain finite."));
            return Result<PhysicsShapeDescriptor>::Success(resolved);
        }

        /** @brief Validates each independently named box half dimension. */
        Result<void> ValidateShape(const PhysicsBoxShape &box) {
            if (!PositiveFinite(box.halfExtentsMeters.x) || !PositiveFinite(box.halfExtentsMeters.y) ||
                !PositiveFinite(box.halfExtentsMeters.z))
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Box half extents must be positive and finite."));
            return Result<void>::Success();
        }

        /** @brief Validates the sphere radius. */
        Result<void> ValidateShape(const PhysicsSphereShape &sphere) {
            if (!PositiveFinite(sphere.radiusMeters))
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Sphere radius must be positive and finite."));
            return Result<void>::Success();
        }

        /** @brief Validates radius and the non-degenerate cylindrical portion independently. */
        Result<void> ValidateShape(const PhysicsCapsuleShape &capsule) {
            if (!PositiveFinite(capsule.radiusMeters) || !PositiveFinite(capsule.cylindricalHalfHeightMeters))
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Capsule radius and cylindrical half-height must be positive and finite."));
            return Result<void>::Success();
        }

        /** @brief Checks the plane equation without modifying its orientation or signed distance. */
        Result<void> ValidateShape(const PhysicsStaticPlaneShape &plane) {
            if (!Math::IsFinite(plane.normal) || !std::isfinite(plane.signedDistanceMeters))
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Plane normal and signed distance must be finite."));
            if (const double squaredNorm = static_cast<double>(plane.normal.x) * plane.normal.x +
                                           static_cast<double>(plane.normal.y) * plane.normal.y +
                                           static_cast<double>(plane.normal.z) * plane.normal.z;
                std::abs(squaredNorm - 1.0) > PhysicsPlaneNormalSquaredNormTolerance)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Plane normal must have unit squared norm within 1e-6."));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidatePhysicsShapeDescriptor */
    Result<void> ValidatePhysicsShapeDescriptor(const PhysicsShapeDescriptor &descriptor) {
        return std::visit([](const auto &shape) {
            return ValidateShape(shape);
        }, descriptor);
    }

    /** @copydoc ResolvePhysicsPrimitiveShape */
    Result<ResolvedPhysicsPrimitiveShape> ResolvePhysicsPrimitiveShape(const PhysicsPrimitiveShapeRequest &request) {
        if (const auto geometry = ValidatePhysicsShapeDescriptor(request.geometry); geometry.HasError())
            return Result<ResolvedPhysicsPrimitiveShape>::Failure(geometry.ErrorValue());
        if (const auto pose = ValidatePhysicsPose(request.localPose); pose.HasError())
            return Result<ResolvedPhysicsPrimitiveShape>::Failure(pose.ErrorValue());
        if (!PositiveFinite(request.scale.factors.x) || !PositiveFinite(request.scale.factors.y) ||
            !PositiveFinite(request.scale.factors.z))
            return InvalidScale("Physics shape scale factors must be positive and finite.");

        auto resolvedGeometry = std::visit([&request](const auto &shape) {
            return ResolveShape(shape, request.scale.factors);
        }, request.geometry);
        if (resolvedGeometry.HasError())
            return Result<ResolvedPhysicsPrimitiveShape>::Failure(resolvedGeometry.ErrorValue());

        ResolvedPhysicsPrimitiveShape resolved{std::move(resolvedGeometry).Value(), request.localPose};
        if (const auto geometry = ValidatePhysicsShapeDescriptor(resolved.geometry); geometry.HasError())
            return Result<ResolvedPhysicsPrimitiveShape>::Failure(geometry.ErrorValue());
        return Result<ResolvedPhysicsPrimitiveShape>::Success(std::move(resolved));
    }
}  // namespace Horo::Physics
