#include "Horo/Terrain/TerrainSourceArtifacts.h"

#include "TerrainTileCookCodec.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Horo::Terrain {
    namespace {
        using Detail::CanonicalWriter;

        /** @brief Exact source-grid coverage read only after the owning tile verifier succeeds. */
        struct Grid final {
            std::vector<std::uint32_t> xs, zs;
        };

        std::uint32_t ReadU32(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
            std::uint32_t value{};
            for (std::uint8_t byte = 0; byte < 4; ++byte)
                value |= static_cast<std::uint32_t>(bytes[offset + byte]) << (byte * 8U);
            return value;
        }

        Grid TileGrid(const TerrainCookedTile &tile) {
            const auto stride = ReadU32(tile.payload, 139);
            Grid grid;
            const auto positions = [stride](std::vector<std::uint32_t> &values, const auto begin, const auto end) {
                values.reserve((end - begin + stride - 1) / stride + 1);
                for (auto value = begin; value < end; value += stride)
                    values.push_back(value);
                values.push_back(end);
            };
            positions(grid.xs, ReadU32(tile.payload, 123), ReadU32(tile.payload, 127));
            positions(grid.zs, ReadU32(tile.payload, 131), ReadU32(tile.payload, 135));
            return grid;
        }

        /** @brief Complete retained allocation ledger; no dependency vector or encoded tile manifest is retained by TileSet. */
        std::uint64_t OwnedBytes(const CookedTerrainTileSet &tiles, const std::vector<TerrainSourceArtifact> &artifacts,
                                 const std::size_t manifestCapacity) {
            auto bytes = static_cast<std::uint64_t>(sizeof(CookedTerrainSourceArtifacts)) + tiles.coordinates.projectedCrs.capacity() +
                         tiles.tiles.capacity() * sizeof(TerrainCookedTile) + artifacts.capacity() * sizeof(TerrainSourceArtifact) +
                         manifestCapacity;
            for (const auto &tile : tiles.tiles)
                bytes += tile.payload.capacity();
            for (const auto &artifact : artifacts)
                bytes += artifact.vertices.capacity() * sizeof(TerrainSourceVertex) +
                         artifact.triangles.capacity() * sizeof(TerrainSourceTriangle) + artifact.payload.capacity();
            return bytes;
        }

        bool ValidProfile(const TerrainSourceArtifactProfile &profile) {
            if (!Detail::ValidProfile(profile.tiles) || profile.collisionLod >= profile.tiles.lodLevels ||
                profile.navigationLod >= profile.tiles.lodLevels)
                return false;
            const auto limits = GetTerrainTierProfile(profile.tiles.tier).Value().limits;
            const auto within = [](const std::uint64_t requested, const std::uint64_t ceiling) {
                return requested != 0 && requested <= ceiling;
            };
            return within(profile.maximumVertices, limits.maximumWorkItems) &&
                   within(profile.maximumTriangles, limits.maximumWorkItems * 2) &&
                   within(profile.maximumOwnedBytes, limits.maximumStagingBytes) &&
                   within(profile.maximumWorkItems, limits.maximumWorkItems);
        }

        Sha256Digest Fingerprint(const CookedTerrainTileSet &tiles, const TerrainSourceArtifactProfile &profile) {
            Sha256Builder hash;
            CanonicalWriter writer{nullptr, &hash};
            writer.Text("horo.terrain.source-artifacts.v1");
            writer.Bytes(tiles.fingerprint.bytes);
            writer.Bytes(tiles.manifestDigest.bytes);
            writer.Unsigned(CurrentTerrainSourceArtifactSchema, 4);
            writer.Byte(profile.collisionLod);
            writer.Byte(profile.navigationLod);
            writer.Unsigned(profile.maximumVertices, 8);
            writer.Unsigned(profile.maximumTriangles, 8);
            writer.Unsigned(profile.maximumOwnedBytes, 8);
            writer.Unsigned(profile.maximumWorkItems, 8);
            writer.Flush();
            return hash.Finalize();
        }

        /** @brief Subtraction-based aggregate admission avoids overflow and precedes geometry allocation. */
        struct Budget final {
            std::uint64_t vertices{}, triangles{}, bytes{}, work{};

            bool Admit(const Grid &grid, const TerrainSourceArtifactProfile &profile, const std::uint64_t overhead) {
                const auto v = static_cast<std::uint64_t>(grid.xs.size()) * grid.zs.size();
                const auto t = 2ULL * (grid.xs.size() - 1) * (grid.zs.size() - 1);
                // Each coarse quad visits all enclosed source samples once for both holes and error.
                const auto visitsX = grid.xs.back() - grid.xs.front() + grid.xs.size() - 1;
                const auto visitsZ = grid.zs.back() - grid.zs.front() + grid.zs.size() - 1;
                const auto w = v + t + static_cast<std::uint64_t>(visitsX) * visitsZ;
                const auto b = overhead + v * (sizeof(TerrainSourceVertex) + 24) + t * (sizeof(TerrainSourceTriangle) + 28);
                if (v > profile.maximumVertices - vertices || t > profile.maximumTriangles - triangles ||
                    b > profile.maximumOwnedBytes - bytes || w > profile.maximumWorkItems - work)
                    return false;
                vertices += v;
                triangles += t;
                bytes += b;
                work += w;
                return true;
            }
        };

        double Height(const TerrainCanonicalSource &source, const std::uint32_t x, const std::uint32_t z) {
            return source.heightsMeters[static_cast<std::size_t>(z) * source.width + x];
        }

        /** @brief Both original and coarse triangles are convex height interpolants inside this source-sample range. */
        Result<bool> SolidQuad(const TerrainCanonicalSource &source, const TerrainSourceTriangle &quad,
                               const CancellationToken &cancellation, double &error) {
            bool solid = true;
            auto minimum = Height(source, quad.beginX, quad.beginZ);
            auto maximum = minimum;
            for (auto z = quad.beginZ; z <= quad.endZ; ++z) {
                if (cancellation.IsCancellationRequested())
                    return Result<bool>::Failure(MakeError(TerrainTileCookErrors::Cancelled));
                for (auto x = quad.beginX; x <= quad.endX; ++x) {
                    const auto index = static_cast<std::size_t>(z) * source.width + x;
                    solid = solid && (source.holes.empty() || source.holes[index] == 0);
                    const auto height = Height(source, x, z);
                    minimum = std::min(minimum, height);
                    maximum = std::max(maximum, height);
                }
            }
            if (solid && (quad.endX - quad.beginX > 1 || quad.endZ - quad.beginZ > 1))
                error = std::max(error, maximum - minimum);
            return Result<bool>::Success(solid);
        }

        Result<void> BuildGeometry(const TerrainCanonicalSource &source, const Grid &grid, const CancellationToken &cancellation,
                                   TerrainSourceArtifact &artifact) {
            artifact.vertices.reserve(grid.xs.size() * grid.zs.size());
            artifact.triangles.reserve(2 * (grid.xs.size() - 1) * (grid.zs.size() - 1));
            for (const auto z : grid.zs) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(TerrainTileCookErrors::Cancelled));
                for (const auto x : grid.xs)
                    artifact.vertices.push_back({source.coordinates.originX + x * source.coordinates.spacingX, Height(source, x, z),
                                                 source.coordinates.originZ + z * source.coordinates.spacingZ});
            }
            for (std::uint32_t z = 0; z + 1 < grid.zs.size(); ++z) {
                for (std::uint32_t x = 0; x + 1 < grid.xs.size(); ++x) {
                    const auto a = static_cast<std::uint32_t>(z * grid.xs.size() + x);
                    const auto b = a + 1;
                    const auto c = static_cast<std::uint32_t>(a + grid.xs.size());
                    const auto d = c + 1;
                    if (artifact.vertices[a].x >= artifact.vertices[b].x || artifact.vertices[a].z >= artifact.vertices[c].z)
                        return Result<void>::Failure(MakeError(TerrainSourceErrors::PrecisionLost));
                    TerrainSourceTriangle quad{{a, c, b}, grid.xs[x], grid.xs[x + 1], grid.zs[z], grid.zs[z + 1]};
                    const auto solid = SolidQuad(source, quad, cancellation, artifact.maximumGeometricError);
                    if (solid.HasError())
                        return Result<void>::Failure(solid.ErrorValue());
                    if (solid.Value()) {
                        artifact.triangles.push_back(quad);
                        quad.indices = {b, c, d};
                        artifact.triangles.push_back(quad);
                    }
                }
            }
            return Result<void>::Success();
        }

        Result<void> EncodeArtifact(const CookedTerrainTileSet &tiles, const TerrainCapabilityRevision capability,
                                    const Sha256Digest &fingerprint, TerrainSourceArtifact &artifact, const std::size_t coordinateBytes,
                                    const CancellationToken &cancellation) {
            artifact.payload.reserve(349 + coordinateBytes + 24 * artifact.vertices.size() + 28 * artifact.triangles.size());
            CanonicalWriter writer{&artifact.payload};
            writer.Text("HTSG");
            writer.Unsigned(CurrentTerrainSourceArtifactSchema, 4);
            writer.Bytes(SerializeTerrainTileId(artifact.tile));
            writer.Byte(static_cast<std::uint8_t>(artifact.role));
            writer.Unsigned(capability.Value(), 8);
            writer.Bytes(tiles.sourceAsset.Bytes());
            writer.Unsigned(tiles.sourceRevision.Value(), 8);
            writer.Bytes(tiles.sourceDigest.bytes);
            writer.Bytes(tiles.fingerprint.bytes);
            writer.Bytes(fingerprint.bytes);
            writer.Bytes(tiles.manifestDigest.bytes);
            Detail::WriteCoordinates(writer, tiles.coordinates);
            for (const auto &seam : artifact.seams)
                writer.Bytes(seam.bytes);
            writer.Double(artifact.maximumGeometricError);
            writer.Byte(artifact.requiresSameLodNeighbors ? 1 : 0);
            writer.Unsigned(artifact.vertices.size(), 8);
            writer.Unsigned(artifact.triangles.size(), 8);
            for (const auto &vertex : artifact.vertices) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(TerrainTileCookErrors::Cancelled));
                writer.Double(vertex.x);
                writer.Double(vertex.y);
                writer.Double(vertex.z);
            }
            for (const auto &triangle : artifact.triangles) {
                if (cancellation.IsCancellationRequested())
                    return Result<void>::Failure(MakeError(TerrainTileCookErrors::Cancelled));
                for (const auto index : triangle.indices)
                    writer.Unsigned(index, 4);
                writer.Unsigned(triangle.beginX, 4);
                writer.Unsigned(triangle.endX, 4);
                writer.Unsigned(triangle.beginZ, 4);
                writer.Unsigned(triangle.endZ, 4);
            }
            artifact.digest = ComputeSha256(std::as_bytes(std::span{artifact.payload}));
            return Result<void>::Success();
        }

        std::vector<std::uint8_t> Manifest(const CookedTerrainTileSet &tiles, const Sha256Digest &fingerprint,
                                           const std::span<const TerrainSourceArtifact> artifacts) {
            std::vector<std::uint8_t> bytes;
            bytes.reserve(82 + artifacts.size() * 66);
            CanonicalWriter writer{&bytes};
            writer.Text("HTSM");
            writer.Unsigned(CurrentTerrainSourceArtifactSchema, 4);
            writer.Bytes(tiles.manifestDigest.bytes);
            writer.Bytes(fingerprint.bytes);
            writer.Unsigned(artifacts.size(), 8);
            for (const auto &artifact : artifacts) {
                writer.Bytes(SerializeTerrainTileId(artifact.tile));
                writer.Byte(static_cast<std::uint8_t>(artifact.role));
                writer.Unsigned(artifact.payload.size(), 8);
                writer.Bytes(artifact.digest.bytes);
            }
            return bytes;
        }

        /** @brief Call-scoped candidate construction; borrowed inputs never escape into the immutable result. */
        struct ArtifactCandidate final {
            const TerrainCanonicalSource &source;
            const CookedTerrainTileSet &tiles;
            const TerrainSourceArtifactProfile &profile;
            const CancellationToken &cancellation;
            const Sha256Digest &fingerprint;
            const std::vector<std::uint8_t> &coordinateBytes;
            Budget budget;
            std::vector<TerrainSourceArtifact> artifacts;
            std::size_t artifactCount{};

            /** @brief Admit all retained headers and the coordinate scratch before reserving candidate storage. */
            Result<void> Prepare() {
                artifactCount = std::ranges::count_if(tiles.tiles, [this](const auto &tile) {
                    return tile.id.tile.lod == profile.collisionLod;
                }) + std::ranges::count_if(tiles.tiles, [this](const auto &tile) {
                    return tile.id.tile.lod == profile.navigationLod;
                }) + tiles.tiles.size();
                budget.bytes = sizeof(CookedTerrainSourceArtifacts) + coordinateBytes.capacity() +
                               tiles.coordinates.projectedCrs.capacity() + tiles.tiles.capacity() * sizeof(TerrainCookedTile) + 82 +
                               artifactCount * (66 + sizeof(TerrainSourceArtifact));
                for (const auto &tile : tiles.tiles)
                    budget.bytes += tile.payload.capacity();
                if (budget.bytes > profile.maximumOwnedBytes)
                    return Result<void>::Failure(MakeError(TerrainTileCookErrors::LimitExceeded));
                artifacts.reserve(artifactCount);
                budget.bytes += (artifacts.capacity() - artifactCount) * sizeof(TerrainSourceArtifact);
                if (budget.bytes > profile.maximumOwnedBytes)
                    return Result<void>::Failure(MakeError(TerrainTileCookErrors::LimitExceeded));
                return Result<void>::Success();
            }

            /** @brief Build one admitted role, then reconcile predicted bytes against actual retained capacities. */
            Result<void> BuildArtifact(const TerrainCookedTile &tile, const Grid &grid, const TerrainSourceArtifactRole role) {
                const auto scratchBytes = (grid.xs.capacity() + grid.zs.capacity()) * sizeof(std::uint32_t);
                if (scratchBytes > profile.maximumOwnedBytes - budget.bytes ||
                    !budget.Admit(grid, profile, 349 + coordinateBytes.size() + scratchBytes))
                    return Result<void>::Failure(MakeError(TerrainTileCookErrors::LimitExceeded));
                budget.bytes -= scratchBytes;  // Scratch is reused across roles, never retained by an artifact.
                TerrainSourceArtifact artifact;
                artifact.tile = tile.id;
                artifact.role = role;
                artifact.seams = tile.seams;
                const auto built = BuildGeometry(source, grid, cancellation, artifact);
                if (built.HasError())
                    return Result<void>::Failure(built.ErrorValue());
                const auto encoded = EncodeArtifact(tiles, source.capability, fingerprint, artifact, coordinateBytes.size(), cancellation);
                if (encoded.HasError())
                    return Result<void>::Failure(encoded.ErrorValue());
                const auto integrity = VerifyTerrainSourceArtifactPayload(artifact.payload, artifact.digest);
                if (integrity.HasError())
                    return Result<void>::Failure(integrity.ErrorValue());
                artifacts.push_back(std::move(artifact));
                const auto actual = OwnedBytes(tiles, artifacts, 82 + artifactCount * 66) + coordinateBytes.capacity() + scratchBytes;
                if (actual > profile.maximumOwnedBytes)
                    return Result<void>::Failure(MakeError(TerrainTileCookErrors::LimitExceeded));
                budget.bytes = std::max(budget.bytes, actual - scratchBytes);
                return Result<void>::Success();
            }

            /** @brief Reuse one tile grid across its explicitly selected consumer roles. */
            Result<void> Build() {
                const auto prepared = Prepare();
                if (prepared.HasError())
                    return prepared;
                constexpr std::array roles{TerrainSourceArtifactRole::Visual, TerrainSourceArtifactRole::Collision,
                                           TerrainSourceArtifactRole::Navigation};
                for (const auto &tile : tiles.tiles) {
                    const auto grid = TileGrid(tile);
                    for (const auto role : roles) {
                        if ((role == TerrainSourceArtifactRole::Collision && tile.id.tile.lod != profile.collisionLod) ||
                            (role == TerrainSourceArtifactRole::Navigation && tile.id.tile.lod != profile.navigationLod))
                            continue;
                        const auto built = BuildArtifact(tile, grid, role);
                        if (built.HasError())
                            return built;
                    }
                }
                return Result<void>::Success();
            }
        };
    }  // namespace

    CookedTerrainSourceArtifacts::CookedTerrainSourceArtifacts(CookedTerrainTileSet tiles, const TerrainCapabilityRevision capability,
                                                               const Sha256Digest fingerprint, std::vector<TerrainSourceArtifact> artifacts,
                                                               std::vector<std::uint8_t> manifest)
        : tiles_(std::move(tiles)), capability_(capability), fingerprint_(fingerprint), artifacts_(std::move(artifacts)),
          manifest_(std::move(manifest)), manifestDigest_(ComputeSha256(std::as_bytes(std::span{manifest_}))) {}

    /** @copydoc CookedTerrainSourceArtifacts::ValidateCurrent */
    Result<void> CookedTerrainSourceArtifacts::ValidateCurrent(const TerrainCanonicalSource &source) const {
        if (tiles_.tiles.empty() || artifacts_.empty())
            return Result<void>::Failure(MakeError(TerrainTileCookErrors::CorruptPrevious));
        if (source.dataset != tiles_.dataset || source.sourceAsset != tiles_.sourceAsset || source.revision != tiles_.sourceRevision ||
            source.capability != capability_)
            return Result<void>::Failure(MakeError(TerrainSourceErrors::RevisionStale));
        // Validate hostile shape/channel sizes before SourceDigest reads any sample.
        const auto checked = CookTerrainTiles(source, tiles_.profile, {}, {});
        if (checked.HasError())
            return Result<void>::Failure(checked.ErrorValue());
        if (checked.Value().sourceDigest != tiles_.sourceDigest)
            return Result<void>::Failure(MakeError(TerrainSourceErrors::RevisionStale));
        return Result<void>::Success();
    }

    /** @copydoc CookTerrainSourceArtifacts */
    Result<CookedTerrainSourceArtifacts> CookTerrainSourceArtifacts(const TerrainCanonicalSource &source,
                                                                    const TerrainSourceArtifactProfile &profile,
                                                                    const std::span<const TerrainTileCookDependency> dependencies,
                                                                    const CancellationToken &cancellation) {
        if (!ValidProfile(profile))
            return Result<CookedTerrainSourceArtifacts>::Failure(MakeError(TerrainTileCookErrors::InvalidProfile));
        auto cooked = CookTerrainTiles(source, profile.tiles, dependencies, cancellation);
        if (cooked.HasError())
            return Result<CookedTerrainSourceArtifacts>::Failure(cooked.ErrorValue());
        auto tiles = std::move(cooked).Value();
        // Verification owns byte-layout admission; geometry never decodes an unverified HTIL prefix.
        const auto verified = VerifyCookedTerrainTiles(tiles);
        if (verified.HasError())
            return Result<CookedTerrainSourceArtifacts>::Failure(verified.ErrorValue());
        std::vector<std::uint8_t> coordinateBytes;
        CanonicalWriter coordinates{&coordinateBytes};
        Detail::WriteCoordinates(coordinates, tiles.coordinates);
        const auto fingerprint = Fingerprint(tiles, profile);
        ArtifactCandidate candidate{source, tiles, profile, cancellation, fingerprint, coordinateBytes, {}, {}, 0};
        const auto built = candidate.Build();
        if (built.HasError())
            return Result<CookedTerrainSourceArtifacts>::Failure(built.ErrorValue());
        if (cancellation.IsCancellationRequested())
            return Result<CookedTerrainSourceArtifacts>::Failure(MakeError(TerrainTileCookErrors::Cancelled));
        auto manifest = Manifest(tiles, fingerprint, candidate.artifacts);
        if (OwnedBytes(tiles, candidate.artifacts, manifest.capacity()) + coordinateBytes.capacity() > profile.maximumOwnedBytes)
            return Result<CookedTerrainSourceArtifacts>::Failure(MakeError(TerrainTileCookErrors::LimitExceeded));
        if (cancellation.IsCancellationRequested())
            return Result<CookedTerrainSourceArtifacts>::Failure(MakeError(TerrainTileCookErrors::Cancelled));
        return Result<CookedTerrainSourceArtifacts>::Success(CookedTerrainSourceArtifacts{std::move(tiles), source.capability, fingerprint,
                                                                                          std::move(candidate.artifacts),
                                                                                          std::move(manifest)});
    }
}  // namespace Horo::Terrain
