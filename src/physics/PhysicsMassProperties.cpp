#include "Horo/Physics/PhysicsMassProperties.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <string>
#include <type_traits>
#include <utility>

namespace Horo::Physics {
    namespace {
        struct DoubleVector final {
            double x{};
            double y{};
            double z{};
        };

        struct DoubleTensor final {
            double xx{};
            double yy{};
            double zz{};
            double xy{};
            double xz{};
            double yz{};
        };

        struct UnitDensityPrimitive final {
            double volume{};
            DoubleTensor inertia;
        };

        template <typename T> [[nodiscard]] Result<T> Invalid(std::string message) {
            return Result<T>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, std::move(message)));
        }

        template <typename T> [[nodiscard]] Result<T> Unsupported(std::string message) {
            return Result<T>::Failure(MakeError(PhysicsErrors::OperationUnsupported, std::move(message)));
        }

        [[nodiscard]] DoubleVector ToDouble(const Math::Vec3 value) noexcept {
            return {static_cast<double>(value.x), static_cast<double>(value.y), static_cast<double>(value.z)};
        }

        [[nodiscard]] bool IsFinite(const DoubleVector &value) noexcept {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        [[nodiscard]] bool IsFinite(const DoubleTensor &value) noexcept {
            return std::isfinite(value.xx) && std::isfinite(value.yy) && std::isfinite(value.zz) && std::isfinite(value.xy) &&
                   std::isfinite(value.xz) && std::isfinite(value.yz);
        }

        [[nodiscard]] DoubleTensor AddTensor(const DoubleTensor &left, const DoubleTensor &right) noexcept {
            return {left.xx + right.xx, left.yy + right.yy, left.zz + right.zz, left.xy + right.xy, left.xz + right.xz, left.yz + right.yz};
        }

        [[nodiscard]] DoubleTensor SubtractTensor(const DoubleTensor &left, const DoubleTensor &right) noexcept {
            return {left.xx - right.xx, left.yy - right.yy, left.zz - right.zz, left.xy - right.xy, left.xz - right.xz, left.yz - right.yz};
        }

        [[nodiscard]] DoubleTensor ScaleTensor(const DoubleTensor &value, const double scalar) noexcept {
            return {value.xx * scalar, value.yy * scalar, value.zz * scalar, value.xy * scalar, value.xz * scalar, value.yz * scalar};
        }

        [[nodiscard]] DoubleVector AddVector(const DoubleVector &left, const DoubleVector &right) noexcept {
            return {left.x + right.x, left.y + right.y, left.z + right.z};
        }

        [[nodiscard]] DoubleVector ScaleVector(const DoubleVector &value, const double scalar) noexcept {
            return {value.x * scalar, value.y * scalar, value.z * scalar};
        }

        [[nodiscard]] DoubleTensor ParallelAxis(const double mass, const DoubleVector &offset) noexcept {
            const double squaredLength = offset.x * offset.x + offset.y * offset.y + offset.z * offset.z;
            return {mass * (squaredLength - offset.x * offset.x),
                    mass * (squaredLength - offset.y * offset.y),
                    mass * (squaredLength - offset.z * offset.z),
                    -mass * offset.x * offset.y,
                    -mass * offset.x * offset.z,
                    -mass * offset.y * offset.z};
        }

        [[nodiscard]] std::array<std::array<double, 3>, 3> MassPropertyRotationMatrix(const Math::Quaternion rotation) noexcept {
            const auto x = ToDouble(rotation.Rotate({1.0F, 0.0F, 0.0F}));
            const auto y = ToDouble(rotation.Rotate({0.0F, 1.0F, 0.0F}));
            const auto z = ToDouble(rotation.Rotate({0.0F, 0.0F, 1.0F}));
            return {{{x.x, y.x, z.x}, {x.y, y.y, z.y}, {x.z, y.z, z.z}}};
        }

        [[nodiscard]] double TensorEntry(const DoubleTensor &tensor, const std::size_t row, const std::size_t column) noexcept {
            if (row == 0 && column == 0)
                return tensor.xx;
            if (row == 1 && column == 1)
                return tensor.yy;
            if (row == 2 && column == 2)
                return tensor.zz;
            if ((row == 0 && column == 1) || (row == 1 && column == 0))
                return tensor.xy;
            if ((row == 0 && column == 2) || (row == 2 && column == 0))
                return tensor.xz;
            return tensor.yz;
        }

        [[nodiscard]] DoubleTensor RotateTensor(const DoubleTensor &tensor, const Math::Quaternion rotation) noexcept {
            const auto matrix = MassPropertyRotationMatrix(rotation);
            const auto rotatedEntry = [&matrix, &tensor](const std::size_t row, const std::size_t column) {
                double value{};
                for (std::size_t first = 0; first < 3; ++first)
                    for (std::size_t second = 0; second < 3; ++second)
                        value += matrix[row][first] * TensorEntry(tensor, first, second) * matrix[column][second];
                return value;
            };
            return {rotatedEntry(0, 0), rotatedEntry(1, 1), rotatedEntry(2, 2), rotatedEntry(0, 1), rotatedEntry(0, 2), rotatedEntry(1, 2)};
        }

        [[nodiscard]] bool FitsFloat(const double value) noexcept {
            return std::isfinite(value) && std::abs(value) <= static_cast<double>(std::numeric_limits<float>::max());
        }

        [[nodiscard]] Result<UnitDensityPrimitive> BoxPrimitive(const PhysicsBoxShape &box) {
            const double x = box.halfExtentsMeters.x;
            const double y = box.halfExtentsMeters.y;
            const double z = box.halfExtentsMeters.z;
            const double volume = 8.0 * x * y * z;
            return Result<UnitDensityPrimitive>::Success(
                {volume, {volume * (y * y + z * z) / 3.0, volume * (x * x + z * z) / 3.0, volume * (x * x + y * y) / 3.0, 0.0, 0.0, 0.0}});
        }

        [[nodiscard]] Result<UnitDensityPrimitive> SpherePrimitive(const PhysicsSphereShape &sphere) {
            const double radius = sphere.radiusMeters;
            const double volume = (4.0 / 3.0) * std::numbers::pi_v<double> * radius * radius * radius;
            const double inertia = 0.4 * volume * radius * radius;
            return Result<UnitDensityPrimitive>::Success({volume, {inertia, inertia, inertia, 0.0, 0.0, 0.0}});
        }

        [[nodiscard]] Result<UnitDensityPrimitive> CapsulePrimitive(const PhysicsCapsuleShape &capsule) {
            const double radius = capsule.radiusMeters;
            const double halfHeight = capsule.cylindricalHalfHeightMeters;
            const double pi = std::numbers::pi_v<double>;
            const double radiusSquared = radius * radius;
            const double radiusFourth = radiusSquared * radiusSquared;
            const double radiusFifth = radiusFourth * radius;
            const double volume = 2.0 * pi * radiusSquared * halfHeight + (4.0 / 3.0) * pi * radiusSquared * radius;
            const double axialInertia = pi * radiusFourth * halfHeight + (8.0 / 15.0) * pi * radiusFifth;
            const double transverseInertia = (2.0 / 3.0) * pi * radiusSquared * halfHeight * halfHeight * halfHeight +
                                             (4.0 / 3.0) * pi * halfHeight * halfHeight * radiusSquared * radius +
                                             1.5 * pi * halfHeight * radiusFourth + (8.0 / 15.0) * pi * radiusFifth;
            return Result<UnitDensityPrimitive>::Success({volume, {transverseInertia, axialInertia, transverseInertia, 0.0, 0.0, 0.0}});
        }

        [[nodiscard]] Result<UnitDensityPrimitive> Primitive(const PhysicsShapeDescriptor &descriptor) {
            return std::visit([]<typename Shape>(const Shape &shape) -> Result<UnitDensityPrimitive> {
                using ShapeType = std::decay_t<Shape>;
                if constexpr (std::is_same_v<ShapeType, PhysicsBoxShape>)
                    return BoxPrimitive(shape);
                else if constexpr (std::is_same_v<ShapeType, PhysicsSphereShape>)
                    return SpherePrimitive(shape);
                else if constexpr (std::is_same_v<ShapeType, PhysicsCapsuleShape>)
                    return CapsulePrimitive(shape);
                else
                    return Unsupported<UnitDensityPrimitive>("Static planes do not define finite mass or inertia properties.");
            }, descriptor);
        }

        [[nodiscard]] Result<void> ValidateAccumulated(const double volume, const DoubleVector &firstMoment, const DoubleTensor &inertia) {
            if (!std::isfinite(volume) || volume <= 0.0 || !IsFinite(firstMoment) || !IsFinite(inertia))
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Physics mass-property accumulation overflowed or became non-finite."));
            return Result<void>::Success();
        }

        [[nodiscard]] Result<PhysicsMassProperties> ConvertProperties(const double mass, const DoubleVector &center,
                                                                      const DoubleTensor &inertia) {
            if (!FitsFloat(mass) || !FitsFloat(center.x) || !FitsFloat(center.y) || !FitsFloat(center.z) || !FitsFloat(inertia.xx) ||
                !FitsFloat(inertia.yy) || !FitsFloat(inertia.zz) || !FitsFloat(inertia.xy) || !FitsFloat(inertia.xz) ||
                !FitsFloat(inertia.yz))
                return Invalid<PhysicsMassProperties>("Physics mass properties exceed finite float representation.");
            PhysicsMassProperties properties{static_cast<float>(mass),
                                             {static_cast<float>(center.x), static_cast<float>(center.y), static_cast<float>(center.z)},
                                             {static_cast<float>(inertia.xx), static_cast<float>(inertia.yy),
                                              static_cast<float>(inertia.zz), static_cast<float>(inertia.xy),
                                              static_cast<float>(inertia.xz), static_cast<float>(inertia.yz)}};
            if (const auto valid = ValidatePhysicsMassProperties(properties); valid.HasError())
                return Result<PhysicsMassProperties>::Failure(valid.ErrorValue());
            return Result<PhysicsMassProperties>::Success(properties);
        }

        [[nodiscard]] Result<double> DensityForPolicy(const PhysicsMassPropertiesPolicy &policy, const double volume) {
            return std::visit([volume]<typename Policy>(const Policy &value) {
                using PolicyType = std::decay_t<Policy>;
                if constexpr (std::is_same_v<PolicyType, PhysicsMass>) {
                    if (!std::isfinite(value.kilograms) || value.kilograms < MinimumPhysicsMassKilograms ||
                        value.kilograms > MaximumPhysicsMassKilograms)
                        return Invalid<double>("Explicit Physics mass is outside CanonicalV1 bounds.");
                    const double density = static_cast<double>(value.kilograms) / volume;
                    if (!std::isfinite(density) || density < MinimumPhysicsDensity || density > MaximumPhysicsDensity)
                        return Invalid<double>("Explicit Physics mass implies density outside CanonicalV1 bounds.");
                    return Result<double>::Success(density);
                } else if constexpr (std::is_same_v<PolicyType, PhysicsDensity>) {
                    if (!std::isfinite(value.kilogramsPerCubicMeter) || value.kilogramsPerCubicMeter < MinimumPhysicsDensity ||
                        value.kilogramsPerCubicMeter > MaximumPhysicsDensity)
                        return Invalid<double>("Explicit Physics density is outside CanonicalV1 bounds.");
                    return Result<double>::Success(static_cast<double>(value.kilogramsPerCubicMeter));
                } else {
                    return Invalid<double>("Mass-property overrides do not derive a compound density.");
                }
            }, policy);
        }
    }  // namespace

    /** @copydoc ValidatePhysicsInertiaTensor */
    Result<void> ValidatePhysicsInertiaTensor(const PhysicsInertiaTensor &inertia) {
        const double xx = inertia.xxKilogramSquareMeters;
        const double yy = inertia.yyKilogramSquareMeters;
        const double zz = inertia.zzKilogramSquareMeters;
        const double xy = inertia.xyKilogramSquareMeters;
        const double xz = inertia.xzKilogramSquareMeters;
        const double yz = inertia.yzKilogramSquareMeters;
        if (!std::isfinite(xx) || !std::isfinite(yy) || !std::isfinite(zz) || !std::isfinite(xy) || !std::isfinite(xz) ||
            !std::isfinite(yz))
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Physics inertia must contain only finite values."));
        if (xx <= 0.0 || yy <= 0.0 || zz <= 0.0)
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Physics inertia diagonal entries must be positive."));
        const double xyMinor = xx * yy - xy * xy;
        const double xzMinor = xx * zz - xz * xz;
        const double yzMinor = yy * zz - yz * yz;
        if (const double determinant = xx * yy * zz + 2.0 * xy * xz * yz - xx * yz * yz - yy * xz * xz - zz * xy * xy;
            !std::isfinite(xyMinor) || !std::isfinite(xzMinor) || !std::isfinite(yzMinor) || !std::isfinite(determinant) ||
            xyMinor <= 0.0 || xzMinor <= 0.0 || yzMinor <= 0.0 || determinant <= 0.0)
            return Result<void>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Physics inertia must be symmetric positive definite."));
        return Result<void>::Success();
    }

    /** @copydoc ValidatePhysicsMassProperties */
    Result<void> ValidatePhysicsMassProperties(const PhysicsMassProperties &properties) {
        if (!std::isfinite(properties.massKilograms) || properties.massKilograms < MinimumPhysicsMassKilograms ||
            properties.massKilograms > MaximumPhysicsMassKilograms)
            return Result<void>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Physics mass must be finite and within CanonicalV1 kilogram bounds."));
        if (!Math::IsFinite(properties.centerOfMassMeters))
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Physics center of mass must be finite."));
        return ValidatePhysicsInertiaTensor(properties.inertia);
    }

    /** @copydoc ResolvePhysicsMassProperties */
    Result<PhysicsMassProperties> ResolvePhysicsMassProperties(const PhysicsMassPropertiesRequest &request) {
        if (const auto *overrideProperties = std::get_if<PhysicsMassPropertiesOverride>(&request.policy)) {
            if (const auto valid = ValidatePhysicsMassProperties(overrideProperties->properties); valid.HasError())
                return Result<PhysicsMassProperties>::Failure(valid.ErrorValue());
            return Result<PhysicsMassProperties>::Success(overrideProperties->properties);
        }
        if (request.shapes.empty())
            return Invalid<PhysicsMassProperties>("Physics mass derivation requires at least one finite-volume contributor.");
        if (request.shapes.size() > MaximumPhysicsMassPropertyShapes)
            return Invalid<PhysicsMassProperties>("Physics mass derivation exceeds the bounded compound contributor limit.");

        DoubleVector firstMoment;
        DoubleTensor originInertia;
        double volume{};
        for (const PhysicsMassPropertiesShape &shape : request.shapes) {
            if (const auto descriptor = ValidatePhysicsShapeDescriptor(shape.geometry); descriptor.HasError())
                return Result<PhysicsMassProperties>::Failure(descriptor.ErrorValue());
            if (const auto pose = ValidatePhysicsPose(shape.localPose); pose.HasError())
                return Result<PhysicsMassProperties>::Failure(pose.ErrorValue());
            const auto primitive = Primitive(shape.geometry);
            if (primitive.HasError())
                return Result<PhysicsMassProperties>::Failure(primitive.ErrorValue());
            const UnitDensityPrimitive &unit = primitive.Value();
            if (!std::isfinite(unit.volume) || unit.volume <= 0.0 || !IsFinite(unit.inertia))
                return Invalid<PhysicsMassProperties>("Physics contributor volume or inertia is non-finite or non-positive.");
            const DoubleVector position = ToDouble(shape.localPose.translation);
            const DoubleTensor rotatedInertia = RotateTensor(unit.inertia, shape.localPose.rotation);
            const DoubleTensor shiftedInertia = AddTensor(rotatedInertia, ParallelAxis(unit.volume, position));
            volume += unit.volume;
            firstMoment = AddVector(firstMoment, ScaleVector(position, unit.volume));
            originInertia = AddTensor(originInertia, shiftedInertia);
            if (const auto valid = ValidateAccumulated(volume, firstMoment, originInertia); valid.HasError())
                return Result<PhysicsMassProperties>::Failure(valid.ErrorValue());
        }

        const auto density = DensityForPolicy(request.policy, volume);
        if (density.HasError())
            return Result<PhysicsMassProperties>::Failure(density.ErrorValue());
        const double scale = density.Value();
        const double mass = scale * volume;
        const DoubleVector center = ScaleVector(firstMoment, 1.0 / volume);
        const DoubleTensor inertia = SubtractTensor(ScaleTensor(originInertia, scale), ParallelAxis(mass, center));
        return ConvertProperties(mass, center, inertia);
    }
}  // namespace Horo::Physics
