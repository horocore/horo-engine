#pragma once

#include "Horo/Destruction/ChunkMeshCook.h"
#include "OfflineVoronoiInternal.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <ranges>
#include <string_view>

namespace Horo::Destruction::ChunkMeshDetail {
    using VoronoiDetail::Cross;
    using VoronoiDetail::Dot;
    using VoronoiDetail::Finite;
    using VoronoiDetail::Point;
    using VoronoiDetail::Position;
    using VoronoiDetail::Sub;

    [[nodiscard]] inline bool Nonzero(const Sha256Digest &digest) {
        return std::ranges::any_of(digest.bytes, [](std::uint8_t byte) {
            return byte != 0;
        });
    }

    inline void HashByte(Sha256Builder &hash, std::uint8_t value) {
        const auto byte = static_cast<std::byte>(value);
        (void)hash.Update(std::span{&byte, 1});
    }

    inline void HashU64(Sha256Builder &hash, std::uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8)
            HashByte(hash, static_cast<std::uint8_t>(value >> shift));
    }

    inline void HashDigest(Sha256Builder &hash, const Sha256Digest &digest) {
        for (const auto byte : digest.bytes)
            HashByte(hash, byte);
    }

    inline void HashName(Sha256Builder &hash, std::string_view name) {
        HashU64(hash, name.size());
        for (const char value : name)
            HashByte(hash, static_cast<std::uint8_t>(value));
    }

    [[nodiscard]] inline Sha256Digest MaterialDigest(std::span<const ChunkMaterialBinding> materials) {
        Sha256Builder hash;
        HashU64(hash, materials.size());
        for (const auto &material : materials) {
            HashU64(hash, material.slot);
            for (const auto byte : material.asset.Bytes())
                HashByte(hash, byte);
            HashDigest(hash, material.revisionDigest);
        }
        return hash.Finalize();
    }

    inline void HashFloat(Sha256Builder &hash, float value) {
        HashU64(hash, std::bit_cast<std::uint32_t>(value));
    }

    inline void HashDouble(Sha256Builder &hash, double value) {
        HashU64(hash, std::bit_cast<std::uint64_t>(value));
    }

    inline void HashArtifactMetadata(Sha256Builder &hash, const ChunkMeshArtifact &artifact) {
        HashU64(hash, artifact.schemaVersion);
        HashU64(hash, static_cast<std::uint8_t>(artifact.tier));
        HashU64(hash, artifact.producedFeatures.bits);
        HashDigest(hash, artifact.content.SemanticDigest());
        for (const auto byte : artifact.sourceAsset.Bytes())
            HashByte(hash, byte);
        HashU64(hash, artifact.sourceRevision);
        HashDigest(hash, artifact.sourceDigest);
        HashU64(hash, artifact.recipeId);
        HashU64(hash, artifact.recipeRevision);
        HashDigest(hash, artifact.inputFingerprint);
        HashDigest(hash, artifact.materialFingerprint);
        HashU64(hash, artifact.materials.size());
        for (const auto &material : artifact.materials) {
            HashU64(hash, material.slot);
            for (const auto byte : material.asset.Bytes())
                HashByte(hash, byte);
            HashDigest(hash, material.revisionDigest);
        }
    }

    /** @brief Hashes canonical geometric membership, independent of source vertex/triangle order. */
    [[nodiscard]] inline CollisionPieceId PieceIdentity(const ChunkCollisionPiece &piece) {
        auto positions = piece.positions;
        std::ranges::sort(positions);
        positions.erase(std::ranges::unique(positions).begin(), positions.end());
        Sha256Builder hash;
        HashU64(hash, ChunkMeshCookSchemaVersion);
        HashU64(hash, positions.size());
        for (const auto &position : positions)
            for (const float value : position)
                HashFloat(hash, value == 0.0F ? 0.0F : value);
        const auto digest = hash.Finalize();
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < sizeof(value); ++i)
            value = (value << 8U) | digest.bytes[i];
        // Zero is reserved; duplicate identities still fail before publication.
        return CollisionPieceId::Create(value == 0 ? 1 : value).Value();
    }

    /** @brief Hashes the complete neutral region table without changing canonical field order. */
    inline void HashCollisionPieces(Sha256Builder &hash, std::span<const ChunkCollisionPiece> pieces) {
        HashU64(hash, pieces.size());
        for (const auto &piece : pieces) {
            HashU64(hash, piece.id.Value());
            HashU64(hash, piece.positions.size());
            for (const auto &position : piece.positions)
                for (const float value : position)
                    HashFloat(hash, value);
            HashU64(hash, piece.triangles.size());
            for (const auto &triangle : piece.triangles)
                for (const auto index : triangle)
                    HashU64(hash, index);
            HashDouble(hash, piece.volume);
        }
    }

    inline void HashChunkMesh(Sha256Builder &hash, const ChunkMesh &chunk) {
        HashU64(hash, chunk.id.Value());
        HashCollisionPieces(hash, chunk.collisionPieces);
        HashU64(hash, chunk.vertices.size());
        for (const auto &vertex : chunk.vertices) {
            for (const float value : vertex.position)
                HashFloat(hash, value);
            for (const float value : vertex.normal)
                HashFloat(hash, value);
            for (const float value : vertex.tangent)
                HashFloat(hash, value);
            for (const float value : vertex.uv)
                HashFloat(hash, value);
        }
        HashU64(hash, chunk.faces.size());
        for (const auto &face : chunk.faces) {
            for (const auto index : face.indices)
                HashU64(hash, index);
            HashU64(hash, face.materialSlot);
            HashByte(hash, face.interior ? 1 : 0);
        }
        for (float value : chunk.mass.minimum)
            HashFloat(hash, value);
        for (float value : chunk.mass.maximum)
            HashFloat(hash, value);
        HashDouble(hash, chunk.mass.volume);
        for (double value : chunk.mass.firstMoment)
            HashDouble(hash, value);
    }

    [[nodiscard]] inline Sha256Digest ArtifactDigest(const ChunkMeshArtifact &artifact) {
        Sha256Builder hash;
        HashArtifactMetadata(hash, artifact);
        HashU64(hash, artifact.chunks.size());
        for (const auto &chunk : artifact.chunks)
            HashChunkMesh(hash, chunk);
        HashU64(hash, artifact.estimatedBytes);
        HashU64(hash, artifact.workItems);
        return hash.Finalize();
    }

    [[nodiscard]] inline bool ValidLimits(const DestructionLimits &limits, DestructionFeatureTier tier) {
        const auto profile = GetDestructionTierProfile(tier);
        if (profile.HasError() || !profile.Value().supportedFeatures.Contains(DestructionFeature::PreCookedFracture))
            return false;
        return limits.maximumChunksPerDestructible != 0 &&
               limits.maximumChunksPerDestructible <= DestructionHardLimits::ChunksPerDestructible &&
               limits.maximumChunksPerDestructible <= profile.Value().limits.maximumChunksPerDestructible &&
               limits.maximumArtifactBytes != 0 && limits.maximumArtifactBytes <= DestructionHardLimits::ArtifactBytes &&
               limits.maximumArtifactBytes <= profile.Value().limits.maximumArtifactBytes &&
               limits.maximumTransitionBytes >= limits.maximumArtifactBytes &&
               limits.maximumTransitionBytes <= DestructionHardLimits::TransitionBytes &&
               limits.maximumTransitionBytes <= profile.Value().limits.maximumTransitionBytes &&
               limits.maximumResidentBytes >= limits.maximumTransitionBytes &&
               limits.maximumResidentBytes <= DestructionHardLimits::ResidentBytes &&
               limits.maximumResidentBytes <= profile.Value().limits.maximumResidentBytes && limits.maximumWorkItemsPerTransition != 0 &&
               limits.maximumWorkItemsPerTransition <= DestructionHardLimits::WorkItemsPerTransition &&
               limits.maximumWorkItemsPerTransition <= profile.Value().limits.maximumWorkItemsPerTransition;
    }

    struct Budget final {
        std::uint64_t bytes{};
        std::uint64_t work{};
        const DestructionLimits &limits;

        [[nodiscard]] inline bool Charge(std::uint64_t addedBytes, std::uint64_t addedWork) {
            if (bytes > limits.maximumArtifactBytes || addedBytes > limits.maximumArtifactBytes - bytes ||
                work > limits.maximumWorkItemsPerTransition || addedWork > limits.maximumWorkItemsPerTransition - work)
                return false;
            bytes += addedBytes;
            work += addedWork;
            return true;
        }
    };

    /** @brief Copies sealed neutral regions with budget admission before geometry allocation. */
    [[nodiscard]] inline Result<void> AppendCollisionPieces(std::span<const OfflineVoronoiCollisionPiece> pieces, Budget &budget,
                                                            ChunkMesh &mesh) {
        for (const auto &piece : pieces) {
            if (!budget.Charge(sizeof(ChunkCollisionPiece) + piece.positions.size() * sizeof(piece.positions.front()) +
                                   piece.triangles.size() * sizeof(piece.triangles.front()),
                               piece.positions.size() + piece.triangles.size()))
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::LimitExceeded));
            ChunkCollisionPiece output{{}, piece.positions, piece.triangles, piece.volume};
            output.id = PieceIdentity(output);
            mesh.collisionPieces.push_back(std::move(output));
        }
        std::ranges::sort(mesh.collisionPieces, {}, &ChunkCollisionPiece::id);
        if (std::ranges::adjacent_find(mesh.collisionPieces, {}, &ChunkCollisionPiece::id) != mesh.collisionPieces.end())
            return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
        return Result<void>::Success();
    }

    inline void SetMeshBounds(ChunkMesh &mesh) {
        mesh.mass.minimum = mesh.vertices.front().position;
        mesh.mass.maximum = mesh.mass.minimum;
        for (const auto &vertex : mesh.vertices) {
            for (std::size_t axis = 0; axis < 3; ++axis) {
                mesh.mass.minimum[axis] = std::min(mesh.mass.minimum[axis], vertex.position[axis]);
                mesh.mass.maximum[axis] = std::max(mesh.mass.maximum[axis], vertex.position[axis]);
            }
        }
    }

    using InteriorPair = std::pair<DestructionChunkId, DestructionChunkId>;

    struct InteriorArea final {
        double low{};
        double high{};
    };

    [[nodiscard]] inline Result<void> CheckInterior(const OfflineVoronoiChunk &source, const std::map<DestructionChunkId, Point> &sites,
                                                    const std::array<Point, 3> &points, const Point &normal, double area,
                                                    std::map<InteriorPair, InteriorArea> &areas) {
        DestructionChunkId match{};
        for (const auto neighbor : source.neighbors) {
            const auto it = sites.find(neighbor);
            if (it == sites.end() || neighbor == source.id)
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInterior));
            const Point middle{(source.site[0] + it->second[0]) * 0.5, (source.site[1] + it->second[1]) * 0.5,
                               (source.site[2] + it->second[2]) * 0.5};
            const Point direction = Sub(it->second, source.site);
            const double distance = std::sqrt(Dot(direction, direction));
            if (!(distance > 1.0e-9))
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInterior));
            const double tolerance = 2.0e-4 * std::max(1.0, distance);
            if (std::abs(Dot(Sub(points[0], middle), direction) / distance) < tolerance &&
                std::abs(Dot(Sub(points[1], middle), direction) / distance) < tolerance &&
                std::abs(Dot(Sub(points[2], middle), direction) / distance) < tolerance) {
                if (Dot(normal, direction) <= 0)
                    return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInterior));
                if (match.IsValid())
                    return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInterior));
                match = neighbor;
            }
        }
        if (!match.IsValid())
            return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInterior));
        auto &pair = areas[std::minmax(source.id, match)];
        if (source.id < match)
            pair.low += area;
        else
            pair.high += area;
        return Result<void>::Success();
    }

    [[nodiscard]] inline Result<void> AppendFace(const OfflineVoronoiChunk &source, const OfflineVoronoiTriangle &face,
                                                 const ChunkUvPolicy uv, const std::map<DestructionChunkId, Point> &sites,
                                                 std::map<InteriorPair, InteriorArea> &areas, ChunkMesh &target) {
        if (std::ranges::any_of(face.indices, [&](std::uint32_t index) {
            return index >= source.positions.size();
        }))
            return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
        const std::array<Point, 3> points{Position(source.positions[face.indices[0]]), Position(source.positions[face.indices[1]]),
                                          Position(source.positions[face.indices[2]])};
        if (!Finite(points[0]) || !Finite(points[1]) || !Finite(points[2]))
            return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
        const Point cross = Cross(Sub(points[1], points[0]), Sub(points[2], points[0]));
        const double length = std::sqrt(Dot(cross, cross));
        if (!(length > 1.0e-12) || !std::isfinite(length))
            return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
        if (face.interior && !sites.empty()) {
            if (auto checked = CheckInterior(source, sites, points, cross, length * 0.5, areas); checked.HasError())
                return checked;
        }
        const Point normal{cross[0] / length, cross[1] / length, cross[2] / length};
        const Point reference = std::abs(normal[0]) < 0.8 ? Point{1, 0, 0} : Point{0, 0, 1};
        const Point projected = Sub(reference, Point{normal[0] * Dot(reference, normal), normal[1] * Dot(reference, normal),
                                                     normal[2] * Dot(reference, normal)});
        const double tangentLength = std::sqrt(Dot(projected, projected));
        const Point tangent{projected[0] / tangentLength, projected[1] / tangentLength, projected[2] / tangentLength};
        const Point bitangent = Cross(normal, tangent);
        const double scale = face.interior ? uv.interiorScale : uv.exteriorScale;
        ChunkMeshFace outputFace{{}, face.materialSlot, face.interior};
        for (const Point &point : points) {
            const double u = Dot(point, tangent) * scale;
            const double v = Dot(point, bitangent) * scale;
            if (!std::isfinite(u) || !std::isfinite(v) || std::abs(u) > std::numeric_limits<float>::max() ||
                std::abs(v) > std::numeric_limits<float>::max())
                return Result<void>::Failure(MakeError(ChunkMeshCookErrors::InvalidInput));
            const auto index = static_cast<std::uint32_t>(target.vertices.size());
            outputFace.indices[index % 3U] = index;
            target.vertices.push_back(
                {{static_cast<float>(point[0]), static_cast<float>(point[1]), static_cast<float>(point[2])},
                 {static_cast<float>(normal[0]), static_cast<float>(normal[1]), static_cast<float>(normal[2])},
                 {static_cast<float>(tangent[0]), static_cast<float>(tangent[1]), static_cast<float>(tangent[2]), 1.0F},
                 {static_cast<float>(u), static_cast<float>(v)}});
        }
        target.faces.push_back(outputFace);
        return Result<void>::Success();
    }
}  // namespace Horo::Destruction::ChunkMeshDetail
