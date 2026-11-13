#include "ClusteredLightGridInternal.h"

#include <algorithm>
#include <cmath>

namespace Horo::Render {
    namespace {
        double ViewDepth(const ClusteredLightGridDescriptor &descriptor, float deviceDepth) noexcept {
            const double nearDistance = descriptor.camera.projection.nearPlane;
            const double farDistance = descriptor.camera.projection.farPlane;
            if (deviceDepth == 0)
                return nearDistance;
            if (deviceDepth == 1)
                return farDistance;
            if (descriptor.camera.projection.kind == RenderProjectionKind::Orthographic)
                return nearDistance + deviceDepth * (farDistance - nearDistance);
            return nearDistance * farDistance / (farDistance - deviceDepth * (farDistance - nearDistance));
        }

        std::uint32_t DepthIndex(const ClusteredLightGridDescriptor &descriptor, double depth) noexcept {
            const double nearDistance = descriptor.camera.projection.nearPlane;
            const double farDistance = descriptor.camera.projection.farPlane;
            const double unit = descriptor.depthPartition == LightClusterDepthPartition::Linear
                                    ? (depth - nearDistance) / (farDistance - nearDistance)
                                    : std::log(depth / nearDistance) / std::log(farDistance / nearDistance);
            const double slice = std::clamp(unit * descriptor.z, 0.0, static_cast<double>(descriptor.z - 1U));
            return static_cast<std::uint32_t>(slice);
        }

        bool FiniteCoordinates(float x, float y, float depth) noexcept {
            return std::isfinite(x) && std::isfinite(y) && std::isfinite(depth);
        }

        bool OutsidePixel(const ClusteredLightGridDescriptor &descriptor, float x, float y) noexcept {
            return x < 0 || y < 0 || x >= descriptor.extent.width || y >= descriptor.extent.height;
        }

        std::uint32_t Index(const ClusteredLightGridDescriptor &descriptor, float pixelX, float pixelY, double viewDepth) noexcept {
            const auto x = std::min(static_cast<std::uint32_t>(static_cast<double>(pixelX) * descriptor.x / descriptor.extent.width),
                                    descriptor.x - 1U);
            const auto y = std::min(static_cast<std::uint32_t>(static_cast<double>(pixelY) * descriptor.y / descriptor.extent.height),
                                    descriptor.y - 1U);
            const auto z = DepthIndex(descriptor, viewDepth);
            return x + descriptor.x * (y + descriptor.y * z);
        }
    }  // namespace

    Result<std::optional<std::uint32_t>> MapClusteredLightPixel(const ClusteredLightGrid &grid, float pixelX, float pixelY,
                                                                float deviceDepth) {
        using Outcome = Result<std::optional<std::uint32_t>>;
        if (!grid.IsValid() || !FiniteCoordinates(pixelX, pixelY, deviceDepth))
            return Outcome::Failure(MakeError(LightCullingErrors::InvalidInput));
        const auto &descriptor = grid.descriptor;
        if (OutsidePixel(descriptor, pixelX, pixelY) || deviceDepth < 0 || deviceDepth > 1)
            return Outcome::Success(std::nullopt);
        const double depth = ViewDepth(descriptor, deviceDepth);
        if (!std::isfinite(depth) || depth <= 0)
            return Outcome::Failure(MakeError(LightCullingErrors::InvalidInput));
        return Outcome::Success(Index(descriptor, pixelX, pixelY, depth));
    }

    Result<std::optional<std::uint32_t>> MapClusteredLightViewDepth(const ClusteredLightGrid &grid, float pixelX, float pixelY,
                                                                    float viewDepth) {
        using Outcome = Result<std::optional<std::uint32_t>>;
        if (!grid.IsValid() || !FiniteCoordinates(pixelX, pixelY, viewDepth) || viewDepth <= 0)
            return Outcome::Failure(MakeError(LightCullingErrors::InvalidInput));
        const auto &descriptor = grid.descriptor;
        if (OutsidePixel(descriptor, pixelX, pixelY) || viewDepth < descriptor.camera.projection.nearPlane ||
            viewDepth > descriptor.camera.projection.farPlane)
            return Outcome::Success(std::nullopt);
        return Outcome::Success(Index(descriptor, pixelX, pixelY, viewDepth));
    }
}  // namespace Horo::Render
