#include "Horo/Navigation/NavMeshCodec.h"

#include "Horo/Navigation/NavigationErrors.h"
#include "NavMeshWireArchive.h"

#include <array>

namespace Horo::Navigation {
    namespace {
        constexpr std::uint32_t Magic = 0x314d4e48U;
        constexpr std::size_t FixedHeaderBytes = 203;

        /** @brief Temporary tables are bounded by header preflight and exact remaining wire bytes. */
        struct Tables final {
            std::vector<Math::Vec3> vertices;
            std::vector<NavMeshPolygon> polygons;
            std::vector<std::uint32_t> indices;
            std::vector<std::uint32_t> adjacencies;
            std::vector<NavMeshOffMeshLink> links;
            std::vector<NavMeshSourceProvenance> provenance;
            std::vector<NavMeshProviderPayloadDescriptor> providers;
            std::vector<std::byte> providerBytes;

            [[nodiscard]] NavMeshDecodedTablesView View() const noexcept {
                return {vertices, polygons, indices, adjacencies, links, provenance, providers};
            }
        };

        template <class T> constexpr std::size_t WireBytes = 0;
        template <> constexpr std::size_t WireBytes<Math::Vec3> = 12;
        template <> constexpr std::size_t WireBytes<NavMeshPolygon> = 24;
        template <> constexpr std::size_t WireBytes<std::uint32_t> = 4;
        template <> constexpr std::size_t WireBytes<NavMeshOffMeshLink> = 45;
        template <> constexpr std::size_t WireBytes<NavMeshSourceProvenance> = 65;
        template <> constexpr std::size_t WireBytes<NavMeshProviderPayloadDescriptor> = 94;

        /** @brief Traverse only a previously validated tile's owned decoded rows. */
        template <class T> void WriteRows(MeshWire::Writer &writer, const std::span<const T> rows, const NavMeshTableRange range) {
            for (auto row : rows.subspan(range.first, range.count))
                MeshWire::Fields(writer, row);
        }

        /** @brief Check contiguous global ownership and byte availability before allocating one tile table. */
        template <class T>
        void ReadRows(MeshWire::Reader &reader, std::vector<T> &rows, const NavMeshTableRange range, const std::uint32_t maximum) {
            if (range.first != rows.size() || range.first > maximum || range.count > maximum - range.first ||
                static_cast<std::uint64_t>(range.count) * WireBytes<T> > reader.Remaining()) {
                reader.valid = false;
                return;
            }
            const auto previousSize = rows.size();
            rows.resize(previousSize + range.count);
            for (auto &row : std::span<T>{rows}.subspan(previousSize))
                MeshWire::Fields(reader, row);
        }

        /** @brief Encode exact tile rows and opaque provider bytes without provider-native layout. */
        [[nodiscard]] MeshWire::Writer WriteTile(const NavMeshArtifactView &artifact, const NavMeshTileDescriptor &tile,
                                                 const std::size_t maximum) {
            MeshWire::Writer writer{maximum};
            WriteRows(writer, artifact.tables.vertices, tile.vertices);
            WriteRows(writer, artifact.tables.polygons, tile.polygons);
            WriteRows(writer, artifact.tables.polygonVertexIndices, tile.polygonVertexIndices);
            WriteRows(writer, artifact.tables.polygonAdjacencies, tile.polygonAdjacencies);
            WriteRows(writer, artifact.tables.offMeshLinks, tile.offMeshLinks);
            WriteRows(writer, artifact.tables.provenance, tile.provenance);
            WriteRows(writer, artifact.tables.providerPayloads, tile.providerPayloads);
            for (const auto &provider :
                 artifact.tables.providerPayloads.subspan(tile.providerPayloads.first, tile.providerPayloads.count)) {
                writer.Raw(artifact.providerPayloadBytes.subspan(static_cast<std::size_t>(provider.byteOffset),
                                                                 static_cast<std::size_t>(provider.encodedBytes)));
            }
            return writer;
        }

        /** @brief Decode tile rows only within exact preflighted global counts and encoded fragment size. */
        void ReadTile(MeshWire::Reader &reader, Tables &tables, const NavMeshTileDescriptor &tile, const NavMeshArtifactHeader &header) {
            ReadRows(reader, tables.vertices, tile.vertices, header.vertexCount);
            ReadRows(reader, tables.polygons, tile.polygons, header.polygonCount);
            ReadRows(reader, tables.indices, tile.polygonVertexIndices, header.polygonVertexIndexCount);
            ReadRows(reader, tables.adjacencies, tile.polygonAdjacencies, header.polygonAdjacencyCount);
            ReadRows(reader, tables.links, tile.offMeshLinks, header.offMeshLinkCount);
            ReadRows(reader, tables.provenance, tile.provenance, header.provenanceCount);
            ReadRows(reader, tables.providers, tile.providerPayloads, header.providerPayloadCount);
            if (!reader.valid)
                return;
            for (const auto &provider :
                 std::span<const NavMeshProviderPayloadDescriptor>{tables.providers}.subspan(tile.providerPayloads.first,
                                                                                             tile.providerPayloads.count)) {
                if (provider.byteOffset != tables.providerBytes.size() || provider.byteOffset > header.providerEncodedBytes ||
                    provider.encodedBytes > header.providerEncodedBytes - provider.byteOffset ||
                    provider.encodedBytes > reader.Remaining()) {
                    reader.valid = false;
                    return;
                }
                const auto bytes = reader.Raw(static_cast<std::size_t>(provider.encodedBytes));
                tables.providerBytes.insert(tables.providerBytes.end(), bytes.begin(), bytes.end());
            }
        }

        /** @brief Preserve one stable corruption descriptor for malformed bytes rather than parser internals. */
        [[nodiscard]] Result<DecodedNavMeshArtifact> Corrupt() {
            return Result<DecodedNavMeshArtifact>::Failure(MakeError(NavigationErrors::NavMeshArtifactCorrupt));
        }

        /** @brief Verify fixed schema and actual body bytes without allocating table storage. */
        [[nodiscard]] Result<NavMeshArtifactHeader> ReadHeader(MeshWire::Reader &reader, const std::span<const std::byte> bytes,
                                                               const NavMeshArtifactLimits &limits) {
            std::uint32_t magic{};
            reader(magic);
            NavMeshArtifactHeader header;
            MeshWire::Fields(reader, header);
            if (!reader.valid || magic != Magic)
                return Result<NavMeshArtifactHeader>::Failure(MakeError(NavigationErrors::NavMeshArtifactCorrupt));
            if (const auto preflight = ValidateNavMeshArtifactHeader(header, limits); preflight.HasError())
                return Result<NavMeshArtifactHeader>::Failure(preflight.ErrorValue());
            if (header.compression != NavMeshCompression::None)
                return Result<NavMeshArtifactHeader>::Failure(MakeError(NavigationErrors::UnsupportedCookedVersion));
            if (header.portableEncodedBytes != reader.Remaining() ||
                header.payloadDigest != ComputeSha256(bytes.subspan(reader.position)) || header.tileCount > reader.Remaining() / 130)
                return Result<NavMeshArtifactHeader>::Failure(MakeError(NavigationErrors::NavMeshArtifactCorrupt));
            return Result<NavMeshArtifactHeader>::Success(header);
        }

        /** @brief Decode exactly the preflighted fragments before the existing semantic validation barrier. */
        [[nodiscard]] Result<DecodedNavMeshArtifact> ReadBody(MeshWire::Reader &reader, const NavMeshArtifactHeader &header,
                                                              const NavMeshArtifactLimits &limits) {
            std::vector<NavMeshTileDescriptor> tiles;
            std::vector<Sha256Digest> observed;
            std::vector<NavMeshEncodedTileRange> ranges;
            Tables tables;
            tiles.reserve(header.tileCount);
            observed.reserve(header.tileCount);
            ranges.reserve(header.tileCount);
            for (std::uint32_t index = 0; index < header.tileCount; ++index) {
                NavMeshTileDescriptor tile;
                MeshWire::Fields(reader, tile);
                std::uint64_t size{};
                reader(size);
                if (!reader.valid || size > reader.Remaining())
                    return Corrupt();
                ranges.emplace_back(tile.key, reader.position, static_cast<std::size_t>(size));
                const auto chunk = reader.Raw(static_cast<std::size_t>(size));
                const auto digest = ComputeSha256(chunk);
                if (digest != tile.payloadDigest)
                    return Corrupt();
                MeshWire::Reader chunkReader{chunk};
                ReadTile(chunkReader, tables, tile, header);
                if (!chunkReader.valid || chunkReader.Remaining() != 0)
                    return Corrupt();
                tiles.push_back(tile);
                observed.push_back(digest);
            }
            if (!reader.valid || reader.Remaining() != 0)
                return Corrupt();
            const NavMeshArtifactView artifact{header, header.payloadDigest, tiles, observed, tables.View(), tables.providerBytes};
            auto data = NavMeshData::Create(artifact, limits);
            if (data.HasError())
                return Result<DecodedNavMeshArtifact>::Failure(data.ErrorValue());
            return Result<DecodedNavMeshArtifact>::Success({std::move(data).Value(), std::move(ranges)});
        }
    }  // namespace

    /** @copydoc EncodeNavMeshArtifact */
    Result<std::vector<std::byte>> EncodeNavMeshArtifact(const NavMeshArtifactView &artifact, const NavMeshArtifactLimits &limits) {
        if (const auto validated = ValidateNavMeshArtifact(artifact, limits); validated.HasError())
            return Result<std::vector<std::byte>>::Failure(validated.ErrorValue());
        if (artifact.header.compression != NavMeshCompression::None || artifact.header.byteOrder != NavMeshByteOrder::LittleEndian) {
            return Result<std::vector<std::byte>>::Failure(MakeError(NavigationErrors::UnsupportedCookedVersion));
        }
        MeshWire::Writer body{static_cast<std::size_t>(limits.maxPortableEncodedBytes)};
        for (auto tile : artifact.tiles) {
            auto encoded = WriteTile(artifact, tile, static_cast<std::size_t>(limits.maxPortableEncodedBytes));
            if (!encoded.valid)
                return Result<std::vector<std::byte>>::Failure(MakeError(NavigationErrors::NavMeshArtifactCapacityExceeded));
            tile.payloadDigest = ComputeSha256(encoded.bytes);
            MeshWire::Fields(body, tile);
            body(static_cast<std::uint64_t>(encoded.bytes.size()));
            body.Raw(encoded.bytes);
        }
        if (!body.valid)
            return Result<std::vector<std::byte>>::Failure(MakeError(NavigationErrors::NavMeshArtifactCapacityExceeded));
        auto header = artifact.header;
        header.portableEncodedBytes = body.bytes.size();
        header.portableDecodedBytes = body.bytes.size();
        header.payloadDigest = ComputeSha256(body.bytes);
        if (const auto preflight = ValidateNavMeshArtifactHeader(header, limits); preflight.HasError())
            return Result<std::vector<std::byte>>::Failure(preflight.ErrorValue());
        MeshWire::Writer output{FixedHeaderBytes + body.bytes.size()};
        output(Magic);
        MeshWire::Fields(output, header);
        output.Raw(body.bytes);
        return Result<std::vector<std::byte>>::Success(std::move(output.bytes));
    }

    /** @copydoc DecodeNavMeshArtifact */
    Result<DecodedNavMeshArtifact> DecodeNavMeshArtifact(const std::span<const std::byte> bytes, const NavMeshArtifactLimits &limits) {
        if (bytes.size() < FixedHeaderBytes)
            return Corrupt();
        MeshWire::Reader reader{bytes};
        const auto header = ReadHeader(reader, bytes, limits);
        if (header.HasError())
            return Result<DecodedNavMeshArtifact>::Failure(header.ErrorValue());
        return ReadBody(reader, header.Value(), limits);
    }
}  // namespace Horo::Navigation
