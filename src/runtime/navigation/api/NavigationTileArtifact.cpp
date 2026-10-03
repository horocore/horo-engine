#include "NavigationTileArtifactInternal.h"

#include <array>
#include <cmath>

namespace Horo::Navigation {
    namespace {
        using namespace TileArtifactInternal;
        constexpr std::uint32_t TileMagic = 0x31544e48;  // HNT1

        /** @brief Rejects reserved identities before they enter an owned artifact. */
        template <typename T> [[nodiscard]] T ReadIdentity(Reader &reader) {
            auto id = T::Create(reader.Integer(8));
            if (id.HasError())
                throw std::invalid_argument("Invalid navigation identity");
            return std::move(id).Value();
        }

        /** @brief Emits the exact tile/profile grid and resolved profile geometry. */
        void EncodeDescriptor(Writer &writer, const NavigationPreparedTile &input) {
            writer.Integer(TileMagic);
            writer.Integer(input.tile.key.profile.Value(), 8);
            writer.Integer(input.tile.key.surface.Value(), 8);
            writer.Integer(static_cast<std::uint32_t>(input.tile.key.tile.x));
            writer.Integer(static_cast<std::uint32_t>(input.tile.key.tile.z));
            writer.Integer(input.tile.key.tile.layer, 2);
            writer.Vector(input.tile.bounds.minimum);
            writer.Vector(input.tile.bounds.maximum);
            writer.Float(input.tile.tileSizeMeters);
            const auto &g = input.geometry;
            for (const auto value : {g.radiusMeters, g.heightMeters, g.maxSlopeDegrees, g.stepHeightMeters, g.cellSizeMeters,
                                     g.cellHeightMeters, g.minimumRegionSizeMeters})
                writer.Float(value);
            writer.Integer(input.borderSizeCells);
            writer.Digest(input.dependencyKey);
        }

        /** @brief Parses fixed metadata before reading any table count. */
        [[nodiscard]] NavigationPreparedTile DecodeDescriptor(Reader &reader) {
            if (reader.Integer() != TileMagic)
                throw std::invalid_argument("Unsupported navigation tile format");
            NavigationPreparedTile input;
            input.tile.key.profile = ReadIdentity<NavigationAgentProfileId>(reader);
            input.tile.key.surface = ReadIdentity<SurfaceId>(reader);
            input.tile.key.tile.x = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(reader.Integer()));
            input.tile.key.tile.z = std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(reader.Integer()));
            input.tile.key.tile.layer = static_cast<std::uint16_t>(reader.Integer(2));
            input.tile.bounds.minimum = reader.Vector();
            input.tile.bounds.maximum = reader.Vector();
            input.tile.tileSizeMeters = reader.Float();
            auto &g = input.geometry;
            g.radiusMeters = reader.Float();
            g.heightMeters = reader.Float();
            g.maxSlopeDegrees = reader.Float();
            g.stepHeightMeters = reader.Float();
            g.cellSizeMeters = reader.Float();
            g.cellHeightMeters = reader.Float();
            g.minimumRegionSizeMeters = reader.Float();
            input.borderSizeCells = static_cast<std::uint32_t>(reader.Integer());
            input.dependencyKey = reader.Digest();
            return input;
        }

        /** @brief Checks exact descriptor placement and resolved build geometry for both fresh and decoded tiles. */
        [[nodiscard]] Result<void> ValidateDescriptor(const NavigationPreparedTile &input, const NavigationTileBuildResult &result) {
            if (!input.tile.key.surface.IsValid() || !input.tile.key.profile.IsValid() || !Present(input.dependencyKey) ||
                result.key != input.tile.key.tile || result.bounds.minimum != input.tile.bounds.minimum ||
                result.bounds.maximum != input.tile.bounds.maximum || !input.tile.bounds.IsValid() ||
                !std::isfinite(input.tile.tileSizeMeters) || input.tile.tileSizeMeters <= 0 ||
                ValidateNavigationAgentBuildGeometry(input.geometry).HasError())
                return Failure<void>(NavigationErrors::BakeInputInvalid);
            const double cells = static_cast<double>(input.tile.tileSizeMeters) / input.geometry.cellSizeMeters;
            const double requiredBorder = std::ceil(static_cast<double>(input.geometry.radiusMeters) / input.geometry.cellSizeMeters) + 3;
            const float x = static_cast<float>(input.tile.key.tile.x) * input.tile.tileSizeMeters;
            if (const float z = static_cast<float>(input.tile.key.tile.z) * input.tile.tileSizeMeters;
                !std::isfinite(cells) || cells < 1 || cells > 65'000 || std::abs(cells - std::round(cells)) > 1.0e-4 ||
                requiredBorder >= 255 || input.borderSizeCells != requiredBorder || input.tile.bounds.minimum.x != x ||
                input.tile.bounds.minimum.z != z || input.tile.bounds.maximum.x != x + input.tile.tileSizeMeters ||
                input.tile.bounds.maximum.z != z + input.tile.tileSizeMeters || input.tile.bounds.maximum.y <= input.tile.bounds.minimum.y)
                return Failure<void>(NavigationErrors::BakeInputInvalid);
            return Result<void>::Success();
        }

        /** @brief Applies the neutral topology validator after portable encoding established exact byte size. */
        [[nodiscard]] Result<void> ValidateBuiltTopology(const NavigationPreparedTile &input, const NavigationTileBuildResult &result,
                                                         const std::size_t bytes) {
            const Sha256Digest digest = input.dependencyKey;
            const NavMeshTileDescriptor tile{.key = result.key,
                                             .bounds = result.bounds,
                                             .vertices = {0, static_cast<std::uint32_t>(result.vertices.size())},
                                             .polygons = {0, static_cast<std::uint32_t>(result.polygons.size())},
                                             .polygonVertexIndices = {0, static_cast<std::uint32_t>(result.polygonVertexIndices.size())},
                                             .polygonAdjacencies = {0, static_cast<std::uint32_t>(result.polygonAdjacencies.size())},
                                             .offMeshLinks = {0, static_cast<std::uint32_t>(result.offMeshLinks.size())},
                                             .provenance = {0, static_cast<std::uint32_t>(result.provenance.size())},
                                             .payloadDigest = digest};
            const NavMeshArtifactView view{.header = {.coordinateFrame = {.tileSizeMeters = input.tile.tileSizeMeters},
                                                      .profile = {.id = input.tile.key.profile,
                                                                  .buildGeometry = input.geometry,
                                                                  .contentDigest = digest},
                                                      .tileCount = 1,
                                                      .vertexCount = tile.vertices.count,
                                                      .polygonCount = tile.polygons.count,
                                                      .polygonVertexIndexCount = tile.polygonVertexIndices.count,
                                                      .polygonAdjacencyCount = tile.polygonAdjacencies.count,
                                                      .offMeshLinkCount = tile.offMeshLinks.count,
                                                      .provenanceCount = tile.provenance.count,
                                                      .portableEncodedBytes = bytes,
                                                      .portableDecodedBytes = bytes,
                                                      .payloadDigest = digest},
                                           .observedPayloadDigest = digest,
                                           .tiles = std::span{&tile, 1},
                                           .observedTilePayloadDigests = std::span{&digest, 1},
                                           .tables = {.vertices = result.vertices,
                                                      .polygons = result.polygons,
                                                      .polygonVertexIndices = result.polygonVertexIndices,
                                                      .polygonAdjacencies = result.polygonAdjacencies,
                                                      .offMeshLinks = result.offMeshLinks,
                                                      .provenance = result.provenance}};
            return ValidateNavMeshTile(view, 0);
        }

        /** @brief Validates empty state invariants or the same neutral contract for fresh and cached built topology. */
        [[nodiscard]] Result<void> Validate(const NavigationPreparedTile &input, const NavigationTileBuildResult &result,
                                            const std::size_t bytes) {
            if (const auto descriptor = ValidateDescriptor(input, result); descriptor.HasError())
                return descriptor;
            if (result.IsEmpty()) {
                if (!result.vertices.empty() || !result.polygons.empty() || !result.polygonVertexIndices.empty() ||
                    !result.polygonAdjacencies.empty() || !result.provenance.empty() || !result.offMeshLinks.empty())
                    return Failure<void>(NavigationErrors::NavMeshArtifactCorrupt);
                return Result<void>::Success();
            }
            if (result.state != NavigationTileBuildState::Built)
                return Failure<void>(NavigationErrors::NavMeshArtifactCorrupt);
            return ValidateBuiltTopology(input, result, bytes);
        }

        /** @brief Encodes portable topology; no statistics, allocator capacity or provider memory layout is persisted. */
        void EncodeTopology(Writer &writer, const NavigationTileBuildResult &result) {
            writer.Integer(static_cast<std::uint8_t>(result.state), 1);
            writer.Integer(result.vertices.size());
            for (const auto vertex : result.vertices)
                writer.Vector(vertex);
            writer.Integer(result.polygons.size());
            for (const auto &polygon : result.polygons) {
                writer.Integer(polygon.vertexIndices.first);
                writer.Integer(polygon.vertexIndices.count);
                writer.Integer(polygon.adjacencies.first);
                writer.Integer(polygon.adjacencies.count);
                writer.Integer(polygon.area.Value(), 8);
            }
            for (const auto *table : {&result.polygonVertexIndices, &result.polygonAdjacencies}) {
                writer.Integer(table->size());
                for (const auto index : *table)
                    writer.Integer(index);
            }
            writer.Integer(result.offMeshLinks.size());
            for (const auto &link : result.offMeshLinks) {
                writer.Vector(link.start);
                writer.Vector(link.end);
                writer.Float(link.radiusMeters);
                writer.Integer(link.startPolygon);
                writer.Integer(link.endPolygon);
                writer.Integer(link.area.Value(), 8);
                writer.Integer(link.bidirectional ? 1 : 0, 1);
            }
            writer.Integer(result.provenance.size());
            for (const auto &source : result.provenance) {
                writer.Integer(static_cast<std::uint8_t>(source.kind), 1);
                writer.Integer(source.producer.Value(), 8);
                writer.Integer(source.contribution.Value(), 8);
                writer.Integer(source.revision.Value(), 8);
                writer.Digest(source.sourceDigest);
                writer.Integer(source.polygons.first);
                writer.Integer(source.polygons.count);
            }
        }

        /** @brief Parses tables only after each count fits both hard limits and remaining bytes. */
        [[nodiscard]] NavigationTileBuildResult DecodeTopology(Reader &reader, const NavigationPreparedTile &input) {
            NavigationTileBuildResult result{.state = static_cast<NavigationTileBuildState>(reader.Integer(1)),
                                             .key = input.tile.key.tile,
                                             .bounds = input.tile.bounds};
            result.vertices.resize(reader.Count(NavigationTileBuildLimits::MaximumVertices, 12));
            for (auto &vertex : result.vertices)
                vertex = reader.Vector();
            result.polygons.resize(reader.Count(NavigationTileBuildLimits::MaximumPolygons, 24));
            for (auto &polygon : result.polygons) {
                polygon.vertexIndices = {static_cast<std::uint32_t>(reader.Integer()), static_cast<std::uint32_t>(reader.Integer())};
                polygon.adjacencies = {static_cast<std::uint32_t>(reader.Integer()), static_cast<std::uint32_t>(reader.Integer())};
                polygon.area = ReadIdentity<NavigationAreaId>(reader);
            }
            for (auto *table : {&result.polygonVertexIndices, &result.polygonAdjacencies}) {
                table->resize(reader.Count(NavigationTileBuildLimits::MaximumPolygons * 6ULL, 4));
                for (auto &index : *table)
                    index = static_cast<std::uint32_t>(reader.Integer());
            }
            result.offMeshLinks.resize(reader.Count(NavigationTileBuildLimits::MaximumOffMeshLinks, 45));
            for (auto &link : result.offMeshLinks) {
                link.start = reader.Vector();
                link.end = reader.Vector();
                link.radiusMeters = reader.Float();
                link.startPolygon = static_cast<std::uint32_t>(reader.Integer());
                link.endPolygon = static_cast<std::uint32_t>(reader.Integer());
                link.area = ReadIdentity<NavigationAreaId>(reader);
                const auto bidirectional = reader.Integer(1);
                if (bidirectional > 1)
                    throw std::invalid_argument("Invalid navigation link direction");
                link.bidirectional = bidirectional == 1;
            }
            result.provenance.resize(reader.Count(NavigationTileBuildLimits::MaximumPolygons, 65));
            for (auto &source : result.provenance) {
                source.kind = static_cast<NavigationSourceProducerKind>(reader.Integer(1));
                source.producer = ReadIdentity<NavigationSourceProducerId>(reader);
                source.contribution = ReadIdentity<NavigationSourceContributionId>(reader);
                source.revision = ReadIdentity<NavigationSourceRevision>(reader);
                source.sourceDigest = reader.Digest();
                source.polygons = {static_cast<std::uint32_t>(reader.Integer()), static_cast<std::uint32_t>(reader.Integer())};
            }
            return result;
        }
    }  // namespace

    /** @copydoc NavigationCookedTile::Create */
    Result<std::shared_ptr<const NavigationCookedTile>> NavigationCookedTile::Create(const NavigationPreparedTile &input,
                                                                                     NavigationTileBuildResult result,
                                                                                     const std::size_t maximumBytes) {
        using namespace TileArtifactInternal;
        if (maximumBytes == 0 || maximumBytes > NavigationTileBuildLimits::MaximumOwnedBytes)
            return Failure<std::shared_ptr<const NavigationCookedTile>>(NavigationErrors::BakeInputInvalid);
        try {
            Writer writer(maximumBytes);
            if (result.vertices.size() > NavigationTileBuildLimits::MaximumVertices ||
                result.polygons.size() > NavigationTileBuildLimits::MaximumPolygons ||
                result.offMeshLinks.size() > NavigationTileBuildLimits::MaximumOffMeshLinks ||
                result.polygonVertexIndices.size() > NavigationTileBuildLimits::MaximumPolygons * 6ULL ||
                result.polygonAdjacencies.size() > NavigationTileBuildLimits::MaximumPolygons * 6ULL ||
                result.provenance.size() > NavigationTileBuildLimits::MaximumPolygons)
                return Failure<std::shared_ptr<const NavigationCookedTile>>(NavigationErrors::CapacityExceeded);
            EncodeDescriptor(writer, input);
            EncodeTopology(writer, result);
            if (const auto valid = Validate(input, result, writer.Bytes().size()); valid.HasError())
                return Result<std::shared_ptr<const NavigationCookedTile>>::Failure(valid.ErrorValue());
            return Result<std::shared_ptr<const NavigationCookedTile>>::Success(
                std::make_shared<const NavigationCookedTile>(ConstructionKey{}, input.tile.key, input.dependencyKey, std::move(result),
                                                             std::move(writer).TakeBytes()));
        } catch (const std::length_error &) {
            return Failure<std::shared_ptr<const NavigationCookedTile>>(NavigationErrors::CapacityExceeded);
        } catch (const std::bad_alloc &) {
            return Failure<std::shared_ptr<const NavigationCookedTile>>(NavigationErrors::CapacityExceeded);
        }
    }

    /** @copydoc NavigationCookedTile::Decode */
    Result<std::shared_ptr<const NavigationCookedTile>> NavigationCookedTile::Decode(const std::span<const std::uint8_t> bytes,
                                                                                     const std::size_t maximumBytes) {
        using namespace TileArtifactInternal;
        if (maximumBytes == 0 || maximumBytes > NavigationTileBuildLimits::MaximumOwnedBytes)
            return Failure<std::shared_ptr<const NavigationCookedTile>>(NavigationErrors::BakeInputInvalid);
        if (bytes.size() > maximumBytes)
            return Failure<std::shared_ptr<const NavigationCookedTile>>(NavigationErrors::CapacityExceeded);
        try {
            Reader reader(bytes);
            const auto input = DecodeDescriptor(reader);
            auto result = DecodeTopology(reader, input);
            if (!reader.Empty())
                return Failure<std::shared_ptr<const NavigationCookedTile>>(NavigationErrors::NavMeshArtifactCorrupt);
            auto artifact = Create(input, std::move(result), maximumBytes);
            if (artifact.HasValue() && !std::ranges::equal(artifact.Value()->Bytes(), bytes))
                return Failure<std::shared_ptr<const NavigationCookedTile>>(NavigationErrors::NavMeshArtifactCorrupt);
            return artifact;
        } catch (const std::invalid_argument &) {
            return Failure<std::shared_ptr<const NavigationCookedTile>>(NavigationErrors::NavMeshArtifactCorrupt);
        } catch (const std::bad_alloc &) {
            return Failure<std::shared_ptr<const NavigationCookedTile>>(NavigationErrors::CapacityExceeded);
        }
    }

    /** @copydoc NavigationCookedTile::StorageBytes */
    std::size_t NavigationCookedTile::StorageBytes() const noexcept {
        return sizeof(*this) + bytes_.capacity() + topology_.vertices.capacity() * sizeof(Math::Vec3) +
               topology_.polygons.capacity() * sizeof(NavMeshPolygon) + topology_.polygonVertexIndices.capacity() * sizeof(std::uint32_t) +
               topology_.polygonAdjacencies.capacity() * sizeof(std::uint32_t) +
               topology_.offMeshLinks.capacity() * sizeof(NavMeshOffMeshLink) +
               topology_.provenance.capacity() * sizeof(NavMeshSourceProvenance) +
               topology_.warnings.capacity() * sizeof(NavigationTileBuildWarning);
    }

    /** @copydoc NavigationCookedTile::NavigationCookedTile */
    NavigationCookedTile::NavigationCookedTile(ConstructionKey, const NavigationBakeTileKey &key, const Sha256Digest &dependency,
                                               NavigationTileBuildResult result, std::vector<std::uint8_t> bytes)
        : key_(key), dependencyKey_(dependency), contentIdentity_(ComputeSha256(std::as_bytes(std::span{bytes}))),
          topology_(std::move(result)), bytes_(std::move(bytes)) {}
}  // namespace Horo::Navigation
