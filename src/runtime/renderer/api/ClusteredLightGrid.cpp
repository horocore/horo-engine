#include "ClusteredLightGridInternal.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace Horo::Render {
    namespace {
        bool Dimensions(const ClusteredLightGridDescriptor &descriptor) noexcept {
            const auto limit = LightCullingBudget::HardMaximumClusters;
            return descriptor.x > 0 && descriptor.y > 0 && descriptor.z > 0 && descriptor.x <= limit && descriptor.y <= limit &&
                   descriptor.z <= limit && std::uint64_t{descriptor.x} * descriptor.y * descriptor.z <= limit;
        }

        bool KnownDepth(const ClusteredLightGridDescriptor &descriptor) noexcept {
            return (descriptor.depthPartition == LightClusterDepthPartition::Linear ||
                    descriptor.depthPartition == LightClusterDepthPartition::Logarithmic) &&
                   (descriptor.depthRange == Math::ClipDepthRange::ZeroToOne ||
                    descriptor.depthRange == Math::ClipDepthRange::NegativeOneToOne);
        }

        Result<Math::Mat4> ViewProjection(const ClusteredLightGridDescriptor &descriptor) {
            const auto &camera = descriptor.camera;
            const auto view = Math::TryLookAt(camera.position, camera.target, camera.up);
            if (view.HasError())
                return Result<Math::Mat4>::Failure(view.ErrorValue());
            const float aspect = static_cast<float>(descriptor.extent.width) / descriptor.extent.height;
            const auto &p = camera.projection;
            const auto projection =
                p.kind == RenderProjectionKind::Perspective
                    ? Math::TryPerspective(p.verticalFovRadians, aspect, p.nearPlane, p.farPlane, descriptor.depthRange)
                    : Math::TryOrthographic(p.orthographicHeight, aspect, p.nearPlane, p.farPlane, descriptor.depthRange);
            if (projection.HasError())
                return Result<Math::Mat4>::Failure(projection.ErrorValue());
            const auto matrix = Math::Multiply(projection.Value(), view.Value());
            if (!Math::IsFinite(matrix))
                return Result<Math::Mat4>::Failure(MakeError(LightCullingErrors::InvalidInput));
            return Result<Math::Mat4>::Success(matrix);
        }

        Result<void> FillClusters(const ClusteredLightGridDescriptor &descriptor, const ClusteredLightDetail::Basis &basis,
                                  std::span<LightCluster> clusters, const CancellationToken &cancellation) {
            for (std::uint32_t index = 0; index < clusters.size(); ++index) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(LightCullingErrors::Cancelled));
                const auto cell = ClusteredLightDetail::Cell(descriptor, basis, index);
                if (cell.HasError())
                    return Result<void>::Failure(cell.ErrorValue());
                clusters[index] = cell.Value();
            }
            return Result<void>::Success();
        }
    }  // namespace

    bool ClusteredLightIdentity::IsValid() const noexcept {
        return view.IsValid() && sceneDomain != 0 && lightTableGeneration != 0 && projectionGeneration != 0 && gridGeneration != 0;
    }

    bool ClusteredLightGridDescriptor::IsValid() const noexcept {
        return identity.IsValid() && camera.IsValid() && extent.width > 0 && extent.height > 0 && Dimensions(*this) && KnownDepth(*this);
    }

    bool ClusteredLightGrid::IsValid() const noexcept {
        return descriptor.IsValid() && clusterCount == std::uint64_t{descriptor.x} * descriptor.y * descriptor.z && clusterSignature != 0 &&
               Math::IsFinite(viewProjection);
    }

    Result<ClusteredLightGrid> BuildClusteredLightGrid(const ClusteredLightGridDescriptor &descriptor, const LightCullingBudget &budget,
                                                       std::span<LightCluster> clusters, const CancellationToken &cancellation) {
        using Outcome = Result<ClusteredLightGrid>;
        if (!descriptor.IsValid() || !budget.IsValid())
            return Outcome::Failure(MakeError(LightCullingErrors::InvalidInput));
        const auto count = descriptor.x * descriptor.y * descriptor.z;
        if (count > budget.maximumClusters || clusters.size() < count)
            return Outcome::Failure(MakeError(LightCullingErrors::Capacity));
        const auto basis = ClusteredLightDetail::CameraBasis(descriptor.camera);
        if (basis.HasError())
            return Outcome::Failure(basis.ErrorValue());
        const auto matrix = ViewProjection(descriptor);
        if (matrix.HasError())
            return Outcome::Failure(matrix.ErrorValue());
        const auto cells = clusters.first(count);
        if (const auto filled = FillClusters(descriptor, basis.Value(), cells, cancellation); filled.HasError())
            return Outcome::Failure(filled.ErrorValue());
        const auto signature = ClusteredLightDetail::Signature(cells);
        if (cancellation.IsCancellationRequested())
            return Outcome::Failure(MakeError(LightCullingErrors::Cancelled));
        return Outcome::Success({descriptor, matrix.Value(), count, signature});
    }
}  // namespace Horo::Render

namespace Horo::Render::ClusteredLightDetail {
    std::uint64_t Signature(std::span<const LightCluster> clusters) noexcept {
        std::uint64_t signature = 14695981039346656037ULL;
        const auto add = [&signature](float value) {
            const auto bits = std::bit_cast<std::uint32_t>(value == 0.0F ? 0.0F : value);
            signature ^= bits;
            signature *= 1099511628211ULL;
        };
        for (const auto &cluster : clusters)
            for (const auto &plane : cluster.planes) {
                add(plane.normal.x);
                add(plane.normal.y);
                add(plane.normal.z);
                add(plane.offset);
            }
        return signature == 0 ? 1 : signature;
    }
}  // namespace Horo::Render::ClusteredLightDetail
