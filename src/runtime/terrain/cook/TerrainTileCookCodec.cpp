#include "TerrainTileCookCodec.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Terrain::Detail {
    /** @copydoc HashPayload */
    Result<Sha256Digest> HashPayload(const std::span<const std::uint8_t> bytes, const CancellationToken &cancellation) {
        Sha256Builder hash;
        for (std::size_t offset = 0; offset < bytes.size();) {
            if (cancellation.IsCancellationRequested())
                return Result<Sha256Digest>::Failure(MakeError(TerrainTileCookErrors::Cancelled));
            const auto count = std::min<std::size_t>(4'096, bytes.size() - offset);
            static_cast<void>(hash.Update(std::as_bytes(bytes.subspan(offset, count))));
            offset += count;
        }
        if (cancellation.IsCancellationRequested())
            return Result<Sha256Digest>::Failure(MakeError(TerrainTileCookErrors::Cancelled));
        return Result<Sha256Digest>::Success(hash.Finalize());
    }

    bool Nonzero(const Sha256Digest &digest) {
        return std::ranges::any_of(digest.bytes, [](const auto byte) {
            return byte != 0;
        });
    }

    void WriteSample(CanonicalWriter &writer, const TerrainCanonicalSource &source, const std::size_t x, const std::size_t z) {
        const auto index = z * source.width + x;
        writer.Float(source.heightsMeters[index]);
        for (std::uint8_t layer = 0; layer < source.layerCount; ++layer)
            writer.Unsigned(source.weights[index * source.layerCount + layer], 2);
        writer.Byte(source.holes.empty() ? 0 : source.holes[index]);
    }

    Sha256Digest SourceDigest(const TerrainCanonicalSource &source) {
        Sha256Builder hash;
        CanonicalWriter writer{nullptr, &hash};
        writer.Text("horo.terrain.source.v1");
        writer.Bytes(source.dataset.Bytes());
        writer.Bytes(source.sourceAsset.Bytes());
        writer.Unsigned(source.revision.Value(), 8);
        writer.Unsigned(source.capability.Value(), 8);
        writer.Unsigned(source.width, 4);
        writer.Unsigned(source.height, 4);
        writer.Byte(source.layerCount);
        writer.Byte(source.HasHoles() ? 1 : 0);
        WriteCoordinates(writer, source.coordinates);
        for (std::size_t z = 0; z < source.height; ++z) {
            for (std::size_t x = 0; x < source.width; ++x)
                WriteSample(writer, source, x, z);
        }
        writer.Flush();
        return hash.Finalize();
    }

    std::vector<std::uint32_t> SamplePositions(const std::uint32_t begin, const std::uint32_t end, const std::uint32_t stride) {
        std::vector<std::uint32_t> positions;
        for (std::uint32_t value = begin; value < end; value += stride)
            positions.push_back(value);
        positions.push_back(end);
        return positions;
    }

    std::optional<std::int32_t> WorldTileOrigin(const double origin, const double spacing, const std::uint32_t tileQuads,
                                                const std::uint64_t tileCount) {
        const auto quantized = std::floor(origin / (spacing * tileQuads));
        if (!std::isfinite(quantized) || quantized < std::numeric_limits<std::int32_t>::min() ||
            quantized > std::numeric_limits<std::int32_t>::max())
            return std::nullopt;
        const auto firstTile = static_cast<std::int32_t>(quantized);
        if (const auto remaining =
                static_cast<std::uint64_t>(static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) - firstTile);
            tileCount == 0 || tileCount - 1 > remaining)
            return std::nullopt;
        return firstTile;
    }

    Sha256Digest EdgeDigest(const TerrainCanonicalSource &source, const std::vector<std::uint32_t> &xs,
                            const std::vector<std::uint32_t> &zs, const std::uint32_t stride, const bool alongZ, const bool highEdge) {
        Sha256Builder hash;
        CanonicalWriter writer{nullptr, &hash};
        writer.Text("horo.terrain.seam.v1");
        writer.Unsigned(stride, 4);
        writer.Byte(source.layerCount);
        writer.Byte(alongZ ? 1 : 0);
        writer.Unsigned(alongZ ? zs.size() : xs.size(), 4);
        if (alongZ) {
            for (const auto z : zs)
                WriteSample(writer, source, highEdge ? xs.back() : xs.front(), z);
        } else {
            for (const auto x : xs)
                WriteSample(writer, source, x, highEdge ? zs.back() : zs.front());
        }
        writer.Flush();
        return hash.Finalize();
    }

    bool ValidCoordinates(const TerrainSourceCoordinates &coordinates) {
        if (coordinates.space >= TerrainCoordinateSpace::GeographicDegrees || !std::isfinite(coordinates.originX) ||
            !std::isfinite(coordinates.originZ) || !std::isfinite(coordinates.spacingX) || !std::isfinite(coordinates.spacingZ) ||
            !std::isfinite(coordinates.heightScale) || !std::isfinite(coordinates.heightOffset) ||
            !std::isfinite(coordinates.maximumPrecisionError) || coordinates.spacingX <= 0 || coordinates.spacingZ <= 0 ||
            coordinates.heightScale <= 0 || coordinates.maximumPrecisionError < 0 ||
            coordinates.projectedCrs.size() > std::numeric_limits<std::uint16_t>::max())
            return false;
        if (coordinates.space == TerrainCoordinateSpace::LocalMeters)
            return coordinates.projectedCrs.empty();
        const auto &crs = coordinates.projectedCrs;
        return crs.size() > 5 && crs.starts_with("EPSG:") && std::all_of(crs.begin() + 5, crs.end(), [](const char c) {
            return c >= '0' && c <= '9';
        });
    }

    bool ValidProfile(const TerrainTileCookProfile &profile) {
        const auto tier = GetTerrainTierProfile(profile.tier);
        if (tier.HasError())
            return false;
        const auto &limits = tier.Value().limits;
        return std::has_single_bit(profile.interiorQuads) && profile.interiorQuads <= limits.maximumTileInteriorQuads &&
               profile.lodLevels != 0 && profile.lodLevels <= limits.maximumLodLevels && Nonzero(profile.targetDigest) &&
               Nonzero(profile.toolchainDigest) && profile.maximumTiles != 0 && profile.maximumTiles <= limits.maximumActiveTerrainTiles &&
               profile.maximumPayloadBytes != 0 && profile.maximumPayloadBytes <= limits.maximumStagingBytes &&
               profile.maximumWorkItems != 0 && profile.maximumWorkItems <= limits.maximumWorkItems;
    }

    Sha256Digest Fingerprint(const TerrainCanonicalSource &source, const Sha256Digest &sourceDigest, const TerrainTileCookProfile &profile,
                             const std::vector<TerrainTileCookDependency> &dependencies) {
        Sha256Builder hash;
        CanonicalWriter writer{nullptr, &hash};
        writer.Text("horo.terrain.tile-cook.v1");
        writer.Unsigned(CurrentTerrainTileCookSchema, 4);
        writer.Bytes(source.dataset.Bytes());
        writer.Bytes(source.sourceAsset.Bytes());
        writer.Unsigned(source.revision.Value(), 8);
        writer.Bytes(sourceDigest.bytes);
        WriteProfile(writer, profile);
        writer.Unsigned(dependencies.size(), 4);
        for (const auto &dependency : dependencies) {
            writer.Bytes(dependency.asset.Bytes());
            writer.Bytes(dependency.artifactDigest.bytes);
        }
        writer.Flush();
        return hash.Finalize();
    }

    Sha256Digest ManifestDigest(const CookedTerrainTileSet &cooked) {
        Sha256Builder hash;
        CanonicalWriter writer{nullptr, &hash};
        writer.Text("HTMF");
        writer.Unsigned(CurrentTerrainTileCookSchema, 4);
        writer.Bytes(cooked.dataset.Bytes());
        writer.Bytes(cooked.sourceAsset.Bytes());
        writer.Unsigned(cooked.sourceRevision.Value(), 8);
        writer.Unsigned(cooked.sourceWidth, 4);
        writer.Unsigned(cooked.sourceHeight, 4);
        writer.Byte(cooked.layerCount);
        writer.Byte(cooked.hasHoles ? 1 : 0);
        WriteCoordinates(writer, cooked.coordinates);
        WriteProfile(writer, cooked.profile);
        writer.Bytes(cooked.sourceDigest.bytes);
        writer.Bytes(cooked.fingerprint.bytes);
        writer.Unsigned(cooked.tiles.size(), 4);
        for (const auto &tile : cooked.tiles) {
            writer.Bytes(SerializeTerrainTileId(tile.id));
            writer.Unsigned(tile.samplesX, 4);
            writer.Unsigned(tile.samplesZ, 4);
            writer.Unsigned(tile.payload.size(), 8);
            for (const auto &seam : tile.seams)
                writer.Bytes(seam.bytes);
            writer.Bytes(tile.digest.bytes);
        }
        writer.Flush();
        return hash.Finalize();
    }
}  // namespace Horo::Terrain::Detail
