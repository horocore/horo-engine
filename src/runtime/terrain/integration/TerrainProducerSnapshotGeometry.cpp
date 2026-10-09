#include "TerrainProducerSnapshotInternal.h"

#include <cmath>

namespace Horo::Terrain::ProducerDetail {
    namespace {
        /** @brief Copies finite canonical vertices only after all element/byte charges have been admitted. */
        Result<void> CopyVertices(const TerrainProducerSnapshotRequest &request, const TerrainSourceArtifact &artifact,
                                  TerrainProducerMesh &mesh, Budget &budget, const CancellationToken &cancellation) {
            mesh.vertices.reserve(artifact.vertices.size());
            for (const auto &vertex : artifact.vertices) {
                auto step = budget.Step(request, cancellation);
                if (!step.HasValue())
                    return step;
                if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z))
                    return Result<void>::Failure(Failure(request, TerrainProducerErrors::Invalid));
                mesh.vertices.push_back(vertex);
            }
            return Result<void>::Success();
        }

        /** @brief Preserves exact nonempty source-grid coverage; no hole coverage is reconstructed. */
        bool ValidCoverage(const TerrainSourceTriangle &triangle, const TerrainProducerMesh &mesh) noexcept {
            return triangle.beginX < triangle.endX && triangle.beginZ < triangle.endZ && triangle.endX < mesh.sourceWidth &&
                   triangle.endZ < mesh.sourceHeight;
        }

        /** @brief Validates indices before access, then requires finite nondegenerate upward winding. */
        bool ValidTriangle(const TerrainSourceTriangle &triangle, const TerrainProducerMesh &mesh) noexcept {
            const auto &indices = triangle.indices;
            if (!std::ranges::all_of(indices,
                                     [&](const auto index) {
                return index < mesh.vertices.size();
            }) ||
                !ValidCoverage(triangle, mesh))
                return false;
            const auto &a = mesh.vertices[indices[0]];
            const auto &b = mesh.vertices[indices[1]];
            const auto &c = mesh.vertices[indices[2]];
            const double area = (b.z - a.z) * (c.x - a.x) - (b.x - a.x) * (c.z - a.z);
            return std::isfinite(area) && area > 0;
        }

        /** @brief Copies only validated triangles; every failure unwinds the unpublished mesh. */
        Result<void> CopyTriangles(const TerrainProducerSnapshotRequest &request, const TerrainSourceArtifact &artifact,
                                   TerrainProducerMesh &mesh, Budget &budget, const CancellationToken &cancellation) {
            mesh.triangles.reserve(artifact.triangles.size());
            for (const auto &triangle : artifact.triangles) {
                auto step = budget.Step(request, cancellation);
                if (!step.HasValue())
                    return step;
                if (!ValidTriangle(triangle, mesh))
                    return Result<void>::Failure(Failure(request, TerrainProducerErrors::Invalid));
                mesh.triangles.push_back(triangle);
            }
            return Result<void>::Success();
        }

        /** @brief Builds one detached source/provenance projection; admission and hash verification precede copied geometry. */
        Result<TerrainProducerMesh> CopyMesh(const TerrainProducerSnapshotRequest &request, const TerrainSourceArtifact &artifact,
                                             Budget &budget, const CancellationToken &cancellation) {
            if (!Charge(budget.vertices, artifact.vertices.size(), budget.limits.maximumVertices) ||
                !Charge(budget.triangles, artifact.triangles.size(), budget.limits.maximumTriangles) ||
                !budget.Bytes(artifact.vertices.size() * sizeof(TerrainSourceVertex)) ||
                !budget.Bytes(artifact.triangles.size() * sizeof(TerrainSourceTriangle)))
                return Result<TerrainProducerMesh>::Failure(Failure(request, TerrainProducerErrors::Limit));
            auto verified = VerifyPayload(request, artifact.payload, artifact.digest, budget, cancellation);
            if (!verified.HasValue())
                return Result<TerrainProducerMesh>::Failure(verified.ErrorValue());
            const auto &source = request.terrain->Tiles();
            TerrainProducerMesh mesh;
            mesh.tile = artifact.tile;
            mesh.sourceAsset = source.sourceAsset;
            mesh.sourceRevision = source.sourceRevision;
            mesh.sourceDigest = source.sourceDigest;
            mesh.cookFingerprint = request.terrain->Fingerprint();
            mesh.artifactDigest = artifact.digest;
            mesh.seams = artifact.seams;
            mesh.sourceWidth = source.sourceWidth;
            mesh.sourceHeight = source.sourceHeight;
            mesh.maximumGeometricError = artifact.maximumGeometricError;
            mesh.requiresSameLodNeighbors = artifact.requiresSameLodNeighbors;
            auto copied = CopyVertices(request, artifact, mesh, budget, cancellation);
            if (!copied.HasValue())
                return Result<TerrainProducerMesh>::Failure(copied.ErrorValue());
            copied = CopyTriangles(request, artifact, mesh, budget, cancellation);
            if (!copied.HasValue())
                return Result<TerrainProducerMesh>::Failure(copied.ErrorValue());
            return Result<TerrainProducerMesh>::Success(std::move(mesh));
        }
    }  // namespace

    /** @copydoc CopyMeshes */
    Result<void> CopyMeshes(const TerrainProducerSnapshotRequest &request, const std::span<const TerrainSourceArtifact *const> meshes,
                            std::vector<TerrainProducerMesh> &output, Budget &budget, const CancellationToken &cancellation) {
        for (const auto *artifact : meshes) {
            auto mesh = CopyMesh(request, *artifact, budget, cancellation);
            if (!mesh.HasValue())
                return Result<void>::Failure(mesh.ErrorValue());
            output.push_back(std::move(mesh).Value());
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Terrain::ProducerDetail
