#include "Horo/Terrain/TerrainTileCook.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <string_view>

namespace Horo::Terrain {
    namespace {
        constexpr std::string_view TileMagic = "HTIL";
        constexpr std::string_view ManifestMagic = "HTMF";

        bool Nonzero(const Sha256Digest &digest) {
            return std::any_of(digest.bytes.begin(), digest.bytes.end(), [](const auto byte) {
                return byte != 0;
            });
        }

        class CanonicalWriter final {
        public:
            explicit CanonicalWriter(std::vector<std::uint8_t> *bytes, Sha256Builder *hash = nullptr) : bytes_(bytes), hash_(hash) {}

            void Byte(const std::uint8_t value) {
                if (bytes_)
                    bytes_->push_back(value);
                if (hash_) {
                    pending_[pendingSize_++] = static_cast<std::byte>(value);
                    if (pendingSize_ == pending_.size())
                        Flush();
                }
            }

            void Flush() {
                if (pendingSize_ == 0)
                    return;
                valid_ = hash_->Update(std::span{pending_.data(), pendingSize_}) && valid_;
                pendingSize_ = 0;
            }

            void Unsigned(std::uint64_t value, const std::uint8_t count) {
                for (std::uint8_t index = 0; index < count; ++index) {
                    Byte(static_cast<std::uint8_t>(value));
                    value >>= 8U;
                }
            }

            template <std::size_t N> void Bytes(const std::array<std::uint8_t, N> &value) {
                for (const auto byte : value)
                    Byte(byte);
            }

            void Text(const std::string_view value) {
                Unsigned(value.size(), 2);
                for (const char byte : value)
                    Byte(static_cast<std::uint8_t>(byte));
            }

            void Float(const float value) {
                Unsigned(std::bit_cast<std::uint32_t>(value == 0.0F ? 0.0F : value), 4);
            }

            void Double(const double value) {
                Unsigned(std::bit_cast<std::uint64_t>(value == 0.0 ? 0.0 : value), 8);
            }

            [[nodiscard]] bool Valid() const noexcept {
                return valid_;
            }

        private:
            std::vector<std::uint8_t> *bytes_{};
            Sha256Builder *hash_{};
            std::array<std::byte, 4096> pending_{};
            std::size_t pendingSize_{};
            bool valid_{true};
        };

        void WriteCoordinates(CanonicalWriter &writer, const TerrainSourceCoordinates &coordinates) {
            writer.Byte(static_cast<std::uint8_t>(coordinates.space));
            writer.Text(coordinates.projectedCrs);
            writer.Double(coordinates.originX);
            writer.Double(coordinates.originZ);
            writer.Double(coordinates.spacingX);
            writer.Double(coordinates.spacingZ);
            writer.Double(coordinates.heightScale);
            writer.Double(coordinates.heightOffset);
            writer.Double(coordinates.maximumPrecisionError);
        }

        void WriteProfile(CanonicalWriter &writer, const TerrainTileCookProfile &profile) {
            writer.Unsigned(profile.interiorQuads, 4);
            writer.Byte(profile.lodLevels);
            writer.Byte(static_cast<std::uint8_t>(profile.tier));
            writer.Bytes(profile.targetDigest.bytes);
            writer.Bytes(profile.toolchainDigest.bytes);
            writer.Unsigned(profile.maximumTiles, 4);
            writer.Unsigned(profile.maximumPayloadBytes, 8);
            writer.Unsigned(profile.maximumWorkItems, 8);
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
                quantized > static_cast<double>(std::numeric_limits<std::int32_t>::max()) - tileCount)
                return std::nullopt;
            return static_cast<std::int32_t>(quantized);
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

        bool ValidProfile(const TerrainTileCookProfile &profile) {
            const auto tier = GetTerrainTierProfile(profile.tier);
            if (tier.HasError())
                return false;
            const auto &limits = tier.Value().limits;
            return std::has_single_bit(profile.interiorQuads) && profile.interiorQuads <= limits.maximumTileInteriorQuads &&
                   profile.lodLevels != 0 && profile.lodLevels <= limits.maximumLodLevels && Nonzero(profile.targetDigest) &&
                   Nonzero(profile.toolchainDigest) && profile.maximumTiles != 0 &&
                   profile.maximumTiles <= limits.maximumActiveTerrainTiles && profile.maximumPayloadBytes != 0 &&
                   profile.maximumPayloadBytes <= limits.maximumStagingBytes && profile.maximumWorkItems != 0 &&
                   profile.maximumWorkItems <= limits.maximumWorkItems;
        }

        Sha256Digest Fingerprint(const TerrainCanonicalSource &source, const Sha256Digest &sourceDigest,
                                 const TerrainTileCookProfile &profile, const std::vector<TerrainTileCookDependency> &dependencies) {
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
            writer.Text(ManifestMagic);
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

        bool PayloadMatchesProvenance(const TerrainCookedTile &tile, const CookedTerrainTileSet &cooked) {
            // Fixed v1 prefix: sized magic, schema, tile ID, source asset/revision and source/cook digests.
            constexpr std::size_t PrefixBytes = 123;
            if (tile.payload.size() < PrefixBytes || tile.payload[0] != 4 || tile.payload[1] != 0 ||
                !std::equal(TileMagic.begin(), TileMagic.end(), tile.payload.begin() + 2) ||
                tile.payload[6] != CurrentTerrainTileCookSchema || tile.payload[7] != 0 || tile.payload[8] != 0 || tile.payload[9] != 0)
                return false;
            const auto tileId = SerializeTerrainTileId(tile.id);
            if (!std::equal(tileId.begin(), tileId.end(), tile.payload.begin() + 10) ||
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

        bool ValidTileSamples(const TerrainCookedTile &tile, const CookedTerrainTileSet &cooked) {
            const auto sampleBytes = 5U + 2U * cooked.layerCount;
            const auto sampleCount = static_cast<std::size_t>(tile.samplesX) * tile.samplesZ;
            const auto start = tile.payload.size() - sampleCount * sampleBytes;
            for (std::size_t sample = 0; sample < sampleCount; ++sample) {
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
                const auto x = alongZ ? (highEdge ? tile.samplesX - 1 : 0) : sample;
                const auto z = alongZ ? sample : (highEdge ? tile.samplesZ - 1 : 0);
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
            const auto expectedBytes =
                153ULL + encodedCoordinates.size() + static_cast<std::uint64_t>(nx) * nz * (5ULL + 2ULL * cooked.layerCount);
            if (tile.samplesX != nx || tile.samplesZ != nz || tile.payload.size() != expectedBytes)
                return false;
            return ReadU32(tile.payload, 123) == beginX && ReadU32(tile.payload, 127) == endX && ReadU32(tile.payload, 131) == beginZ &&
                   ReadU32(tile.payload, 135) == endZ && ReadU32(tile.payload, 139) == stride && ReadU32(tile.payload, 143) == nx &&
                   ReadU32(tile.payload, 147) == nz && tile.payload[151] == cooked.layerCount &&
                   tile.payload[152] == (cooked.hasHoles ? 1 : 0) &&
                   std::equal(encodedCoordinates.begin(), encodedCoordinates.end(), tile.payload.begin() + 153);
        }

        bool ManifestLayoutValid(const CookedTerrainTileSet &cooked) {
            if (!ValidProfile(cooked.profile) || !ValidCoordinates(cooked.coordinates) || cooked.sourceWidth < 2 || cooked.sourceHeight < 2)
                return false;
            const auto tier = GetTerrainTierProfile(cooked.profile.tier);
            const auto &limits = tier.Value().limits;
            if (cooked.sourceWidth > limits.maximumSamplesPerAxis || cooked.sourceHeight > limits.maximumSamplesPerAxis ||
                cooked.layerCount > limits.maximumLayersPerTile)
                return false;
            std::size_t cursor{};
            std::uint64_t totalBytes{};
            std::uint64_t totalWork{};
            for (std::uint8_t lod = 0; lod < cooked.profile.lodLevels; ++lod) {
                const auto stride = std::uint32_t{1} << lod;
                const auto tileQuads = cooked.profile.interiorQuads * stride;
                const auto countX = (static_cast<std::uint64_t>(cooked.sourceWidth - 2) / tileQuads) + 1;
                const auto countZ = (static_cast<std::uint64_t>(cooked.sourceHeight - 2) / tileQuads) + 1;
                if (countX * countZ > cooked.tiles.size() - cursor)
                    return false;
                const auto originX = WorldTileOrigin(cooked.coordinates.originX, cooked.coordinates.spacingX, tileQuads, countX);
                const auto originZ = WorldTileOrigin(cooked.coordinates.originZ, cooked.coordinates.spacingZ, tileQuads, countZ);
                if (!originX || !originZ)
                    return false;
                const auto lodStart = cursor;
                for (std::uint32_t z = 0; z < countZ; ++z) {
                    for (std::uint32_t x = 0; x < countX; ++x) {
                        const auto &tile = cooked.tiles[cursor];
                        const auto address = TerrainTileCoordinate{static_cast<std::int32_t>(static_cast<std::int64_t>(*originX) + x),
                                                                   static_cast<std::int32_t>(static_cast<std::int64_t>(*originZ) + z), lod};
                        const auto beginX = x * tileQuads;
                        const auto beginZ = z * tileQuads;
                        const auto endX = std::min(beginX + tileQuads, cooked.sourceWidth - 1);
                        const auto endZ = std::min(beginZ + tileQuads, cooked.sourceHeight - 1);
                        if (tile.id.dataset != cooked.dataset || tile.id.tile != address ||
                            !TileMatchesLayout(tile, cooked, beginX, endX, beginZ, endZ, stride))
                            return false;
                        if (x != 0 && cooked.tiles[cursor - 1].seams[1] != tile.seams[0])
                            return false;
                        if (z != 0 && cooked.tiles[cursor - countX].seams[3] != tile.seams[2])
                            return false;
                        if (tile.payload.size() > cooked.profile.maximumPayloadBytes - totalBytes)
                            return false;
                        totalBytes += tile.payload.size();
                        const auto visits = static_cast<std::uint64_t>(tile.samplesX) * tile.samplesZ;
                        if (visits > cooked.profile.maximumWorkItems - totalWork)
                            return false;
                        totalWork += visits;
                        ++cursor;
                    }
                }
                if (cursor - lodStart != countX * countZ)
                    return false;
            }
            return cursor == cooked.tiles.size() && cursor <= cooked.profile.maximumTiles;
        }

        TerrainCookedTile BuildTile(const TerrainCanonicalSource &source, const Sha256Digest &sourceDigest, const Sha256Digest &fingerprint,
                                    const std::uint32_t stride, const std::uint32_t beginX, const std::uint32_t endX,
                                    const std::uint32_t beginZ, const std::uint32_t endZ, const std::int32_t tileX,
                                    const std::int32_t tileZ, const std::uint8_t lod) {
            TerrainCookedTile tile;
            tile.id = {.dataset = source.dataset, .tile = {.x = tileX, .z = tileZ, .lod = lod}};
            const auto xs = SamplePositions(beginX, endX, stride);
            const auto zs = SamplePositions(beginZ, endZ, stride);
            tile.samplesX = static_cast<std::uint32_t>(xs.size());
            tile.samplesZ = static_cast<std::uint32_t>(zs.size());
            tile.seams = {EdgeDigest(source, xs, zs, stride, true, false), EdgeDigest(source, xs, zs, stride, true, true),
                          EdgeDigest(source, xs, zs, stride, false, false), EdgeDigest(source, xs, zs, stride, false, true)};
            CanonicalWriter writer{&tile.payload};
            writer.Text(TileMagic);
            writer.Unsigned(CurrentTerrainTileCookSchema, 4);
            writer.Bytes(SerializeTerrainTileId(tile.id));
            writer.Bytes(source.sourceAsset.Bytes());
            writer.Unsigned(source.revision.Value(), 8);
            writer.Bytes(sourceDigest.bytes);
            writer.Bytes(fingerprint.bytes);
            writer.Unsigned(beginX, 4);
            writer.Unsigned(endX, 4);
            writer.Unsigned(beginZ, 4);
            writer.Unsigned(endZ, 4);
            writer.Unsigned(stride, 4);
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
    }  // namespace

    /** @copydoc VerifyCookedTerrainTiles */
    Result<void> VerifyCookedTerrainTiles(const CookedTerrainTileSet &cooked) {
        if (!cooked.dataset.IsValid() || !cooked.sourceAsset.IsValid() || !cooked.sourceRevision.IsValid() ||
            !Nonzero(cooked.sourceDigest) || !Nonzero(cooked.fingerprint) || cooked.tiles.empty() ||
            cooked.tiles.size() > TerrainDescriptorHardLimits::ActiveTerrainTiles)
            return Result<void>::Failure(MakeError(TerrainTileCookErrors::CorruptPrevious));
        if (!ManifestLayoutValid(cooked))
            return Result<void>::Failure(MakeError(TerrainTileCookErrors::CorruptPrevious));
        for (std::size_t index = 0; index < cooked.tiles.size(); ++index) {
            const auto &tile = cooked.tiles[index];
            if (tile.id.dataset != cooked.dataset || tile.id.tile.lod >= TerrainDescriptorHardLimits::LodLevels || tile.samplesX < 2 ||
                tile.samplesZ < 2 || tile.samplesX > TerrainDescriptorHardLimits::TileInteriorQuads + 1 ||
                tile.samplesZ > TerrainDescriptorHardLimits::TileInteriorQuads + 1 || tile.payload.empty() ||
                tile.payload.size() > TerrainDescriptorHardLimits::StagingBytes || !PayloadMatchesProvenance(tile, cooked) ||
                tile.digest != ComputeSha256(std::as_bytes(std::span{tile.payload})) || !ValidTileSamples(tile, cooked) ||
                !ValidTileSeams(tile, cooked))
                return Result<void>::Failure(MakeError(TerrainTileCookErrors::CorruptPrevious));
            if (index != 0) {
                const auto &prior = cooked.tiles[index - 1].id.tile;
                const auto &current = tile.id.tile;
                if (prior.lod > current.lod ||
                    (prior.lod == current.lod && (prior.z > current.z || (prior.z == current.z && prior.x >= current.x))))
                    return Result<void>::Failure(MakeError(TerrainTileCookErrors::CorruptPrevious));
            }
        }
        if (cooked.manifestDigest != ManifestDigest(cooked))
            return Result<void>::Failure(MakeError(TerrainTileCookErrors::CorruptPrevious));
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
        const auto &tierLimits = tier.Value().limits;
        if (source.width > tierLimits.maximumSamplesPerAxis || source.height > tierLimits.maximumSamplesPerAxis ||
            source.layerCount > tierLimits.maximumLayersPerTile)
            return Result<CookedTerrainTileSet>::Failure(MakeError(TerrainTileCookErrors::LimitExceeded));
        std::vector<TerrainTileCookDependency> sortedDependencies{dependencies.begin(), dependencies.end()};
        std::sort(sortedDependencies.begin(), sortedDependencies.end(), [](const auto &left, const auto &right) {
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
        std::uint64_t cumulativeBytes{};
        std::uint64_t cumulativeWork{};
        for (std::uint8_t lod = 0; lod < profile.lodLevels; ++lod) {
            const auto stride = std::uint32_t{1} << lod;
            const auto tileQuads = profile.interiorQuads * stride;
            const auto tileCountX = (static_cast<std::uint64_t>(source.width - 2) / tileQuads) + 1;
            const auto tileCountZ = (static_cast<std::uint64_t>(source.height - 2) / tileQuads) + 1;
            if (tileCountX * tileCountZ > profile.maximumTiles - cooked.tiles.size())
                return Result<CookedTerrainTileSet>::Failure(MakeError(TerrainTileCookErrors::LimitExceeded));
            const auto originX = WorldTileOrigin(source.coordinates.originX, source.coordinates.spacingX, tileQuads, tileCountX);
            const auto originZ = WorldTileOrigin(source.coordinates.originZ, source.coordinates.spacingZ, tileQuads, tileCountZ);
            if (!originX || !originZ)
                return Result<CookedTerrainTileSet>::Failure(MakeError(TerrainTileCookErrors::InvalidSource));
            for (std::uint32_t z = 0; z < tileCountZ; ++z) {
                for (std::uint32_t x = 0; x < tileCountX; ++x) {
                    if (cancellation.IsCancellationRequested())
                        return Result<CookedTerrainTileSet>::Failure(MakeError(TerrainTileCookErrors::Cancelled));
                    const auto beginX = x * tileQuads;
                    const auto beginZ = z * tileQuads;
                    const auto endX = std::min(beginX + tileQuads, source.width - 1);
                    const auto endZ = std::min(beginZ + tileQuads, source.height - 1);
                    const auto nx = (endX - beginX + stride - 1) / stride + 1;
                    const auto nz = (endZ - beginZ + stride - 1) / stride + 1;
                    const auto visits = static_cast<std::uint64_t>(nx) * nz;
                    if (visits > profile.maximumWorkItems - cumulativeWork)
                        return Result<CookedTerrainTileSet>::Failure(MakeError(TerrainTileCookErrors::LimitExceeded));
                    cumulativeWork += visits;
                    const auto expectedBytes = 212ULL + source.coordinates.projectedCrs.size() + visits * (5ULL + 2ULL * source.layerCount);
                    if (expectedBytes > profile.maximumPayloadBytes - cumulativeBytes)
                        return Result<CookedTerrainTileSet>::Failure(MakeError(TerrainTileCookErrors::LimitExceeded));
                    auto tile = BuildTile(source, cooked.sourceDigest, cooked.fingerprint, stride, beginX, endX, beginZ, endZ,
                                          static_cast<std::int32_t>(static_cast<std::int64_t>(*originX) + x),
                                          static_cast<std::int32_t>(static_cast<std::int64_t>(*originZ) + z), lod);
                    if (tile.payload.size() != expectedBytes)
                        return Result<CookedTerrainTileSet>::Failure(MakeError(TerrainTileCookErrors::InvalidSource));
                    cumulativeBytes += tile.payload.size();
                    if (previous && previous->fingerprint == cooked.fingerprint) {
                        const auto old = std::find_if(previous->tiles.begin(), previous->tiles.end(), [&](const auto &candidate) {
                            return candidate.id == tile.id;
                        });
                        if (old != previous->tiles.end() && old->digest == tile.digest && old->payload == tile.payload &&
                            old->seams == tile.seams)
                            tile = *old;
                    }
                    cooked.tiles.push_back(std::move(tile));
                }
            }
        }
        if (cancellation.IsCancellationRequested())
            return Result<CookedTerrainTileSet>::Failure(MakeError(TerrainTileCookErrors::Cancelled));
        cooked.manifestDigest = ManifestDigest(cooked);
        return Result<CookedTerrainTileSet>::Success(std::move(cooked));
    }
}  // namespace Horo::Terrain
