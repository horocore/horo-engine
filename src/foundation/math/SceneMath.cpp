/** @copydoc SceneMath.h */

#include "Horo/Math/SceneMath.h"

#include "../FoundationErrors.h"
#include "Horo/Foundation/Assertions.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <ranges>
#include <string>
#include <utility>

namespace Horo::Math {
    namespace {
        [[nodiscard]] Error MakeMathError(const ErrorCodeDescriptor &descriptor, const char *message) {
            return MakeError(descriptor, message);
        }

        [[nodiscard]] bool IsValidEpsilon(const float epsilon) noexcept {
            return std::isfinite(epsilon) && epsilon >= 0.0F;
        }

        [[nodiscard]] bool IsAffine(const Mat4 &matrix) noexcept {
            return IsFinite(matrix) && matrix.At(3, 0) == 0.0F && matrix.At(3, 1) == 0.0F && matrix.At(3, 2) == 0.0F &&
                   matrix.At(3, 3) == 1.0F;
        }

        [[nodiscard]] bool IsValidQuaternion(const Quaternion value) noexcept {
            return IsFinite(value) && LengthSquared(Vec4{value.x, value.y, value.z, value.w}) > DefaultEpsilon * DefaultEpsilon;
        }

        [[nodiscard]] bool IsValidRay(const Ray &ray) noexcept {
            return IsFinite(ray.origin) && IsFinite(ray.direction) && std::isfinite(ray.minimumDistance) &&
                   std::isfinite(ray.maximumDistance) && ray.minimumDistance >= 0.0F && ray.maximumDistance >= ray.minimumDistance &&
                   NearlyEqual(LengthSquared(ray.direction), 1.0F, 0.0001F);
        }
    }  // namespace

    bool IsFinite(const Vec2 value) noexcept {
        return std::isfinite(value.x) && std::isfinite(value.y);
    }

    bool IsFinite(const Vec3 value) noexcept {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
    }

    bool IsFinite(const Vec4 value) noexcept {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
    }

    float Length(const Vec2 value) noexcept {
        return std::sqrt(LengthSquared(value));
    }

    float Length(const Vec3 value) noexcept {
        return std::sqrt(LengthSquared(value));
    }

    float Length(const Vec4 value) noexcept {
        return std::sqrt(LengthSquared(value));
    }

    Vec2 Normalize(const Vec2 value) noexcept {
        HORO_ASSERT(IsFinite(value) && LengthSquared(value) > DefaultEpsilon * DefaultEpsilon);
        return value / Length(value);
    }

    Vec3 Normalize(const Vec3 value) noexcept {
        HORO_ASSERT(IsFinite(value) && LengthSquared(value) > DefaultEpsilon * DefaultEpsilon);
        return value / Length(value);
    }

    Vec4 Normalize(const Vec4 value) noexcept {
        HORO_ASSERT(IsFinite(value) && LengthSquared(value) > DefaultEpsilon * DefaultEpsilon);
        return value / Length(value);
    }

    Result<Vec2> TryNormalize(const Vec2 value) noexcept {
        if (!IsFinite(value))
            return Result<Vec2>::Failure(MakeMathError(Errors::NonFiniteInput, "Vector must be finite."));
        const double length = std::hypot(static_cast<double>(value.x), static_cast<double>(value.y));
        if (length <= static_cast<double>(DefaultEpsilon))
            return Result<Vec2>::Failure(MakeMathError(Errors::ZeroLength, "Vector length is too small to normalize."));
        return Result<Vec2>::Success(
            {static_cast<float>(static_cast<double>(value.x) / length), static_cast<float>(static_cast<double>(value.y) / length)});
    }

    Result<Vec3> TryNormalize(const Vec3 value) noexcept {
        if (!IsFinite(value))
            return Result<Vec3>::Failure(MakeMathError(Errors::NonFiniteInput, "Vector must be finite."));
        const double length = std::hypot(static_cast<double>(value.x), static_cast<double>(value.y), static_cast<double>(value.z));
        if (length <= static_cast<double>(DefaultEpsilon))
            return Result<Vec3>::Failure(MakeMathError(Errors::ZeroLength, "Vector length is too small to normalize."));
        return Result<Vec3>::Success({static_cast<float>(static_cast<double>(value.x) / length),
                                      static_cast<float>(static_cast<double>(value.y) / length),
                                      static_cast<float>(static_cast<double>(value.z) / length)});
    }

    Result<Vec4> TryNormalize(const Vec4 value) noexcept {
        if (!IsFinite(value))
            return Result<Vec4>::Failure(MakeMathError(Errors::NonFiniteInput, "Vector must be finite."));
        const double length = std::hypot(std::hypot(static_cast<double>(value.x), static_cast<double>(value.y)),
                                         std::hypot(static_cast<double>(value.z), static_cast<double>(value.w)));
        if (length <= static_cast<double>(DefaultEpsilon))
            return Result<Vec4>::Failure(MakeMathError(Errors::ZeroLength, "Vector length is too small to normalize."));
        return Result<Vec4>::Success(
            {static_cast<float>(static_cast<double>(value.x) / length), static_cast<float>(static_cast<double>(value.y) / length),
             static_cast<float>(static_cast<double>(value.z) / length), static_cast<float>(static_cast<double>(value.w) / length)});
    }

    bool NearlyEqual(const float lhs, const float rhs, const float epsilon) noexcept {
        return IsValidEpsilon(epsilon) && std::isfinite(lhs) && std::isfinite(rhs) && std::fabs(lhs - rhs) <= epsilon;
    }

    bool NearlyEqual(const Vec2 lhs, const Vec2 rhs, const float epsilon) noexcept {
        return NearlyEqual(lhs.x, rhs.x, epsilon) && NearlyEqual(lhs.y, rhs.y, epsilon);
    }

    bool NearlyEqual(const Vec3 lhs, const Vec3 rhs, const float epsilon) noexcept {
        return NearlyEqual(lhs.x, rhs.x, epsilon) && NearlyEqual(lhs.y, rhs.y, epsilon) && NearlyEqual(lhs.z, rhs.z, epsilon);
    }

    bool NearlyEqual(const Vec4 lhs, const Vec4 rhs, const float epsilon) noexcept {
        return NearlyEqual(lhs.x, rhs.x, epsilon) && NearlyEqual(lhs.y, rhs.y, epsilon) && NearlyEqual(lhs.z, rhs.z, epsilon) &&
               NearlyEqual(lhs.w, rhs.w, epsilon);
    }

    Quaternion Quaternion::FromAxisAngle(const Vec3 axis, const float radians) noexcept {
        auto result = TryFromAxisAngle(axis, radians);
        HORO_ASSERT(result.HasValue());
        return std::move(result).Value();
    }

    Result<Quaternion> Quaternion::TryFromAxisAngle(const Vec3 axis, const float radians) noexcept {
        if (!std::isfinite(radians))
            return Result<Quaternion>::Failure(MakeMathError(Errors::NonFiniteInput, "Rotation angle must be finite."));
        auto normalized = TryNormalize(axis);
        if (normalized.HasError())
            return Result<Quaternion>::Failure(normalized.ErrorValue());
        const float halfAngle = radians * 0.5F;
        const float sine = std::sin(halfAngle);
        const Vec3 unitAxis = normalized.Value();
        return Result<Quaternion>::Success(Quaternion{unitAxis.x * sine, unitAxis.y * sine, unitAxis.z * sine, std::cos(halfAngle)});
    }

    Quaternion Quaternion::FromEulerRadians(const Vec3 radians) noexcept {
        auto result = TryFromEulerRadians(radians);
        HORO_ASSERT(result.HasValue());
        return std::move(result).Value();
    }

    Result<Quaternion> Quaternion::TryFromEulerRadians(const Vec3 radians) noexcept {
        if (!IsFinite(radians))
            return Result<Quaternion>::Failure(MakeMathError(Errors::NonFiniteInput, "Euler angles must be finite."));
        const Quaternion xRotation = FromAxisAngle({1.0F, 0.0F, 0.0F}, radians.x);
        const Quaternion yRotation = FromAxisAngle({0.0F, 1.0F, 0.0F}, radians.y);
        const Quaternion zRotation = FromAxisAngle({0.0F, 0.0F, 1.0F}, radians.z);
        return Result<Quaternion>::Success((zRotation * yRotation * xRotation).Normalized());
    }

    Vec3 Quaternion::ToEulerRadians() const noexcept {
        const Quaternion value = Normalized();
        const float xAngle =
            std::atan2(2.0F * (value.w * value.x + value.y * value.z), 1.0F - 2.0F * (value.x * value.x + value.y * value.y));
        const float ySine = std::clamp(2.0F * (value.w * value.y - value.z * value.x), -1.0F, 1.0F);
        const float zAngle =
            std::atan2(2.0F * (value.w * value.z + value.x * value.y), 1.0F - 2.0F * (value.y * value.y + value.z * value.z));
        return {xAngle, std::asin(ySine), zAngle};
    }

    Quaternion Quaternion::Normalized() const noexcept {
        HORO_ASSERT(IsValidQuaternion(*this));
        const float inverseLength = 1.0F / std::sqrt(x * x + y * y + z * z + w * w);
        return {x * inverseLength, y * inverseLength, z * inverseLength, w * inverseLength};
    }

    Result<Quaternion> Quaternion::TryNormalized() const noexcept {
        if (!IsFinite(*this))
            return Result<Quaternion>::Failure(MakeMathError(Errors::NonFiniteInput, "Quaternion must be finite."));
        const Result<Vec4> normalized = TryNormalize({x, y, z, w});
        if (normalized.HasError())
            return Result<Quaternion>::Failure(normalized.ErrorValue());
        const Vec4 value = normalized.Value();
        return Result<Quaternion>::Success({value.x, value.y, value.z, value.w});
    }

    Quaternion Quaternion::Inverse() const noexcept {
        HORO_ASSERT(IsValidQuaternion(*this));
        const float inverseLengthSquared = 1.0F / (x * x + y * y + z * z + w * w);
        return {-x * inverseLengthSquared, -y * inverseLengthSquared, -z * inverseLengthSquared, w * inverseLengthSquared};
    }

    Result<Quaternion> Quaternion::TryInverse() const noexcept {
        if (!IsFinite(*this))
            return Result<Quaternion>::Failure(MakeMathError(Errors::NonFiniteInput, "Quaternion must be finite."));
        if (!IsValidQuaternion(*this))
            return Result<Quaternion>::Failure(MakeMathError(Errors::ZeroLength, "Quaternion length is too small."));
        return Result<Quaternion>::Success(Inverse());
    }

    Vec3 Quaternion::Rotate(const Vec3 value) const noexcept {
        HORO_ASSERT(IsValidQuaternion(*this) && IsFinite(value));
        const Quaternion rotation = Normalized();
        const Quaternion vector{value.x, value.y, value.z, 0.0F};
        const Quaternion result = rotation * vector * rotation.Inverse();
        return {result.x, result.y, result.z};
    }

    Result<Vec3> Quaternion::TryRotate(const Vec3 value) const noexcept {
        if (!IsFinite(value) || !IsFinite(*this))
            return Result<Vec3>::Failure(MakeMathError(Errors::NonFiniteInput, "Rotation inputs must be finite."));
        if (!IsValidQuaternion(*this))
            return Result<Vec3>::Failure(MakeMathError(Errors::ZeroLength, "Quaternion length is too small."));
        return Result<Vec3>::Success(Rotate(value));
    }

    bool IsFinite(const Quaternion value) noexcept {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
    }

    Quaternion Nlerp(const Quaternion from, Quaternion to, const float alpha) noexcept {
        HORO_ASSERT(IsValidQuaternion(from) && IsValidQuaternion(to) && std::isfinite(alpha));
        if (from.x * to.x + from.y * to.y + from.z * to.z + from.w * to.w < 0.0F)
            to = {-to.x, -to.y, -to.z, -to.w};
        return Quaternion{from.x + (to.x - from.x) * alpha, from.y + (to.y - from.y) * alpha, from.z + (to.z - from.z) * alpha,
                          from.w + (to.w - from.w) * alpha}
            .Normalized();
    }

    Quaternion Slerp(const Quaternion from, Quaternion to, const float alpha) noexcept {
        HORO_ASSERT(IsValidQuaternion(from) && IsValidQuaternion(to) && std::isfinite(alpha));
        Quaternion first = from.Normalized();
        to = to.Normalized();
        float cosine = first.x * to.x + first.y * to.y + first.z * to.z + first.w * to.w;
        if (cosine < 0.0F) {
            to = {-to.x, -to.y, -to.z, -to.w};
            cosine = -cosine;
        }
        cosine = std::clamp(cosine, -1.0F, 1.0F);
        if (cosine > 0.9995F)
            return Nlerp(first, to, alpha);
        const float angle = std::acos(cosine);
        const float inverseSine = 1.0F / std::sin(angle);
        const float firstWeight = std::sin((1.0F - alpha) * angle) * inverseSine;
        const float secondWeight = std::sin(alpha * angle) * inverseSine;
        return Quaternion{first.x * firstWeight + to.x * secondWeight, first.y * firstWeight + to.y * secondWeight,
                          first.z * firstWeight + to.z * secondWeight, first.w * firstWeight + to.w * secondWeight}
            .Normalized();
    }

    Result<Quaternion> TryNlerp(const Quaternion from, const Quaternion to, const float alpha) noexcept {
        if (!std::isfinite(alpha) || !IsFinite(from) || !IsFinite(to))
            return Result<Quaternion>::Failure(MakeMathError(Errors::NonFiniteInput, "Interpolation inputs must be finite."));
        if (!IsValidQuaternion(from) || !IsValidQuaternion(to))
            return Result<Quaternion>::Failure(MakeMathError(Errors::ZeroLength, "Interpolation quaternion is invalid."));
        return Result<Quaternion>::Success(Nlerp(from, to, alpha));
    }

    Result<Quaternion> TrySlerp(const Quaternion from, const Quaternion to, const float alpha) noexcept {
        if (!std::isfinite(alpha) || !IsFinite(from) || !IsFinite(to))
            return Result<Quaternion>::Failure(MakeMathError(Errors::NonFiniteInput, "Interpolation inputs must be finite."));
        if (!IsValidQuaternion(from) || !IsValidQuaternion(to))
            return Result<Quaternion>::Failure(MakeMathError(Errors::ZeroLength, "Interpolation quaternion is invalid."));
        return Result<Quaternion>::Success(Slerp(from, to, alpha));
    }

    Mat4 Transform::ToMatrix() const noexcept {
        auto result = TryToMatrix();
        HORO_ASSERT(result.HasValue());
        return std::move(result).Value();
    }

    Result<Mat4> Transform::TryToMatrix() const noexcept {
        if (!IsFinite(translation) || !IsFinite(scale) || !IsValidQuaternion(rotation))
            return Result<Mat4>::Failure(MakeMathError(Errors::NonFiniteInput, "Transform values are invalid."));
        return Result<Mat4>::Success(Multiply(TranslationMatrix(translation), Multiply(RotationMatrix(rotation), ScaleMatrix(scale))));
    }

    bool IsFinite(const Mat4 &value) noexcept {
        return std::ranges::all_of(value.values, [](const float component) {
            return std::isfinite(component);
        });
    }

    Mat4 Multiply(const Mat4 &lhs, const Mat4 &rhs) noexcept {
        Mat4 result{};
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                for (int index = 0; index < 4; ++index)
                    result.values[static_cast<std::size_t>(column * 4 + row)] += lhs.At(row, index) * rhs.At(index, column);
        return result;
    }

    Mat4 TranslationMatrix(const Vec3 value) noexcept {
        HORO_ASSERT(IsFinite(value));
        Mat4 result = Mat4::Identity();
        result.values[12] = value.x;
        result.values[13] = value.y;
        result.values[14] = value.z;
        return result;
    }

    Mat4 ScaleMatrix(const Vec3 value) noexcept {
        HORO_ASSERT(IsFinite(value));
        Mat4 result = Mat4::Identity();
        result.values[0] = value.x;
        result.values[5] = value.y;
        result.values[10] = value.z;
        return result;
    }

    Mat4 RotationMatrix(const Quaternion value) noexcept {
        HORO_ASSERT(IsValidQuaternion(value));
        const Quaternion q = value.Normalized();
        return Mat4{{1.0F - 2.0F * (q.y * q.y + q.z * q.z), 2.0F * (q.x * q.y + q.w * q.z), 2.0F * (q.x * q.z - q.w * q.y), 0.0F,
                     2.0F * (q.x * q.y - q.w * q.z), 1.0F - 2.0F * (q.x * q.x + q.z * q.z), 2.0F * (q.y * q.z + q.w * q.x), 0.0F,
                     2.0F * (q.x * q.z + q.w * q.y), 2.0F * (q.y * q.z - q.w * q.x), 1.0F - 2.0F * (q.x * q.x + q.y * q.y), 0.0F, 0.0F,
                     0.0F, 0.0F, 1.0F}};
    }

    namespace {
        using AugmentedMat4 = std::array<std::array<float, 8>, 4>;

        [[nodiscard]] bool EliminatePivot(AugmentedMat4 &augmented, const int pivot) noexcept {
            int best = pivot;
            for (int row = pivot + 1; row < 4; ++row) {
                if (std::fabs(augmented[row][pivot]) > std::fabs(augmented[best][pivot]))
                    best = row;
            }
            if (std::fabs(augmented[best][pivot]) <= DefaultEpsilon)
                return false;
            if (best != pivot)
                std::swap(augmented[best], augmented[pivot]);
            const float divisor = augmented[pivot][pivot];
            for (float &value : augmented[pivot])
                value /= divisor;
            for (int row = 0; row < 4; ++row) {
                if (row == pivot)
                    continue;
                const float factor = augmented[row][pivot];
                for (int column = 0; column < 8; ++column)
                    augmented[row][column] -= factor * augmented[pivot][column];
            }
            return true;
        }
    }  // namespace

    Result<Mat4> TryInverse(const Mat4 &matrix) noexcept {
        if (!IsFinite(matrix))
            return Result<Mat4>::Failure(MakeMathError(Errors::NonFiniteInput, "Matrix must be finite."));
        AugmentedMat4 augmented{};
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column)
                augmented[row][column] = matrix.At(row, column);
            augmented[row][row + 4] = 1.0F;
        }
        for (int pivot = 0; pivot < 4; ++pivot) {
            if (!EliminatePivot(augmented, pivot))
                return Result<Mat4>::Failure(MakeMathError(Errors::SingularMatrix, "Matrix is singular."));
        }
        Mat4 inverse{};
        for (int row = 0; row < 4; ++row) {
            for (int column = 0; column < 4; ++column)
                inverse.values[static_cast<std::size_t>(column * 4 + row)] = augmented[row][column + 4];
        }
        if (!IsFinite(inverse))
            return Result<Mat4>::Failure(MakeMathError(Errors::SingularMatrix, "Matrix inverse is not representable."));
        return Result<Mat4>::Success(inverse);
    }

    Result<Mat4> TryInverseAffine(const Mat4 &matrix) noexcept {
        if (!IsAffine(matrix))
            return Result<Mat4>::Failure(MakeMathError(Errors::InvalidAffineMatrix, "Matrix must be finite and affine."));
        const float a00 = matrix.At(0, 0);
        const float a01 = matrix.At(0, 1);
        const float a02 = matrix.At(0, 2);
        const float a10 = matrix.At(1, 0);
        const float a11 = matrix.At(1, 1);
        const float a12 = matrix.At(1, 2);
        const float a20 = matrix.At(2, 0);
        const float a21 = matrix.At(2, 1);
        const float a22 = matrix.At(2, 2);
        const float determinant = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20) + a02 * (a10 * a21 - a11 * a20);
        if (!std::isfinite(determinant) || std::fabs(determinant) <= DefaultEpsilon)
            return Result<Mat4>::Failure(MakeMathError(Errors::SingularMatrix, "Affine matrix is singular."));
        const float inverseDeterminant = 1.0F / determinant;
        Mat4 inverse = Mat4::Identity();
        inverse.values[0] = (a11 * a22 - a12 * a21) * inverseDeterminant;
        inverse.values[4] = (a02 * a21 - a01 * a22) * inverseDeterminant;
        inverse.values[8] = (a01 * a12 - a02 * a11) * inverseDeterminant;
        inverse.values[1] = (a12 * a20 - a10 * a22) * inverseDeterminant;
        inverse.values[5] = (a00 * a22 - a02 * a20) * inverseDeterminant;
        inverse.values[9] = (a02 * a10 - a00 * a12) * inverseDeterminant;
        inverse.values[2] = (a10 * a21 - a11 * a20) * inverseDeterminant;
        inverse.values[6] = (a01 * a20 - a00 * a21) * inverseDeterminant;
        inverse.values[10] = (a00 * a11 - a01 * a10) * inverseDeterminant;
        const Vec3 translation{matrix.At(0, 3), matrix.At(1, 3), matrix.At(2, 3)};
        const Vec3 inverseTranslation = -TransformDirection(inverse, translation);
        inverse.values[12] = inverseTranslation.x;
        inverse.values[13] = inverseTranslation.y;
        inverse.values[14] = inverseTranslation.z;
        return Result<Mat4>::Success(inverse);
    }

    Result<Transform> TryDecomposeAffineTRS(const Mat4 &matrix) noexcept {
        if (!IsAffine(matrix))
            return Result<Transform>::Failure(MakeMathError(Errors::InvalidAffineMatrix, "Matrix must be finite and affine."));
        Vec3 xAxis{matrix.At(0, 0), matrix.At(1, 0), matrix.At(2, 0)};
        Vec3 yAxis{matrix.At(0, 1), matrix.At(1, 1), matrix.At(2, 1)};
        const Vec3 zOriginal{matrix.At(0, 2), matrix.At(1, 2), matrix.At(2, 2)};
        float scaleX = Length(xAxis);
        if (!std::isfinite(scaleX) || scaleX <= DefaultEpsilon)
            return Result<Transform>::Failure(MakeMathError(Errors::SingularMatrix, "Matrix has a singular X basis."));
        xAxis = Normalize(xAxis);
        yAxis -= xAxis * Dot(xAxis, yAxis);
        const float scaleY = Length(yAxis);
        if (!std::isfinite(scaleY) || scaleY <= DefaultEpsilon)
            return Result<Transform>::Failure(MakeMathError(Errors::SingularMatrix, "Matrix has a singular Y basis."));
        yAxis = Normalize(yAxis);
        Vec3 zAxis = Normalize(Cross(xAxis, yAxis));
        float scaleZ = Dot(zOriginal, zAxis);
        if (!std::isfinite(scaleZ) || std::fabs(scaleZ) <= DefaultEpsilon)
            return Result<Transform>::Failure(MakeMathError(Errors::SingularMatrix, "Matrix has a singular Z basis."));
        if (scaleZ < 0.0F) {
            scaleX = -scaleX;
            xAxis = -xAxis;
            zAxis = Normalize(Cross(xAxis, yAxis));
            scaleZ = Dot(zOriginal, zAxis);
        }
        const float trace = xAxis.x + yAxis.y + zAxis.z;
        Quaternion rotation;
        if (trace > 0.0F) {
            const float s = std::sqrt(trace + 1.0F) * 2.0F;
            rotation = {(yAxis.z - zAxis.y) / s, (zAxis.x - xAxis.z) / s, (xAxis.y - yAxis.x) / s, 0.25F * s};
        } else if (xAxis.x > yAxis.y && xAxis.x > zAxis.z) {
            const float s = std::sqrt(1.0F + xAxis.x - yAxis.y - zAxis.z) * 2.0F;
            rotation = {0.25F * s, (yAxis.x + xAxis.y) / s, (zAxis.x + xAxis.z) / s, (yAxis.z - zAxis.y) / s};
        } else if (yAxis.y > zAxis.z) {
            const float s = std::sqrt(1.0F + yAxis.y - xAxis.x - zAxis.z) * 2.0F;
            rotation = {(yAxis.x + xAxis.y) / s, 0.25F * s, (zAxis.y + yAxis.z) / s, (zAxis.x - xAxis.z) / s};
        } else {
            const float s = std::sqrt(1.0F + zAxis.z - xAxis.x - yAxis.y) * 2.0F;
            rotation = {(zAxis.x + xAxis.z) / s, (zAxis.y + yAxis.z) / s, 0.25F * s, (xAxis.y - yAxis.x) / s};
        }
        return Result<Transform>::Success(Transform{.translation = {matrix.At(0, 3), matrix.At(1, 3), matrix.At(2, 3)},
                                                    .rotation = rotation.Normalized(),
                                                    .scale = {scaleX, scaleY, scaleZ}});
    }

    Mat4 LookAt(const Vec3 eye, const Vec3 target, const Vec3 up) noexcept {
        auto result = TryLookAt(eye, target, up);
        HORO_ASSERT(result.HasValue());
        return std::move(result).Value();
    }

    Result<Mat4> TryLookAt(const Vec3 eye, const Vec3 target, const Vec3 up) noexcept {
        if (!IsFinite(eye) || !IsFinite(target) || !IsFinite(up))
            return Result<Mat4>::Failure(MakeMathError(Errors::NonFiniteInput, "View inputs must be finite."));
        auto forwardResult = TryNormalize(target - eye);
        if (forwardResult.HasError())
            return Result<Mat4>::Failure(MakeMathError(Errors::InvalidView, "View eye and target must differ."));
        auto sideResult = TryNormalize(Cross(forwardResult.Value(), up));
        if (sideResult.HasError())
            return Result<Mat4>::Failure(MakeMathError(Errors::InvalidView, "View up must not be parallel to forward."));
        const Vec3 forward = forwardResult.Value();
        const Vec3 side = sideResult.Value();
        const Vec3 correctedUp = Cross(side, forward);
        return Result<Mat4>::Success(
            Mat4{{side.x, correctedUp.x, -forward.x, 0.0F, side.y, correctedUp.y, -forward.y, 0.0F, side.z, correctedUp.z, -forward.z, 0.0F,
                  -Dot(side, eye), -Dot(correctedUp, eye), Dot(forward, eye), 1.0F}});
    }

    Mat4 Perspective(const float verticalFovRadians, const float aspect, const float nearPlane, const float farPlane,
                     const ClipDepthRange depthRange) noexcept {
        auto result = TryPerspective(verticalFovRadians, aspect, nearPlane, farPlane, depthRange);
        HORO_ASSERT(result.HasValue());
        return std::move(result).Value();
    }

    Result<Mat4> TryPerspective(const float verticalFovRadians, const float aspect, const float nearPlane, const float farPlane,
                                const ClipDepthRange depthRange) noexcept {
        if (!std::isfinite(verticalFovRadians) || !std::isfinite(aspect) || !std::isfinite(nearPlane) || !std::isfinite(farPlane))
            return Result<Mat4>::Failure(MakeMathError(Errors::NonFiniteInput, "Projection inputs must be finite."));
        if (verticalFovRadians <= 0.0F || verticalFovRadians >= Pi || aspect <= 0.0F || nearPlane <= 0.0F || farPlane <= nearPlane)
            return Result<Mat4>::Failure(MakeMathError(Errors::InvalidProjection, "Perspective parameters are invalid."));
        const float focal = 1.0F / std::tan(verticalFovRadians * 0.5F);
        Mat4 result{};
        result.values[0] = focal / aspect;
        result.values[5] = focal;
        result.values[11] = -1.0F;
        if (depthRange == ClipDepthRange::NegativeOneToOne) {
            result.values[10] = (farPlane + nearPlane) / (nearPlane - farPlane);
            result.values[14] = (2.0F * farPlane * nearPlane) / (nearPlane - farPlane);
        } else {
            result.values[10] = farPlane / (nearPlane - farPlane);
            result.values[14] = (farPlane * nearPlane) / (nearPlane - farPlane);
        }
        return Result<Mat4>::Success(result);
    }

    Mat4 Orthographic(const float verticalHeight, const float aspect, const float nearPlane, const float farPlane,
                      const ClipDepthRange depthRange) noexcept {
        auto result = TryOrthographic(verticalHeight, aspect, nearPlane, farPlane, depthRange);
        HORO_ASSERT(result.HasValue());
        return std::move(result).Value();
    }

    Result<Mat4> TryOrthographic(const float verticalHeight, const float aspect, const float nearPlane, const float farPlane,
                                 const ClipDepthRange depthRange) noexcept {
        if (!std::isfinite(verticalHeight) || !std::isfinite(aspect) || !std::isfinite(nearPlane) || !std::isfinite(farPlane))
            return Result<Mat4>::Failure(MakeMathError(Errors::NonFiniteInput, "Projection inputs must be finite."));
        if (verticalHeight <= 0.0F || aspect <= 0.0F || nearPlane <= 0.0F || farPlane <= nearPlane)
            return Result<Mat4>::Failure(MakeMathError(Errors::InvalidProjection, "Orthographic parameters are invalid."));
        const float width = verticalHeight * aspect;
        Mat4 result = Mat4::Identity();
        result.values[0] = 2.0F / width;
        result.values[5] = 2.0F / verticalHeight;
        if (depthRange == ClipDepthRange::NegativeOneToOne) {
            result.values[10] = -2.0F / (farPlane - nearPlane);
            result.values[14] = -(farPlane + nearPlane) / (farPlane - nearPlane);
        } else {
            result.values[10] = -1.0F / (farPlane - nearPlane);
            result.values[14] = -nearPlane / (farPlane - nearPlane);
        }
        return Result<Mat4>::Success(result);
    }

    Vec4 TransformHomogeneous(const Mat4 &matrix, const Vec4 value) noexcept {
        return {matrix.At(0, 0) * value.x + matrix.At(0, 1) * value.y + matrix.At(0, 2) * value.z + matrix.At(0, 3) * value.w,
                matrix.At(1, 0) * value.x + matrix.At(1, 1) * value.y + matrix.At(1, 2) * value.z + matrix.At(1, 3) * value.w,
                matrix.At(2, 0) * value.x + matrix.At(2, 1) * value.y + matrix.At(2, 2) * value.z + matrix.At(2, 3) * value.w,
                matrix.At(3, 0) * value.x + matrix.At(3, 1) * value.y + matrix.At(3, 2) * value.z + matrix.At(3, 3) * value.w};
    }

    Vec3 TransformAffinePoint(const Mat4 &matrix, const Vec3 point) noexcept {
        HORO_ASSERT(IsAffine(matrix) && IsFinite(point));
        const Vec4 result = TransformHomogeneous(matrix, {point.x, point.y, point.z, 1.0F});
        return {result.x, result.y, result.z};
    }

    Vec3 TransformDirection(const Mat4 &matrix, const Vec3 direction) noexcept {
        HORO_ASSERT(IsFinite(matrix) && IsFinite(direction));
        const Vec4 result = TransformHomogeneous(matrix, {direction.x, direction.y, direction.z, 0.0F});
        return {result.x, result.y, result.z};
    }

    Vec3 TransformPoint(const Mat4 &matrix, const Vec3 point) noexcept {
        auto result = TryTransformPoint(matrix, point);
        HORO_ASSERT(result.HasValue());
        return std::move(result).Value();
    }

    Result<Vec3> TryTransformPoint(const Mat4 &matrix, const Vec3 point) noexcept {
        if (!IsFinite(matrix) || !IsFinite(point))
            return Result<Vec3>::Failure(MakeMathError(Errors::NonFiniteInput, "Transform inputs must be finite."));
        const Vec4 result = TransformHomogeneous(matrix, {point.x, point.y, point.z, 1.0F});
        if (!IsFinite(result) || std::fabs(result.w) <= DefaultEpsilon)
            return Result<Vec3>::Failure(MakeMathError(Errors::InvalidHomogeneousPoint, "Homogeneous point has invalid W."));
        return Result<Vec3>::Success({result.x / result.w, result.y / result.w, result.z / result.w});
    }

    Result<Vec3> TryProject(const Mat4 &viewProjection, const Vec3 worldPoint) noexcept {
        if (!IsFinite(viewProjection) || !IsFinite(worldPoint))
            return Result<Vec3>::Failure(MakeMathError(Errors::NonFiniteInput, "Projection inputs must be finite."));
        const Vec4 clip = TransformHomogeneous(viewProjection, {worldPoint.x, worldPoint.y, worldPoint.z, 1.0F});
        if (!IsFinite(clip) || clip.w <= DefaultEpsilon)
            return Result<Vec3>::Failure(MakeMathError(Errors::InvalidProjection, "Projected point must be in front of the view origin."));
        return Result<Vec3>::Success({clip.x / clip.w, clip.y / clip.w, clip.z / clip.w});
    }

    Result<Vec3> TryUnproject(const Mat4 &inverseViewProjection, const Vec3 ndcPoint) noexcept {
        return TryTransformPoint(inverseViewProjection, ndcPoint);
    }

    Result<Ray> TryMakeRay(const Vec3 origin, const Vec3 direction, const float minimumDistance, const float maximumDistance) noexcept {
        if (!IsFinite(origin) || !IsFinite(direction) || !std::isfinite(minimumDistance) || !std::isfinite(maximumDistance) ||
            minimumDistance < 0.0F || maximumDistance < minimumDistance)
            return Result<Ray>::Failure(MakeMathError(Errors::InvalidRay, "Ray range and values must be finite and ordered."));
        auto normalized = TryNormalize(direction);
        if (normalized.HasError())
            return Result<Ray>::Failure(MakeMathError(Errors::InvalidRay, "Ray direction must be non-zero."));
        return Result<Ray>::Success({origin, normalized.Value(), minimumDistance, maximumDistance});
    }

    Result<Plane> TryMakePlane(const Vec3 point, const Vec3 normal) noexcept {
        if (!IsFinite(point) || !IsFinite(normal))
            return Result<Plane>::Failure(MakeMathError(Errors::NonFiniteInput, "Plane inputs must be finite."));
        auto normalized = TryNormalize(normal);
        if (normalized.HasError())
            return Result<Plane>::Failure(MakeMathError(Errors::InvalidPlane, "Plane normal must be non-zero."));
        return Result<Plane>::Success({normalized.Value(), -Dot(normalized.Value(), point)});
    }

    bool Aabb::IsValid() const noexcept {
        return IsFinite(minimum) && IsFinite(maximum) && minimum.x <= maximum.x && minimum.y <= maximum.y && minimum.z <= maximum.z;
    }

    bool BoundingSphere::IsValid() const noexcept {
        return IsFinite(center) && std::isfinite(radius) && radius >= 0.0F;
    }

    Result<Aabb> TransformAabb(const Aabb &bounds, const Mat4 &localToWorld) noexcept {
        if (!bounds.IsValid())
            return Result<Aabb>::Failure(MakeMathError(Errors::InvalidBounds, "AABB minimum and maximum are invalid."));
        if (!IsAffine(localToWorld))
            return Result<Aabb>::Failure(MakeMathError(Errors::InvalidAffineMatrix, "AABB transform must be affine."));
        const std::array corners{
            Vec3{bounds.minimum.x, bounds.minimum.y, bounds.minimum.z}, Vec3{bounds.maximum.x, bounds.minimum.y, bounds.minimum.z},
            Vec3{bounds.minimum.x, bounds.maximum.y, bounds.minimum.z}, Vec3{bounds.maximum.x, bounds.maximum.y, bounds.minimum.z},
            Vec3{bounds.minimum.x, bounds.minimum.y, bounds.maximum.z}, Vec3{bounds.maximum.x, bounds.minimum.y, bounds.maximum.z},
            Vec3{bounds.minimum.x, bounds.maximum.y, bounds.maximum.z}, Vec3{bounds.maximum.x, bounds.maximum.y, bounds.maximum.z},
        };
        Aabb result{.minimum = {std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()},
                    .maximum = {std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                                std::numeric_limits<float>::lowest()}};
        for (const Vec3 corner : corners) {
            const Result<Vec3> transformedResult = TryTransformPoint(localToWorld, corner);
            if (transformedResult.HasError())
                return Result<Aabb>::Failure(transformedResult.ErrorValue());
            const Vec3 transformed = transformedResult.Value();
            result.minimum = {std::min(result.minimum.x, transformed.x), std::min(result.minimum.y, transformed.y),
                              std::min(result.minimum.z, transformed.z)};
            result.maximum = {std::max(result.maximum.x, transformed.x), std::max(result.maximum.y, transformed.y),
                              std::max(result.maximum.z, transformed.z)};
        }
        return Result<Aabb>::Success(result);
    }

    Result<BoundingSphere> SphereFromAabb(const Aabb &bounds) noexcept {
        if (!bounds.IsValid())
            return Result<BoundingSphere>::Failure(MakeMathError(Errors::InvalidBounds, "AABB is invalid."));
        const auto midpoint = [](const float minimum, const float maximum) {
            return static_cast<float>((static_cast<double>(minimum) + static_cast<double>(maximum)) * 0.5);
        };
        const Vec3 center{midpoint(bounds.minimum.x, bounds.maximum.x), midpoint(bounds.minimum.y, bounds.maximum.y),
                          midpoint(bounds.minimum.z, bounds.maximum.z)};
        const double radius = std::hypot(static_cast<double>(bounds.maximum.x) - center.x, static_cast<double>(bounds.maximum.y) - center.y,
                                         static_cast<double>(bounds.maximum.z) - center.z);
        if (!IsFinite(center) || !std::isfinite(radius) || radius > std::numeric_limits<float>::max())
            return Result<BoundingSphere>::Failure(MakeMathError(Errors::InvalidBounds, "Bounding sphere is not representable."));
        return Result<BoundingSphere>::Success({center, static_cast<float>(radius)});
    }

    Result<std::optional<RayHit>> IntersectRayPlane(const Ray &ray, const Plane &plane) noexcept {
        if (!IsValidRay(ray))
            return Result<std::optional<RayHit>>::Failure(MakeMathError(Errors::InvalidRay, "Ray is invalid."));
        if (!IsFinite(plane.normal) || !std::isfinite(plane.distance) || !NearlyEqual(LengthSquared(plane.normal), 1.0F, 0.0001F))
            return Result<std::optional<RayHit>>::Failure(MakeMathError(Errors::InvalidPlane, "Plane is invalid."));
        const float denominator = Dot(plane.normal, ray.direction);
        if (std::fabs(denominator) <= DefaultEpsilon)
            return Result<std::optional<RayHit>>::Success(std::nullopt);
        const float distance = -(Dot(plane.normal, ray.origin) + plane.distance) / denominator;
        if (distance < ray.minimumDistance || distance > ray.maximumDistance)
            return Result<std::optional<RayHit>>::Success(std::nullopt);
        return Result<std::optional<RayHit>>::Success(RayHit{distance, ray.origin + ray.direction * distance, plane.normal});
    }

    Result<std::optional<RayHit>> IntersectRayAabb(const Ray &ray, const Aabb &bounds) noexcept {
        if (!IsValidRay(ray))
            return Result<std::optional<RayHit>>::Failure(MakeMathError(Errors::InvalidRay, "Ray is invalid."));
        if (!bounds.IsValid())
            return Result<std::optional<RayHit>>::Failure(MakeMathError(Errors::InvalidBounds, "AABB is invalid."));
        const std::array origins{ray.origin.x, ray.origin.y, ray.origin.z};
        const std::array directions{ray.direction.x, ray.direction.y, ray.direction.z};
        const std::array minimums{bounds.minimum.x, bounds.minimum.y, bounds.minimum.z};
        const std::array maximums{bounds.maximum.x, bounds.maximum.y, bounds.maximum.z};
        float nearest = ray.minimumDistance;
        float farthest = ray.maximumDistance;
        int hitAxis = -1;
        float hitSign = 0.0F;
        int exitAxis = -1;
        float exitSign = 0.0F;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            if (std::fabs(directions[axis]) <= DefaultEpsilon) {
                if (origins[axis] < minimums[axis] || origins[axis] > maximums[axis])
                    return Result<std::optional<RayHit>>::Success(std::nullopt);
                continue;
            }
            float first = (minimums[axis] - origins[axis]) / directions[axis];
            float second = (maximums[axis] - origins[axis]) / directions[axis];
            float sign = -1.0F;
            if (first > second) {
                std::swap(first, second);
                sign = 1.0F;
            }
            if (first > nearest) {
                nearest = first;
                hitAxis = static_cast<int>(axis);
                hitSign = sign;
            }
            if (second < farthest) {
                farthest = second;
                exitAxis = static_cast<int>(axis);
                exitSign = -sign;
            }
            if (farthest < nearest)
                return Result<std::optional<RayHit>>::Success(std::nullopt);
        }
        if (hitAxis < 0) {
            if (exitAxis < 0)
                return Result<std::optional<RayHit>>::Success(std::nullopt);
            nearest = farthest;
            hitAxis = exitAxis;
            hitSign = exitSign;
        }
        Vec3 normal{};
        if (hitAxis == 0)
            normal.x = hitSign;
        if (hitAxis == 1)
            normal.y = hitSign;
        if (hitAxis == 2)
            normal.z = hitSign;
        return Result<std::optional<RayHit>>::Success(RayHit{nearest, ray.origin + ray.direction * nearest, normal});
    }
}  // namespace Horo::Math
