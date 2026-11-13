#include "ClusteredLightGridInternal.h"

#include <cmath>
#include <limits>

namespace Horo::Render::ClusteredLightDetail {
    namespace {
        struct Slice final {
            float left;
            float right;
            float bottom;
            float top;
            float nearDistance;
            float farDistance;
        };

        Slice Bounds(const ClusteredLightGridDescriptor &d, std::uint32_t index) noexcept {
            const auto x = index % d.x;
            const auto y = (index / d.x) % d.y;
            const auto z = index / (d.x * d.y);
            return {-1.0F + 2.0F * x / d.x, -1.0F + 2.0F * (x + 1U) / d.x, 1.0F - 2.0F * (y + 1U) / d.y,
                    1.0F - 2.0F * y / d.y,  DepthBoundary(d, z),           DepthBoundary(d, z + 1U)};
        }

        Result<LightClusterPlane> Plane(Math::Vec3 inward, Math::Vec3 origin, float localOffset) {
            const auto normal = Math::TryNormalize(inward);
            if (normal.HasError())
                return Result<LightClusterPlane>::Failure(normal.ErrorValue());
            const auto n = normal.Value();
            const double dot =
                static_cast<double>(n.x) * origin.x + static_cast<double>(n.y) * origin.y + static_cast<double>(n.z) * origin.z;
            // Outward rounding preserves conservative membership after world-plane cancellation at rebased float coordinates.
            const double scale = std::abs(static_cast<double>(n.x) * origin.x) + std::abs(static_cast<double>(n.y) * origin.y) +
                                 std::abs(static_cast<double>(n.z) * origin.z);
            const auto offset =
                static_cast<float>(static_cast<double>(localOffset) - dot + scale * std::numeric_limits<float>::epsilon() * 4.0);
            if (!std::isfinite(offset))
                return Result<LightClusterPlane>::Failure(MakeError(LightCullingErrors::InvalidInput));
            const auto rounded = std::nextafter(offset, std::numeric_limits<float>::infinity());
            if (!std::isfinite(rounded))
                return Result<LightClusterPlane>::Failure(MakeError(LightCullingErrors::InvalidInput));
            return Result<LightClusterPlane>::Success({n, rounded});
        }

        Result<void> SidePlanes(LightCluster &cluster, const ClusteredLightGridDescriptor &d, const Basis &basis, const Slice &slice) {
            const float height = d.camera.projection.kind == RenderProjectionKind::Perspective
                                     ? std::tan(d.camera.projection.verticalFovRadians * 0.5F)
                                     : d.camera.projection.orthographicHeight * 0.5F;
            const float width = height * static_cast<float>(d.extent.width) / d.extent.height;
            const std::array axes{basis.right, -basis.right, basis.up, -basis.up};
            const std::array edges{slice.left * width, -slice.right * width, slice.bottom * height, -slice.top * height};
            for (unsigned index = 0; index < 4; ++index) {
                const bool perspective = d.camera.projection.kind == RenderProjectionKind::Perspective;
                const auto inward = perspective ? axes[index] - basis.forward * edges[index] : axes[index];
                const auto plane = Plane(inward, d.camera.position, perspective ? 0.0F : -edges[index]);
                if (plane.HasError())
                    return Result<void>::Failure(plane.ErrorValue());
                cluster.planes[index] = plane.Value();
            }
            return Result<void>::Success();
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

    float DepthBoundary(const ClusteredLightGridDescriptor &d, std::uint32_t slice) noexcept {
        const double nearDistance = d.camera.projection.nearPlane;
        const double farDistance = d.camera.projection.farPlane;
        const double fraction = static_cast<double>(slice) / d.z;
        return static_cast<float>(d.depthPartition == LightClusterDepthPartition::Linear
                                      ? nearDistance + (farDistance - nearDistance) * fraction
                                      : nearDistance * std::pow(farDistance / nearDistance, fraction));
    }

    Result<LightCluster> Cell(const ClusteredLightGridDescriptor &descriptor, const Basis &basis, std::uint32_t index) {
        const auto slice = Bounds(descriptor, index);
        LightCluster cluster;
        if (const auto sides = SidePlanes(cluster, descriptor, basis, slice); sides.HasError())
            return Result<LightCluster>::Failure(sides.ErrorValue());
        const auto nearPlane = Plane(basis.forward, descriptor.camera.position, -slice.nearDistance);
        if (nearPlane.HasError())
            return Result<LightCluster>::Failure(nearPlane.ErrorValue());
        const auto farPlane = Plane(-basis.forward, descriptor.camera.position, slice.farDistance);
        if (farPlane.HasError())
            return Result<LightCluster>::Failure(farPlane.ErrorValue());
        cluster.planes[4] = nearPlane.Value();
        cluster.planes[5] = farPlane.Value();
        return Result<LightCluster>::Success(cluster);
    }
}  // namespace Horo::Render::ClusteredLightDetail
