#include "DirectionalShadowInternal.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Render::DirectionalShadowDetail {
    namespace {
        using Corners = std::array<Math::Vec3, 8>;

        struct Slice final {
            Corners corners;
            Math::Vec3 center;
            float radius{};
        };

        float Component(Math::Vec3 value, unsigned axis) noexcept {
            if (axis == 0)
                return value.x;
            return axis == 1 ? value.y : value.z;
        }

        bool LocalBounds(const Math::Aabb &bounds) noexcept {
            const float limit = 10000000.0F;
            for (unsigned axis = 0; axis < 3; ++axis)
                if (std::abs(Component(bounds.minimum, axis)) > limit || std::abs(Component(bounds.maximum, axis)) > limit)
                    return false;
            return bounds.IsValid();
        }

        Corners BoundsCorners(const Math::Aabb &bounds) noexcept {
            Corners result;
            for (unsigned index = 0; index < result.size(); ++index)
                result[index] = {(index & 1U) ? bounds.maximum.x : bounds.minimum.x, (index & 2U) ? bounds.maximum.y : bounds.minimum.y,
                                 (index & 4U) ? bounds.maximum.z : bounds.minimum.z};
            return result;
        }

        void Accumulate(DepthBounds &bounds, const Corners &corners, Math::Vec3 direction) noexcept {
            for (const auto point : corners) {
                const auto distance = Math::Dot(point, direction);
                bounds.minimum = std::min(bounds.minimum, distance);
                bounds.maximum = std::max(bounds.maximum, distance);
            }
        }

        Slice ReceiverSlice(const RenderProjectionDescriptor &projection, const Basis &camera, float aspect, float nearDistance,
                            float farDistance) noexcept {
            Slice slice;
            slice.center = camera.forward * ((nearDistance + farDistance) * 0.5F);
            const auto halfHeight = [&](float distance) {
                return projection.kind == RenderProjectionKind::Perspective ? std::tan(projection.verticalFovRadians * 0.5F) * distance
                                                                            : projection.orthographicHeight * 0.5F;
            };
            for (unsigned index = 0; index < slice.corners.size(); ++index) {
                const auto distance = (index & 4U) ? farDistance : nearDistance;
                const auto height = halfHeight(distance);
                slice.corners[index] = camera.forward * distance + camera.right * (((index & 1U) ? 1.0F : -1.0F) * height * aspect) +
                                       camera.up * (((index & 2U) ? 1.0F : -1.0F) * height);
                slice.radius = std::max(slice.radius, Math::Length(slice.corners[index] - slice.center));
            }
            // Rotation-invariant receiver sphere with a small outward quantization; no scene-bounds scale authority.
            slice.radius = std::ceil(std::max(slice.radius, 0.01F) * 16.0F) / 16.0F;
            return slice;
        }

        void Snap(Math::Vec3 &center, Math::Vec3 origin, Math::Vec3 axis, float texel) noexcept {
            const double world = (static_cast<double>(origin.x) + center.x) * axis.x + (static_cast<double>(origin.y) + center.y) * axis.y +
                                 (static_cast<double>(origin.z) + center.z) * axis.z;
            const auto adjustment = static_cast<float>(std::round(world / texel) * texel - world);
            center += axis * adjustment;
        }

        Result<Math::Mat4> LightMatrix(Slice slice, Math::Vec3 direction, DepthBounds depth, Math::Vec3 origin, float texel,
                                       float normalBias, bool stabilize, Math::ClipDepthRange depthRange) {
            const Math::Vec3 hint = std::abs(direction.y) > 0.95F ? Math::Vec3{1, 0, 0} : Math::Vec3{0, 1, 0};
            const auto side = Math::TryNormalize(Math::Cross(direction, hint));
            if (side.HasError())
                return Result<Math::Mat4>::Failure(side.ErrorValue());
            const auto up = Math::Cross(side.Value(), direction);
            if (stabilize) {
                Snap(slice.center, origin, side.Value(), texel);
                Snap(slice.center, origin, up, texel);
            }
            Accumulate(depth, slice.corners, direction);
            const float margin = std::max(slice.radius * 0.05F, 1.0F) + normalBias;
            const float centerDepth = Math::Dot(slice.center, direction);
            const auto eye = slice.center + direction * (depth.minimum - centerDepth - margin);
            const auto view = Math::TryLookAt(eye, eye + direction * std::max(slice.radius, 1.0F), up);
            if (view.HasError())
                return Result<Math::Mat4>::Failure(view.ErrorValue());
            const auto projection = Math::TryOrthographic(slice.radius * 2.0F + texel * 2.0F, 1.0F, 0.1F,
                                                          depth.maximum - depth.minimum + margin * 2.0F, depthRange);
            if (projection.HasError())
                return Result<Math::Mat4>::Failure(projection.ErrorValue());
            return Result<Math::Mat4>::Success(Math::Multiply(projection.Value(), view.Value()));
        }

        void Atlas(DirectionalShadowCascade &cascade, std::uint32_t index, const DirectionalShadowProfile &profile,
                   DirectionalShadowAtlasExtent atlas) noexcept {
            const auto inner = profile.tileResolution - DirectionalShadowGuardPixels * 2U;
            cascade.viewport = {(index % 2U) * profile.tileResolution + DirectionalShadowGuardPixels,
                                (index / 2U) * profile.tileResolution + DirectionalShadowGuardPixels, inner, inner};
            cascade.atlasScaleBias = {static_cast<float>(inner) / atlas.width, static_cast<float>(inner) / atlas.height,
                                      static_cast<float>(cascade.viewport.x) / atlas.width,
                                      static_cast<float>(cascade.viewport.y) / atlas.height};
        }
    }  // namespace

    Result<Basis> CameraBasis(const RenderCameraView &camera) {
        const auto forward = Math::TryNormalize(camera.target - camera.position);
        if (forward.HasError())
            return Result<Basis>::Failure(forward.ErrorValue());
        const auto right = Math::TryNormalize(Math::Cross(forward.Value(), camera.up));
        if (right.HasError())
            return Result<Basis>::Failure(right.ErrorValue());
        return Result<Basis>::Success({forward.Value(), right.Value(), Math::Cross(right.Value(), forward.Value())});
    }

    Result<DepthBounds> CasterDepth(const RenderSceneView &scene, Math::Vec3 direction) {
        DepthBounds depth{std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest()};
        for (const auto &instance : scene.instances) {
            auto localized = instance.localToWorld;
            localized.values[12] = static_cast<float>(static_cast<double>(localized.values[12]) - scene.camera.position.x);
            localized.values[13] = static_cast<float>(static_cast<double>(localized.values[13]) - scene.camera.position.y);
            localized.values[14] = static_cast<float>(static_cast<double>(localized.values[14]) - scene.camera.position.z);
            const auto bounds = Math::TransformAabb(instance.localBounds, localized);
            if (bounds.HasError())
                return Result<DepthBounds>::Failure(bounds.ErrorValue());
            if (!LocalBounds(bounds.Value()))
                return Result<DepthBounds>::Failure(MakeError(DirectionalShadowErrors::InvalidCoordinates));
            Accumulate(depth, BoundsCorners(bounds.Value()), direction);
        }
        return Result<DepthBounds>::Success(depth);
    }

    float SplitDistance(const RenderProjectionDescriptor &projection, const DirectionalShadowProfile &profile,
                        std::uint32_t split) noexcept {
        const float farDistance = std::min(projection.farPlane, profile.maximumDistance);
        const float ratio = static_cast<float>(split) / profile.cascadeCount;
        const float linear = projection.nearPlane + (farDistance - projection.nearPlane) * ratio;
        const float logarithmic = projection.nearPlane * std::pow(farDistance / projection.nearPlane, ratio);
        const float lambda = profile.splitMode == DirectionalShadowSplitMode::Logarithmic ? 1.0F : profile.splitLambda;
        return linear * (1.0F - lambda) + logarithmic * lambda;
    }

    Result<DirectionalShadowCascade> FitCascade(const RenderSceneView &scene, const Basis &camera, Math::Vec3 direction,
                                                DepthBounds casterDepth, float aspect, Math::ClipDepthRange depthRange,
                                                const DirectionalShadowProfile &profile, std::uint32_t index,
                                                DirectionalShadowAtlasExtent atlas) {
        using Outcome = Result<DirectionalShadowCascade>;
        DirectionalShadowCascade cascade;
        cascade.splitNear = SplitDistance(scene.camera.projection, profile, index);
        cascade.splitFar = SplitDistance(scene.camera.projection, profile, index + 1U);
        cascade.blendStart = cascade.splitFar - (cascade.splitFar - cascade.splitNear) * profile.blendFraction;
        const float previousNear = SplitDistance(scene.camera.projection, profile, index == 0 ? 0 : index - 1U);
        const float fitNear = cascade.splitNear - (cascade.splitNear - previousNear) * profile.blendFraction;
        auto slice = ReceiverSlice(scene.camera.projection, camera, aspect, fitNear, cascade.splitFar);
        // Receiver normals can move a sample by this many world units in any direction.
        slice.radius += profile.normalBias;
        if (!std::isfinite(slice.radius) || slice.radius > 1000000.0F)
            return Outcome::Failure(MakeError(DirectionalShadowErrors::InvalidCoordinates));
        cascade.worldUnitsPerTexel = slice.radius * 2.0F / (profile.tileResolution - DirectionalShadowGuardPixels * 2U);
        auto matrix = LightMatrix(slice, direction, casterDepth, scene.camera.position, cascade.worldUnitsPerTexel, profile.normalBias,
                                  profile.stabilize, depthRange);
        if (matrix.HasError())
            return Outcome::Failure(matrix.ErrorValue());
        cascade.cameraRelativeViewProjection = std::move(matrix).Value();
        cascade.viewProjection = Math::Multiply(cascade.cameraRelativeViewProjection, Math::TranslationMatrix(-scene.camera.position));
        if (!Math::IsFinite(cascade.viewProjection))
            return Outcome::Failure(MakeError(DirectionalShadowErrors::InvalidCoordinates));
        Atlas(cascade, index, profile, atlas);
        return Outcome::Success(cascade);
    }
}  // namespace Horo::Render::DirectionalShadowDetail
