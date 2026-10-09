#include "Horo/Terrain/TerrainTileCook.h"

#include "TerrainTileCookCodec.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>

namespace Horo::Terrain {
    using Detail::CanonicalWriter;
    using Detail::EdgeDigest;
    using Detail::Fingerprint;
    using Detail::ManifestDigest;
    using Detail::Nonzero;
    using Detail::SamplePositions;
    using Detail::SourceDigest;
    using Detail::ValidCoordinates;
    using Detail::ValidProfile;
    using Detail::WorldTileOrigin;
    using Detail::WriteCoordinates;
    using Detail::WriteSample;

    namespace {
        constexpr std::string_view TileMagic = "HTIL";

        bool ValidSource(const TerrainCanonicalSource &source) {
            if (!source.dataset.IsValid() || !source.sourceAsset.IsValid() || !source.revision.IsValid() || !source.capability.IsValid() ||
                source.width < 2 || source.height < 2 || source.width > TerrainDescriptorHardLimits::SamplesPerAxis ||
                source.height > TerrainDescriptorHardLimits::SamplesPerAxis ||
                source.layerCount > TerrainDescriptorHardLimits::LayersPerTile || !ValidCoordinates(source.coordinates))
                return false;
            const auto samples = static_cast<std::uint64_t>(source.width) * source.height;
            if (samples > TerrainDescriptorHardLimits::WorkItems || source.heightsMeters.size() != samples ||
                source.weights.size() != samples * source.layerCount || (!source.holes.empty() && source.holes.size() != samples))
                return false;
            for (std::size_t index = 0; index < samples; ++index) {
                if (!std::isfinite(source.heightsMeters[index]) || (!source.holes.empty() && source.holes[index] > 1))
                    return false;
                if (source.layerCount != 0) {
                    std::uint32_t total{};
                    for (std::uint8_t layer = 0; layer < source.layerCount; ++layer)
                        total += source.weights[index * source.layerCount + layer];
                    if (total != 65'535)
                        return false;
                }
            }
            return std::isfinite(source.coordinates.originX + static_cast<double>(source.width - 1) * source.coordinates.spacingX) &&
                   std::isfinite(source.coordinates.originZ + static_cast<double>(source.height - 1) * source.coordinates.spacingZ);
        }

        bool PayloadMatchesProvenance(const TerrainCookedTile &tile, const CookedTerrainTileSet &cooked) {
            // Fixed v1 prefix: sized magic, schema, tile ID, source asset/revision and source/cook digests.
            if (constexpr std::size_t PrefixBytes = 123;
                tile.payload.size() < PrefixBytes || tile.payload[0] != 4 || tile.payload[1] != 0 ||
                !std::equal(TileMagic.begin(), TileMagic.end(), tile.payload.begin() + 2) ||
                tile.payload[6] != CurrentTerrainTileCookSchema || tile.payload[7] != 0 || tile.payload[8] != 0 || tile.payload[9] != 0)
                return false;
            if (const auto tileId = SerializeTerrainTileId(tile.id);
                !std::equal(tileId.begin(), tileId.end(), tile.payload.begin() + 10) ||
                !std::equal(cooked.sourceAsset.Bytes().begin(), cooked.sourceAsset.Bytes().end(), tile.payload.begin() + 35) ||
                !std::equal(cooked.sourceDigest.bytes.begin(), cooked.sourceDigest.bytes.end(), tile.payload.begin() + 59) ||
                !std::equal(cooked.fingerprint.bytes.begin(), cooked.fingerprint.bytes.end(), tile.payload.begin() + 91))
                return false;
            std::uint64_t revision{};
            for (std::uint8_t index = 0; index < 8; ++index)
                revision |= static_cast<std::uint64_t>(tile.payload[51 + index]) << (index * 8U);
            return revision == cooked.sourceRevision.Value();
        }

        std::uint32_t ReadU32(const std::vector<std::uint8_t> &bytes, const std::size_t start) {
            std::uint32_t result{};
            for (std::uint8_t index = 0; index < 4; ++index)
                result |= static_cast<std::uint32_t>(bytes[start + index]) << (index * 8U);
            return result;
        }

        bool ValidTileSamples(const TerrainCookedTile &tile, const CookedTerrainTileSet &cooked, const CancellationToken &cancellation) {
            const auto sampleBytes = 5U + 2U * cooked.layerCount;
            const auto sampleCount = static_cast<std::size_t>(tile.samplesX) * tile.samplesZ;
            const auto start = tile.payload.size() - sampleCount * sampleBytes;
            for (std::size_t sample = 0; sample < sampleCount; ++sample) {
                if (cancellation.IsCancellationRequested())
                    return false;
                const auto offset = start + sample * sampleBytes;
                if (!std::isfinite(std::bit_cast<float>(ReadU32(tile.payload, offset))))
                    return false;
                if (cooked.layerCount != 0) {
                    std::uint32_t total{};
                    for (std::uint8_t layer = 0; layer < cooked.layerCount; ++layer) {
                        const auto weight = offset + 4 + layer * 2;
                        total +=
                            static_cast<std::uint16_t>(tile.payload[weight]) | (static_cast<std::uint16_t>(tile.payload[weight + 1]) << 8U);
                    }
                    if (total != 65'535)
                        return false;
                }
                const auto hole = tile.payload[offset + sampleBytes - 1];
                if (hole > 1 || (!cooked.hasHoles && hole != 0))
                    return false;
            }
            return true;
        }

        Sha256Digest EncodedEdgeDigest(const TerrainCookedTile &tile, const CookedTerrainTileSet &cooked, const bool alongZ,
                                       const bool highEdge) {
            const auto sampleBytes = 5U + 2U * cooked.layerCount;
            const auto sampleCount = static_cast<std::size_t>(tile.samplesX) * tile.samplesZ;
            const auto start = tile.payload.size() - sampleCount * sampleBytes;
            Sha256Builder hash;
            CanonicalWriter writer{nullptr, &hash};
            writer.Text("horo.terrain.seam.v1");
            writer.Unsigned(ReadU32(tile.payload, 139), 4);
            writer.Byte(cooked.layerCount);
            writer.Byte(alongZ ? 1 : 0);
            writer.Unsigned(alongZ ? tile.samplesZ : tile.samplesX, 4);
            const auto edgeSamples = alongZ ? tile.samplesZ : tile.samplesX;
            for (std::uint32_t sample = 0; sample < edgeSamples; ++sample) {
                auto x = sample;
                auto z = highEdge ? tile.samplesZ - 1 : 0;
                if (alongZ) {
                    x = highEdge ? tile.samplesX - 1 : 0;
                    z = sample;
                }
                const auto offset = start + (static_cast<std::size_t>(z) * tile.samplesX + x) * sampleBytes;
                for (std::uint32_t byte = 0; byte < sampleBytes; ++byte)
                    writer.Byte(tile.payload[offset + byte]);
            }
            writer.Flush();
            return hash.Finalize();
        }

        bool ValidTileSeams(const TerrainCookedTile &tile, const CookedTerrainTileSet &cooked) {
            return tile.seams[0] == EncodedEdgeDigest(tile, cooked, true, false) &&
                   tile.seams[1] == EncodedEdgeDigest(tile, cooked, true, true) &&
                   tile.seams[2] == EncodedEdgeDigest(tile, cooked, false, false) &&
                   tile.seams[3] == EncodedEdgeDigest(tile, cooked, false, true);
        }

        bool TileMatchesLayout(const TerrainCookedTile &tile, const CookedTerrainTileSet &cooked, const std::uint32_t beginX,
                               const std::uint32_t endX, const std::uint32_t beginZ, const std::uint32_t endZ, const std::uint32_t stride) {
            const auto nx = (endX - beginX + stride - 1) / stride + 1;
            const auto nz = (endZ - beginZ + stride - 1) / stride + 1;
            std::vector<std::uint8_t> encodedCoordinates;
            CanonicalWriter writer{&encodedCoordinates};
            WriteCoordinates(writer, cooked.coordinates);
            if (const auto expectedBytes =
                    153ULL + encodedCoordinates.size() + static_cast<std::uint64_t>(nx) * nz * (5ULL + 2ULL * cooked.layerCount);
                tile.samplesX != nx || tile.samplesZ != nz || tile.payload.size() != expectedBytes)
                return false;
            return ReadU32(tile.payload, 123) == beginX && ReadU32(tile.payload, 127) == endX && ReadU32(tile.payload, 131) == beginZ &&
                   ReadU32(tile.payload, 135) == endZ && ReadU32(tile.payload, 139) == stride && ReadU32(tile.payload, 143) == nx &&
                   ReadU32(tile.payload, 147) == nz && tile.payload[151] == cooked.layerCount &&
                   tile.payload[152] == (cooked.hasHoles ? 1 : 0) &&
                   std::equal(encodedCoordinates.begin(), encodedCoordinates.end(), tile.payload.begin() + 153);
        }

        struct ManifestLevel final {
            std::uint8_t lod{};
            std::uint32_t stride{};
            std::uint32_t tileQuads{};
            std::uint64_t countX{};
            std::int32_t originX{};
            std::int32_t originZ{};
        };

        struct ManifestScan final {
            std::size_t cursor{};
            std::uint64_t totalBytes{};
            std::uint64_t totalWork{};
        };

        bool ValidManifestTile(const CookedTerrainTileSet &cooked, ManifestScan &scan, const ManifestLevel &level, const std::uint32_t x,
                               const std::uint32_t z) {
            const auto &tile = cooked.tiles[scan.cursor];
            const auto address = TerrainTileCoordinate{static_cast<std::int32_t>(static_cast<std::int64_t>(level.originX) + x),
                                                       static_cast<std::int32_t>(static_cast<std::int64_t>(level.originZ) + z), level.lod};
            const auto beginX = x * level.tileQuads;
            const auto beginZ = z * level.tileQuads;
            const auto endX = std::min(beginX + level.tileQuads, cooked.sourceWidth - 1);
            if (const auto endZ = std::min(beginZ + level.tileQuads, cooked.sourceHeight - 1);
                tile.id.dataset != cooked.dataset || tile.id.tile != address ||
                !TileMatchesLayout(tile, cooked, beginX, endX, beginZ, endZ, level.stride) ||
                (x != 0 && cooked.tiles[scan.cursor - 1].seams[1] != tile.seams[0]) ||
                (z != 0 && cooked.tiles[scan.cursor - level.countX].seams[3] != tile.seams[2]))
                return false;
            const auto visits = static_cast<std::uint64_t>(tile.samplesX) * tile.samplesZ;
            if (tile.payload.size() > cooked.profile.maximumPayloadBytes - scan.totalBytes ||
                visits > cooked.profile.maximumWorkItems - scan.totalWork)
                return false;
            scan.totalBytes += tile.payload.size();
            scan.totalWork += visits;
            ++scan.cursor;
            return true;
        }

        bool ValidManifestLevel(const CookedTerrainTileSet &cooked, ManifestScan &scan, const ManifestLevel &level,
                                const std::uint64_t countZ, const CancellationToken &cancellation) {
            for (std::uint32_t z = 0; z < countZ; ++z) {
                for (std::uint32_t x = 0; x < level.countX; ++x) {
                    if (cancellation.IsCancellationRequested() || !ValidManifestTile(cooked, scan, level, x, z))
                        return false;
                }
            }
            return true;
        }

        bool ManifestLayoutValid(const CookedTerrainTileSet &cooked, const CancellationToken &cancellation) {
            if (!ValidProfile(cooked.profile) || !ValidCoordinates(cooked.coordinates) || cooked.sourceWidth < 2 || cooked.sourceHeight < 2)
                return false;
            const auto tier = GetTerrainTierProfile(cooked.profile.tier);
            if (const auto &limits = tier.Value().limits; cooked.sourceWidth > limits.maximumSamplesPerAxis ||
                                                          cooked.sourceHeight > limits.maximumSamplesPerAxis ||
                                                          cooked.layerCount > limits.maximumLayersPerTile)
                return false;
            ManifestScan scan;
            for (std::uint8_t lod = 0; lod < cooked.profile.lodLevels; ++lod) {
                const auto stride = std::uint32_t{1} << lod;
                const auto tileQuads = cooked.profile.interiorQuads * stride;
                const auto countX = (static_cast<std::uint64_t>(cooked.sourceWidth - 2) / tileQuads) + 1;
                const auto countZ = (static_cast<std::uint64_t>(cooked.sourceHeight - 2) / tileQuads) + 1;
                if (countX * countZ > cooked.tiles.size() - scan.cursor)
                    return false;
                const auto originX = WorldTileOrigin(cooked.coordinates.originX, cooked.coordinates.spacingX, tileQuads, countX);
                const auto originZ = WorldTileOrigin(cooked.coordinates.originZ, cooked.coordinates.spacingZ, tileQuads, countZ);
                if (!originX.has_value() || !originZ.has_value())
                    return false;
                const ManifestLevel level{lod, stride, tileQuads, countX, *originX, *originZ};
                if (!ValidManifestLevel(cooked, scan, level, countZ, cancellation))
                    return false;
            }
            return scan.cursor == cooked.tiles.size() && scan.cursor <= cooked.profile.maximumTiles;
        }

        struct TileBuildAddress final {
            TerrainTileCoordinate tile{};
            std::uint32_t stride{};
            std::uint32_t beginX{};
            std::uint32_t endX{};
            std::uint32_t beginZ{};
            std::uint32_t endZ{};
        };

        TerrainCookedTile BuildTile(const TerrainCanonicalSource &source, const Sha256Digest &sourceDigest, const Sha256Digest &fingerprint,
                                    const TileBuildAddress &address) {
            TerrainCookedTile tile;
            tile.id = {.dataset = source.dataset, .tile = address.tile};
            const auto xs = SamplePositions(address.beginX, address.endX, address.stride);
            const auto zs = SamplePositions(address.beginZ, address.endZ, address.stride);
            tile.samplesX = static_cast<std::uint32_t>(xs.size());
            tile.samplesZ = static_cast<std::uint32_t>(zs.size());
            tile.seams = {EdgeDigest(source, xs, zs, address.stride, true, false), EdgeDigest(source, xs, zs, address.stride, true, true),
                          EdgeDigest(source, xs, zs, address.stride, false, false),
                          EdgeDigest(source, xs, zs, address.stride, false, true)};
            CanonicalWriter writer{&tile.payload};
            writer.Text(TileMagic);
            writer.Unsigned(CurrentTerrainTileCookSchema, 4);
            writer.Bytes(SerializeTerrainTileId(tile.id));
            writer.Bytes(source.sourceAsset.Bytes());
            writer.Unsigned(source.revision.Value(), 8);
            writer.Bytes(sourceDigest.bytes);
            writer.Bytes(fingerprint.bytes);
            writer.Unsigned(address.beginX, 4);
            writer.Unsigned(address.endX, 4);
            writer.Unsigned(address.beginZ, 4);
            writer.Unsigned(address.endZ, 4);
            writer.Unsigned(address.stride, 4);
            writer.Unsigned(tile.samplesX, 4);
            writer.Unsigned(tile.samplesZ, 4);
            writer.Byte(source.layerCount);
            writer.Byte(source.HasHoles() ? 1 : 0);
            WriteCoordinates(writer, source.coordinates);
            for (const auto z : zs) {
                for (const auto x : xs)
                    WriteSample(writer, source, x, z);
            }
            tile.digest = ComputeSha256(std::as_bytes(std::span{tile.payload}));
            return tile;
        }

        struct CookBudget final {
            std::uint64_t bytes{};
            std::uint64_t work{};
        };

        Result<void> CookOneTile(const TerrainCanonicalSource &source, CookedTerrainTileSet &cooked, const CookedTerrainTileSet *previous,
                                 const TileBuildAddress &address, CookBudget &budget) {
            const auto nx = (address.endX - address.beginX + address.stride - 1) / address.stride + 1;
            const auto nz = (address.endZ - address.beginZ + address.stride - 1) / address.stride + 1;
            const auto visits = static_cast<std::uint64_t>(nx) * nz;
            if (visits > cooked.profile.maximumWorkItems - budget.work)
                return Result<void>::Failure(MakeError(TerrainTileCookErrors::LimitExceeded));
            const auto expectedBytes = 212ULL + source.coordinates.projectedCrs.size() + visits * (5ULL + 2ULL * source.layerCount);
            if (expectedBytes > cooked.profile.maximumPayloadBytes - budget.bytes)
                return Result<void>::Failure(MakeError(TerrainTileCookErrors::LimitExceeded));
            auto tile = BuildTile(source, cooked.sourceDigest, cooked.fingerprint, address);
            if (tile.payload.size() != expectedBytes)
                return Result<void>::Failure(MakeError(TerrainTileCookErrors::InvalidSource));
            budget.work += visits;
            budget.bytes += tile.payload.size();
            if (previous && previous->fingerprint == cooked.fingerprint) {
                const auto old = std::ranges::find_if(previous->tiles, [&](const auto &candidate) {
                    return candidate.id == tile.id;
                });
                if (old != previous->tiles.end() && old->digest == tile.digest && old->payload == tile.payload && old->seams == tile.seams)
                    tile = *old;
            }
            cooked.tiles.push_back(std::move(tile));
            return Result<void>::Success();
        }

        Result<void> CookLevel(const TerrainCanonicalSource &source, CookedTerrainTileSet &cooked, const CookedTerrainTileSet *previous,
                               const CancellationToken &cancellation, const std::uint8_t lod, CookBudget &budget) {
            const auto stride = std::uint32_t{1} << lod;
            const auto tileQuads = cooked.profile.interiorQuads * stride;
            const auto countX = (static_cast<std::uint64_t>(source.width - 2) / tileQuads) + 1;
            const auto countZ = (static_cast<std::uint64_t>(source.height - 2) / tileQuads) + 1;
            if (countX * countZ > cooked.profile.maximumTiles - cooked.tiles.size())
                return Result<void>::Failure(MakeError(TerrainTileCookErrors::LimitExceeded));
            const auto originX = WorldTileOrigin(source.coordinates.originX, source.coordinates.spacingX, tileQuads, countX);
            const auto originZ = WorldTileOrigin(source.coordinates.originZ, source.coordinates.spacingZ, tileQuads, countZ);
            if (!originX.has_value() || !originZ.has_value())
                return Result<void>::Failure(MakeError(TerrainTileCookErrors::InvalidSource));
            for (std::uint32_t z = 0; z < countZ; ++z) {
                for (std::uint32_t x = 0; x < countX; ++x) {
                    if (cancellation.IsCancellationRequested())
                        return Result<void>::Failure(MakeError(TerrainTileCookErrors::Cancelled));
                    const auto beginX = x * tileQuads;
                    const auto beginZ = z * tileQuads;
                    const TileBuildAddress address{.tile = {static_cast<std::int32_t>(static_cast<std::int64_t>(*originX) + x),
                                                            static_cast<std::int32_t>(static_cast<std::int64_t>(*originZ) + z), lod},
                                                   .stride = stride,
                                                   .beginX = beginX,
                                                   .endX = std::min(beginX + tileQuads, source.width - 1),
                                                   .beginZ = beginZ,
                                                   .endZ = std::min(beginZ + tileQuads, source.height - 1)};
                    const auto result = CookOneTile(source, cooked, previous, address, budget);
                    if (result.HasError())
                        return result;
                }
            }
            return Result<void>::Success();
        }
    }  // namespace

    namespace {
        /** @brief Admits exact tile shape and provenance before any payload hash or sample read. */
        bool AdmittedTilePayload(const TerrainCookedTile &tile, const CookedTerrainTileSet &cooked) {
            return tile.id.dataset == cooked.dataset && tile.id.tile.lod < TerrainDescriptorHardLimits::LodLevels && tile.samplesX >= 2 &&
                   tile.samplesZ >= 2 && tile.samplesX <= TerrainDescriptorHardLimits::TileInteriorQuads + 1 &&
                   tile.samplesZ <= TerrainDescriptorHardLimits::TileInteriorQuads + 1 && !tile.payload.empty() &&
                   tile.payload.size() <= TerrainDescriptorHardLimits::StagingBytes && PayloadMatchesProvenance(tile, cooked);
        }

        /** @brief Verifies admitted payload bytes, samples and seams with bounded cooperative cancellation. */
        bool VerifiedTilePayload(const TerrainCookedTile &tile, const CookedTerrainTileSet &cooked, const CancellationToken &cancellation) {
            if (!AdmittedTilePayload(tile, cooked))
                return false;
            const auto digest = Detail::HashPayload(tile.payload, cancellation);
            return digest.HasValue() && tile.digest == digest.Value() && ValidTileSamples(tile, cooked, cancellation) &&
                   ValidTileSeams(tile, cooked);
        }
    }  // namespace

    /** @copydoc VerifyCookedTerrainTiles */
    Result<void> VerifyCookedTerrainTiles(const CookedTerrainTileSet &cooked, const CancellationToken &cancellation) {
        const auto failure = [&cancellation] {
            return Result<void>::Failure(MakeError(cancellation.IsCancellationRequested() ? TerrainTileCookErrors::Cancelled
                                                                                          : TerrainTileCookErrors::CorruptPrevious));
        };
        if (cancellation.IsCancellationRequested())
            return failure();
        if (!cooked.dataset.IsValid() || !cooked.sourceAsset.IsValid() || !cooked.sourceRevision.IsValid() ||
            !Nonzero(cooked.sourceDigest) || !Nonzero(cooked.fingerprint) || cooked.tiles.empty() ||
            cooked.tiles.size() > TerrainDescriptorHardLimits::ActiveTerrainTiles)
            return failure();
        if (!ManifestLayoutValid(cooked, cancellation))
            return failure();
        for (std::size_t index = 0; index < cooked.tiles.size(); ++index) {
            const auto &tile = cooked.tiles[index];
            if (cancellation.IsCancellationRequested())
                return failure();
            if (!VerifiedTilePayload(tile, cooked, cancellation))
                return failure();
            if (index != 0) {
                const auto &prior = cooked.tiles[index - 1].id.tile;
                const auto &current = tile.id.tile;
                if (prior.lod > current.lod ||
                    (prior.lod == current.lod && (prior.z > current.z || (prior.z == current.z && prior.x >= current.x))))
                    return failure();
            }
        }
        if (cooked.manifestDigest != ManifestDigest(cooked) || cancellation.IsCancellationRequested())
            return failure();
        return Result<void>::Success();
    }

    /** @copydoc CookTerrainTiles */
    Result<CookedTerrainTileSet> CookTerrainTiles(const TerrainCanonicalSource &source, const TerrainTileCookProfile &profile,
                                                  const std::span<const TerrainTileCookDependency> dependencies,
                                                  const CancellationToken &cancellation, const CookedTerrainTileSet *previous) {
        if (cancellation.IsCancellationRequested())
            return Result<CookedTerrainTileSet>::Failure(MakeError(TerrainTileCookErrors::Cancelled));
        if (!ValidSource(source))
            return Result<CookedTerrainTileSet>::Failure(MakeError(TerrainTileCookErrors::InvalidSource));
        if (!ValidProfile(profile) || dependencies.size() > TerrainDescriptorHardLimits::WorkItems)
            return Result<CookedTerrainTileSet>::Failure(MakeError(TerrainTileCookErrors::InvalidProfile));
        const auto tier = GetTerrainTierProfile(profile.tier);
        if (const auto &tierLimits = tier.Value().limits; source.width > tierLimits.maximumSamplesPerAxis ||
                                                          source.height > tierLimits.maximumSamplesPerAxis ||
                                                          source.layerCount > tierLimits.maximumLayersPerTile)
            return Result<CookedTerrainTileSet>::Failure(MakeError(TerrainTileCookErrors::LimitExceeded));
        std::vector<TerrainTileCookDependency> sortedDependencies{dependencies.begin(), dependencies.end()};
        std::ranges::sort(sortedDependencies, [](const auto &left, const auto &right) {
            return left.asset < right.asset;
        });
        for (std::size_t index = 0; index < sortedDependencies.size(); ++index) {
            const auto &entry = sortedDependencies[index];
            if (!entry.asset.IsValid() || !Nonzero(entry.artifactDigest) ||
                (index != 0 && sortedDependencies[index - 1].asset == entry.asset))
                return Result<CookedTerrainTileSet>::Failure(MakeError(TerrainTileCookErrors::InvalidProfile));
        }
        if (previous && VerifyCookedTerrainTiles(*previous).HasError())
            return Result<CookedTerrainTileSet>::Failure(MakeError(TerrainTileCookErrors::CorruptPrevious));

        CookedTerrainTileSet cooked;
        cooked.dataset = source.dataset;
        cooked.sourceAsset = source.sourceAsset;
        cooked.sourceRevision = source.revision;
        cooked.sourceWidth = source.width;
        cooked.sourceHeight = source.height;
        cooked.layerCount = source.layerCount;
        cooked.hasHoles = source.HasHoles();
        cooked.coordinates = source.coordinates;
        cooked.profile = profile;
        cooked.sourceDigest = SourceDigest(source);
        cooked.fingerprint = Fingerprint(source, cooked.sourceDigest, profile, sortedDependencies);
        CookBudget budget;
        for (std::uint8_t lod = 0; lod < profile.lodLevels; ++lod) {
            const auto result = CookLevel(source, cooked, previous, cancellation, lod, budget);
            if (result.HasError())
                return Result<CookedTerrainTileSet>::Failure(result.ErrorValue());
        }
        if (cancellation.IsCancellationRequested())
            return Result<CookedTerrainTileSet>::Failure(MakeError(TerrainTileCookErrors::Cancelled));
        cooked.manifestDigest = ManifestDigest(cooked);
        return Result<CookedTerrainTileSet>::Success(std::move(cooked));
    }
}  // namespace Horo::Terrain
