#include "Horo/Math/SceneMath.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>

namespace {
    using namespace Horo::Math;

    bool Near(const float left, const float right, const float epsilon = 0.0001F) {
        return std::fabs(left - right) <= epsilon;
    }

    TEST_CASE("Vectors Provide Validated And Hot Path Operations", "[unit][foundation]") {
        REQUIRE((NearlyEqual(Vec2{1.0F, 2.0F} + Vec2{3.0F, 4.0F}, Vec2{4.0F, 6.0F})));
        REQUIRE((NearlyEqual(Cross({1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F}), {0.0F, 0.0F, 1.0F})));
        REQUIRE((NearlyEqual(Lerp(Vec4{}, Vec4{2.0F, 4.0F, 6.0F, 8.0F}, 0.5F), Vec4{1.0F, 2.0F, 3.0F, 4.0F})));
        REQUIRE((NearlyEqual(Normalize(Vec3{0.0F, 3.0F, 4.0F}), Vec3{0.0F, 0.6F, 0.8F})));
        REQUIRE((TryNormalize(Vec3{}).HasError()));
        REQUIRE((TryNormalize(Vec3{std::numeric_limits<float>::infinity(), 0.0F, 0.0F}).HasError()));
        const float extreme = std::numeric_limits<float>::max();
        const Horo::Result<Vec3> extremeNormalized = TryNormalize(Vec3{extreme, extreme, extreme});
        REQUIRE((extremeNormalized.HasValue()));
        REQUIRE((NearlyEqual(LengthSquared(extremeNormalized.Value()), 1.0F, 0.0001F)));
        REQUIRE((Near(DegreesToRadians(180.0F), Pi)));
        REQUIRE((Near(RadiansToDegrees(Pi * 0.5F), 90.0F)));
    }

    TEST_CASE("Quaternions Are Finite Invertible And Interpolate Shortest Path", "[unit][foundation]") {
        const Quaternion rotation = Quaternion::FromAxisAngle({0.0F, 1.0F, 0.0F}, Pi * 0.5F);
        const Vec3 rotated = rotation.Rotate({1.0F, 0.0F, 0.0F});
        REQUIRE((NearlyEqual(rotated, Vec3{0.0F, 0.0F, -1.0F}, 0.0001F)));
        REQUIRE((NearlyEqual(rotation.Inverse().Rotate(rotated), Vec3{1.0F, 0.0F, 0.0F}, 0.0001F)));

        const Quaternion authored = Quaternion::FromEulerRadians({0.2F, -0.4F, 0.6F});
        const Quaternion negated{-authored.x, -authored.y, -authored.z, -authored.w};
        REQUIRE((NearlyEqual(Nlerp(authored, negated, 0.5F).Rotate({1.0F, 0.0F, 0.0F}), authored.Rotate({1.0F, 0.0F, 0.0F}), 0.0001F)));
        REQUIRE((NearlyEqual(Slerp(Quaternion::Identity(), rotation, 0.5F).Rotate({1.0F, 0.0F, 0.0F}), Vec3{0.7071067F, 0.0F, -0.7071067F},
                             0.0002F)));
        REQUIRE((Quaternion{0.0F, 0.0F, 0.0F, 0.0F}.TryNormalized().HasError()));
        const float extreme = std::numeric_limits<float>::max();
        REQUIRE((Quaternion{extreme, extreme, extreme, extreme}.TryNormalized().HasValue()));
        REQUIRE((TrySlerp(Quaternion::Identity(), rotation, std::numeric_limits<float>::quiet_NaN()).HasError()));
    }

    TEST_CASE("Matrices Compose Invert And Decompose Affine Trs", "[unit][foundation]") {
        const Transform authored{.translation = {2.0F, -3.0F, 4.0F},
                                 .rotation = Quaternion::FromEulerRadians({0.2F, -0.4F, 0.6F}),
                                 .scale = {-2.0F, 3.0F, 0.5F}};
        const Mat4 matrix = authored.ToMatrix();
        const Horo::Result<Transform> decomposed = TryDecomposeAffineTRS(matrix);
        REQUIRE((decomposed.HasValue()));
        const Mat4 recovered = decomposed.Value().ToMatrix();
        for (std::size_t index = 0; index < matrix.values.size(); ++index)
            REQUIRE((Near(matrix.values[index], recovered.values[index], 0.001F)));

        const Horo::Result<Mat4> affineInverse = TryInverseAffine(matrix);
        const Horo::Result<Mat4> generalInverse = TryInverse(matrix);
        REQUIRE((affineInverse.HasValue() && generalInverse.HasValue()));
        constexpr Vec3 point{0.7F, -1.2F, 2.4F};
        REQUIRE((NearlyEqual(TransformAffinePoint(affineInverse.Value(), TransformAffinePoint(matrix, point)), point, 0.0002F)));
        REQUIRE((NearlyEqual(TransformPoint(generalInverse.Value(), TransformPoint(matrix, point)), point, 0.0002F)));

        Mat4 sheared = matrix;
        sheared.values[4] += 0.35F;
        REQUIRE((TryDecomposeAffineTRS(sheared).HasValue()));
        REQUIRE((TryInverseAffine(ScaleMatrix({0.0F, 1.0F, 1.0F})).HasError()));
        Mat4 nonInvertible = Mat4::Identity();
        nonInvertible.values[0] = 0.0F;
        REQUIRE((TryInverse(nonInvertible).HasError()));
        Mat4 nonFinite = Mat4::Identity();
        nonFinite.values[5] = std::numeric_limits<float>::infinity();
        REQUIRE((TryDecomposeAffineTRS(nonFinite).HasError()));
    }

    TEST_CASE("Projections Respect Depth Conventions And Round Trip", "[unit][foundation]") {
        const Mat4 view = LookAt({0.0F, 0.0F, 5.0F}, {}, {0.0F, 1.0F, 0.0F});
        for (const ClipDepthRange depth : {ClipDepthRange::NegativeOneToOne, ClipDepthRange::ZeroToOne}) {
            const Mat4 perspective = Perspective(Pi * 0.5F, 2.0F, 1.0F, 11.0F, depth);
            const Vec3 nearNdc = TransformPoint(perspective, {0.0F, 0.0F, -1.0F});
            const Vec3 farNdc = TransformPoint(perspective, {0.0F, 0.0F, -11.0F});
            REQUIRE((Near(nearNdc.z, depth == ClipDepthRange::NegativeOneToOne ? -1.0F : 0.0F)));
            REQUIRE((Near(farNdc.z, 1.0F)));

            const Mat4 orthographic = Orthographic(4.0F, 2.0F, 1.0F, 11.0F, depth);
            REQUIRE((Near(TransformPoint(orthographic, {0.0F, 0.0F, -1.0F}).z, depth == ClipDepthRange::NegativeOneToOne ? -1.0F : 0.0F)));
            REQUIRE((Near(TransformPoint(orthographic, {0.0F, 0.0F, -11.0F}).z, 1.0F)));

            const Mat4 viewProjection = Multiply(perspective, view);
            constexpr Vec3 worldPoint{0.25F, -0.5F, 0.0F};
            const Horo::Result<Vec3> projected = TryProject(viewProjection, worldPoint);
            const Horo::Result<Mat4> inverse = TryInverse(viewProjection);
            REQUIRE((projected.HasValue() && inverse.HasValue()));
            const Horo::Result<Vec3> unprojected = TryUnproject(inverse.Value(), projected.Value());
            REQUIRE((unprojected.HasValue()));
            REQUIRE((NearlyEqual(unprojected.Value(), worldPoint, 0.0002F)));
            REQUIRE((TryProject(viewProjection, {0.0F, 0.0F, 10.0F}).HasError()));
        }
        REQUIRE((TryPerspective(0.0F, 1.0F, 0.1F, 10.0F, ClipDepthRange::NegativeOneToOne).HasError()));
        REQUIRE((TryOrthographic(1.0F, -1.0F, 0.1F, 10.0F, ClipDepthRange::ZeroToOne).HasError()));
    }

    TEST_CASE("Rays Bounds And Planes Distinguish Miss From Invalid Input", "[unit][foundation]") {
        const Horo::Result<Ray> ray = TryMakeRay({0.0F, 0.0F, 5.0F}, {0.0F, 0.0F, -2.0F}, 0.0F, 20.0F);
        const Horo::Result<Plane> plane = TryMakePlane({}, {0.0F, 0.0F, 1.0F});
        REQUIRE((ray.HasValue() && plane.HasValue()));
        const auto planeHit = IntersectRayPlane(ray.Value(), plane.Value());
        REQUIRE((planeHit.HasValue() && planeHit.Value().has_value()));
        REQUIRE((Near(planeHit.Value()->distance, 5.0F)));

        constexpr Aabb box{{-1.0F, -1.0F, -1.0F}, {1.0F, 1.0F, 1.0F}};
        const auto hit = IntersectRayAabb(ray.Value(), box);
        REQUIRE((hit.HasValue() && hit.Value().has_value() && Near(hit.Value()->distance, 4.0F)));
        const auto inside = IntersectRayAabb(TryMakeRay({}, {1.0F, 0.0F, 0.0F}, 0.0F, 20.0F).Value(), box);
        REQUIRE((inside.HasValue() && inside.Value().has_value() && Near(inside.Value()->distance, 1.0F)));
        const auto miss = IntersectRayAabb(TryMakeRay({5.0F, 5.0F, 5.0F}, {1.0F, 0.0F, 0.0F}, 0.0F, 20.0F).Value(), box);
        REQUIRE((miss.HasValue() && !miss.Value().has_value()));
        REQUIRE((IntersectRayAabb(ray.Value(), Aabb{{1.0F, 0.0F, 0.0F}, {-1.0F, 0.0F, 0.0F}}).HasError()));

        const Transform transform{.translation = {2.0F, 1.0F, -3.0F},
                                  .rotation = Quaternion::FromAxisAngle({0.0F, 1.0F, 0.0F}, Pi * 0.5F),
                                  .scale = {-2.0F, 1.0F, 0.5F}};
        const Horo::Result<Aabb> transformed = TransformAabb(box, transform.ToMatrix());
        REQUIRE((transformed.HasValue()));
        REQUIRE((NearlyEqual(transformed.Value().Center(), transform.translation, 0.0001F)));
        REQUIRE((NearlyEqual(transformed.Value().Extents(), Vec3{0.5F, 1.0F, 2.0F}, 0.0001F)));

        Mat4 extremeTransform = Mat4::Identity();
        extremeTransform.values[0] = std::numeric_limits<float>::max();
        REQUIRE((TransformAabb(Aabb{{-2.0F, -1.0F, -1.0F}, {2.0F, 1.0F, 1.0F}}, extremeTransform).HasError()));
        const float extreme = std::numeric_limits<float>::max();
        REQUIRE((SphereFromAabb(Aabb{{-extreme, -extreme, -extreme}, {extreme, extreme, extreme}}).HasError()));
    }
}  // namespace
