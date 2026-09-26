#include "OfflineVoronoiProvenance.h"

#include <bit>
#include <cstddef>

namespace Horo::Destruction {
    namespace {
        void AppendU64(std::vector<std::byte> &bytes, std::uint64_t value) {
            for (int shift = 56; shift >= 0; shift -= 8)
                bytes.push_back(static_cast<std::byte>(value >> shift));
        }

        void AppendU32(std::vector<std::byte> &bytes, std::uint32_t value) {
            AppendU64(bytes, value);
        }

        void AppendDigest(std::vector<std::byte> &bytes, const Sha256Digest &digest) {
            for (const auto value : digest.bytes)
                bytes.push_back(static_cast<std::byte>(value));
        }

        void AppendPoint(std::vector<std::byte> &bytes, const std::array<double, 3> &point) {
            for (const double value : point)
                AppendU64(bytes, std::bit_cast<std::uint64_t>(value));
        }

        struct OutputHasher final {
            std::uint64_t value{14695981039346656037ULL};

            void Mix(const std::uint64_t input) {
                for (unsigned shift = 0; shift < 64; shift += 8) {
                    value ^= static_cast<std::uint8_t>(input >> shift);
                    value *= 1099511628211ULL;
                }
            }
        };

        void HashPositions(OutputHasher &hash, const std::vector<std::array<float, 3>> &positions) {
            hash.Mix(positions.size());
            for (const auto &position : positions) {
                for (const float coordinate : position)
                    hash.Mix(std::bit_cast<std::uint32_t>(coordinate));
            }
        }

        void HashCollisionPiece(OutputHasher &hash, const OfflineVoronoiCollisionPiece &piece) {
            HashPositions(hash, piece.positions);
            hash.Mix(piece.triangles.size());
            for (const auto &triangle : piece.triangles) {
                for (const auto index : triangle)
                    hash.Mix(index);
            }
            hash.Mix(std::bit_cast<std::uint64_t>(piece.volume));
        }

        void HashChunk(OutputHasher &hash, const OfflineVoronoiChunk &chunk) {
            hash.Mix(chunk.id.Value());
            for (const double coordinate : chunk.site)
                hash.Mix(std::bit_cast<std::uint64_t>(coordinate));
            HashPositions(hash, chunk.positions);
            hash.Mix(chunk.triangles.size());
            for (const auto &triangle : chunk.triangles) {
                for (const auto index : triangle.indices)
                    hash.Mix(index);
                hash.Mix(triangle.materialSlot);
                hash.Mix(triangle.interior ? 1U : 0U);
            }
            hash.Mix(chunk.collisionPieces.size());
            for (const auto &piece : chunk.collisionPieces)
                HashCollisionPiece(hash, piece);
            hash.Mix(chunk.neighbors.size());
            for (const auto neighbor : chunk.neighbors)
                hash.Mix(neighbor.Value());
            hash.Mix(std::bit_cast<std::uint64_t>(chunk.volume));
            for (const double coordinate : chunk.centerOfMass)
                hash.Mix(std::bit_cast<std::uint64_t>(coordinate));
        }
    }  // namespace

    namespace Detail {
        Sha256Digest VoronoiFingerprint(const OfflineVoronoiSource &source, const OfflineVoronoiRecipe &recipe,
                                        const std::vector<std::array<double, 3>> &sites) {
            std::vector<std::byte> bytes;
            AppendU32(bytes, OfflineVoronoiSchemaVersion);
            for (const auto value : source.asset.Bytes())
                bytes.push_back(static_cast<std::byte>(value));
            AppendU64(bytes, source.revision);
            AppendDigest(bytes, source.digest);
            AppendU64(bytes, recipe.id);
            AppendU64(bytes, recipe.revision);
            AppendU64(bytes, recipe.seed);
            AppendU32(bytes, recipe.siteCount);
            for (const auto id : recipe.siteIds)
                AppendU64(bytes, id.Value());
            for (const auto &site : sites)
                AppendPoint(bytes, site);
            AppendU32(bytes, recipe.interiorMaterialSlot);
            AppendU32(bytes, recipe.toolchainVersion);
            AppendDigest(bytes, recipe.toolchainDigest);
            AppendU32(bytes, static_cast<std::uint32_t>(recipe.tier));
            const auto &limits = recipe.limits;
            for (const auto value :
                 {static_cast<std::uint64_t>(limits.maximumChunksPerDestructible), static_cast<std::uint64_t>(limits.maximumHierarchyDepth),
                  static_cast<std::uint64_t>(limits.maximumActiveChunkBodies),
                  static_cast<std::uint64_t>(limits.maximumEventsPerTransition),
                  static_cast<std::uint64_t>(limits.maximumEventJournalEntries),
                  static_cast<std::uint64_t>(limits.maximumCosmeticDebrisParticles), limits.maximumArtifactBytes,
                  limits.maximumTransitionBytes, limits.maximumResidentBytes, limits.maximumWorkItemsPerTransition, recipe.maximumVertices,
                  recipe.maximumTriangles, recipe.maximumWorkItems, static_cast<std::uint64_t>(recipe.maximumConvexRegions)})
                AppendU64(bytes, value);
            return ComputeSha256(bytes);
        }

        std::uint64_t VoronoiOutputChecksum(const OfflineVoronoiCandidate &candidate) {
            OutputHasher hash;
            hash.Mix(candidate.estimatedBytes);
            hash.Mix(candidate.workItems);
            hash.Mix(candidate.chunks.size());
            for (const auto &chunk : candidate.chunks)
                HashChunk(hash, chunk);
            return hash.value;
        }
    }  // namespace Detail

    /** @copydoc ComputeOfflineVoronoiSourceDigest */
    Sha256Digest ComputeOfflineVoronoiSourceDigest(const OfflineVoronoiSource &source) {
        std::vector<std::byte> bytes;
        AppendU32(bytes, OfflineVoronoiSchemaVersion);
        AppendU64(bytes, source.positions.size());
        for (const auto &point : source.positions) {
            for (const float value : point)
                AppendU32(bytes, std::bit_cast<std::uint32_t>(value));
        }
        AppendU64(bytes, source.indices.size());
        for (const auto value : source.indices)
            AppendU32(bytes, value);
        AppendU64(bytes, source.materialSlots.size());
        for (const auto value : source.materialSlots)
            AppendU32(bytes, value);
        return ComputeSha256(bytes);
    }
}  // namespace Horo::Destruction
