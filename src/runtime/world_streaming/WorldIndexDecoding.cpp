#include "Horo/WorldStreaming/WorldIndexArtifact.h"
#include "WorldIndexWire.h"

#include <ranges>

namespace Horo::WorldStreaming {
    namespace {
        using namespace Detail;

        /** @brief Requires every byte in an already range-checked reserved field to remain zero. */
        bool Zero(const std::span<const std::byte> bytes, const std::size_t offset, const std::size_t count) noexcept {
            return std::ranges::all_of(bytes.subspan(offset, count), [](const auto byte) {
                return byte == std::byte{0};
            });
        }

        /** @brief Validates fixed fields and exact section packing before any table access or allocation. */
        Result<WorldIndexLayout> Header(const std::span<const std::byte> bytes, const WorldIndexArtifactLimits &limits) {
            if (bytes.size() < WorldIndexHeaderBytes)
                return WorldIndexReject<WorldIndexLayout>(WorldArtifactErrors::Invalid, "world.index header is truncated.");
            if (bytes.size() > limits.maximumArtifactBytes)
                return WorldIndexReject<WorldIndexLayout>(WorldArtifactErrors::CapacityExceeded);
            if (WorldIndexRead<std::uint32_t>(bytes, 0) != 0x58444e49)
                return WorldIndexReject<WorldIndexLayout>(WorldArtifactErrors::Invalid, "world.index magic is invalid.");
            if (WorldIndexRead<std::uint16_t>(bytes, 4) != 1 || WorldIndexRead<std::uint16_t>(bytes, 6) != 0 ||
                std::to_integer<unsigned>(bytes[104]) > 3)
                return WorldIndexReject<WorldIndexLayout>(WorldArtifactErrors::Unsupported);
            if (WorldIndexRead<std::uint32_t>(bytes, 12) != WorldIndexHeaderBytes || !Zero(bytes, 8, 4) || !Zero(bytes, 106, 2) ||
                !Zero(bytes, 140, 4) || !Zero(bytes, 184, 8) || WorldIndexRead<std::uint32_t>(bytes, 32) != 0 ||
                WorldIndexRead<std::uint64_t>(bytes, 144) != bytes.size())
                return WorldIndexReject<WorldIndexLayout>(WorldArtifactErrors::Invalid, "world.index fixed fields are noncanonical.");
            const auto layout =
                WorldIndexPackedLayout(WorldIndexRead<std::uint32_t>(bytes, 112), WorldIndexRead<std::uint32_t>(bytes, 120),
                                       WorldIndexRead<std::uint32_t>(bytes, 128), WorldIndexRead<std::uint32_t>(bytes, 136), limits);
            if (layout.HasError())
                return layout;
            if (const auto &packed = layout.Value();
                packed.layers != WorldIndexRead<std::uint32_t>(bytes, 108) || packed.volumes != WorldIndexRead<std::uint32_t>(bytes, 116) ||
                packed.cells != WorldIndexRead<std::uint32_t>(bytes, 124) || packed.strings != WorldIndexRead<std::uint32_t>(bytes, 132) ||
                packed.total != bytes.size())
                return WorldIndexReject<WorldIndexLayout>(WorldArtifactErrors::Invalid, "world.index sections are not exactly packed.");
            return layout;
        }

        /** @brief Reads one canonical NFC string at the next exact string-table position. */
        Result<std::string> Name(const std::span<const std::byte> bytes, const WorldIndexLayout &layout, const std::size_t offset,
                                 const std::size_t size, std::size_t &cursor, const WorldIndexArtifactLimits &limits) {
            if (const auto remaining = layout.total - layout.strings - cursor; offset != cursor || size > remaining)
                return WorldIndexReject<std::string>(WorldArtifactErrors::Invalid, "world.index name ranges are not canonical.");
            const auto encoded = bytes.subspan(layout.strings + cursor, size);
            std::string text;
            text.reserve(encoded.size());
            for (const auto byte : encoded)
                text.push_back(std::to_integer<char>(byte));
            auto normalized = WorldIndexName(text, limits.maximumStringBytes);
            if (normalized.HasError())
                return normalized;
            if (normalized.Value() != text)
                return WorldIndexReject<std::string>(WorldArtifactErrors::Invalid, "world.index names must already be NFC.");
            cursor += size;
            return normalized;
        }

        /** @brief Reconstructs a canonical exact global coordinate from three admitted finite meter values. */
        Result<Math::WorldCoordinate64> Coordinate(const std::span<const std::byte> bytes, const std::size_t offset) {
            std::array<std::int64_t, 3> values{};
            for (std::size_t i = 0; i < values.size(); ++i) {
                const auto millimeters = WorldIndexMillimeters(std::bit_cast<float>(WorldIndexRead<std::uint32_t>(bytes, offset + i * 4)));
                if (millimeters.HasError())
                    return Result<Math::WorldCoordinate64>::Failure(millimeters.ErrorValue());
                values[i] = millimeters.Value();
            }
            return Result<Math::WorldCoordinate64>::Success(Math::WorldCoordinate64::FromMillimeters(values[0], values[1], values[2]));
        }

        /** @brief Copies sorted layer rows only after complete packed-table admission. */
        Result<std::vector<WorldLayerDescriptor>> Layers(const std::span<const std::byte> bytes, const WorldIndexLayout &layout,
                                                         std::size_t &cursor, const WorldIndexArtifactLimits &limits,
                                                         const CancellationToken &cancellation) {
            const auto count = WorldIndexRead<std::uint32_t>(bytes, 112);
            std::vector<WorldLayerDescriptor> result;
            result.reserve(count);
            std::size_t nameBytes{};
            for (std::size_t i = 0; i < count; ++i) {
                if (cancellation.IsCancellationRequested())
                    return WorldIndexReject<std::vector<WorldLayerDescriptor>>(WorldArtifactErrors::Cancelled);
                const auto row = layout.layers + i * WorldIndexLayerBytes;
                const auto rawId = WorldIndexRead<std::uint16_t>(bytes, row);
                if (!Zero(bytes, row + 3, 1) || !Zero(bytes, row + 20, 12) || (i && rawId <= result.back().id.Value()))
                    return WorldIndexReject<std::vector<WorldLayerDescriptor>>(WorldArtifactErrors::Invalid);
                auto id = StreamingLayerId::Create(rawId);
                if (id.HasError())
                    return Result<std::vector<WorldLayerDescriptor>>::Failure(id.ErrorValue());
                auto name = Name(bytes, layout, WorldIndexRead<std::uint32_t>(bytes, row + 12),
                                 WorldIndexRead<std::uint32_t>(bytes, row + 16), cursor, limits);
                if (name.HasError())
                    return Result<std::vector<WorldLayerDescriptor>>::Failure(name.ErrorValue());
                if (name.Value().size() > limits.partition.maximumLayerNameBytes - nameBytes)
                    return WorldIndexReject<std::vector<WorldLayerDescriptor>>(WorldArtifactErrors::CapacityExceeded);
                nameBytes += name.Value().size();
                result.push_back({.id = id.Value(),
                                  .name = std::move(name).Value(),
                                  .ownership = static_cast<WorldLayerOwnership>(std::to_integer<std::uint8_t>(bytes[row + 2])),
                                  .flags = static_cast<WorldLayerFlags>(WorldIndexRead<std::uint32_t>(bytes, row + 4)),
                                  .priorityMultiplier = std::bit_cast<float>(WorldIndexRead<std::uint32_t>(bytes, row + 8))});
            }
            return Result<std::vector<WorldLayerDescriptor>>::Success(std::move(result));
        }

        /** @brief Copies all static-volume fields without resolving providers or selecting runtime policy. */
        Result<std::vector<WorldIndexVolume>> Volumes(const std::span<const std::byte> bytes, const WorldIndexLayout &layout,
                                                      const CancellationToken &cancellation) {
            const auto count = WorldIndexRead<std::uint32_t>(bytes, 120);
            const auto layers = WorldIndexRead<std::uint32_t>(bytes, 112);
            std::vector<WorldIndexVolume> result;
            result.reserve(count);
            for (std::size_t i = 0; i < count; ++i) {
                if (cancellation.IsCancellationRequested())
                    return WorldIndexReject<std::vector<WorldIndexVolume>>(WorldArtifactErrors::Cancelled);
                const auto row = layout.volumes + i * WorldIndexVolumeBytes;
                if (!Zero(bytes, row + 7, 1) || !Zero(bytes, row + 40, 24) || std::to_integer<unsigned>(bytes[row + 6]) > 1)
                    return WorldIndexReject<std::vector<WorldIndexVolume>>(WorldArtifactErrors::Invalid);
                WorldIndexVolume volume{.id = WorldIndexRead<std::uint32_t>(bytes, row),
                                        .kind = static_cast<WorldIndexVolumeKind>(std::to_integer<std::uint8_t>(bytes[row + 4])),
                                        .shape = static_cast<WorldIndexVolumeShape>(std::to_integer<std::uint8_t>(bytes[row + 5])),
                                        .unloadOutside = bytes[row + 6] != std::byte{0},
                                        .priority = std::bit_cast<float>(WorldIndexRead<std::uint32_t>(bytes, row + 32)),
                                        .layerMask = WorldIndexRead<std::uint32_t>(bytes, row + 36)};
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    volume.origin[axis] = std::bit_cast<float>(WorldIndexRead<std::uint32_t>(bytes, row + 8 + axis * 4));
                    volume.extents[axis] = std::bit_cast<float>(WorldIndexRead<std::uint32_t>(bytes, row + 20 + axis * 4));
                }
                if (!WorldIndexValidVolume(volume, layers) || (i && volume.id <= result.back().id))
                    return WorldIndexReject<std::vector<WorldIndexVolume>>(WorldArtifactErrors::Invalid);
                result.push_back(volume);
            }
            return Result<std::vector<WorldIndexVolume>>::Success(std::move(result));
        }

        /** @brief Owns topology and cooked integrity in parallel without inventing missing dependency data. */
        struct CellRows final {
            std::vector<WorldPartitionCellDescriptor> topology;
            std::vector<CookedWorldCellManifestEntry> integrity;
        };

        /** @brief Validates complete canonical cells and aggregate budgets before descriptor publication. */
        Result<CellRows> Cells(const std::span<const std::byte> bytes, const WorldIndexLayout &layout,
                               const WorldIndexArtifactLimits &limits, const CancellationToken &cancellation) {
            const auto count = WorldIndexRead<std::uint32_t>(bytes, 128);
            CellRows result;
            result.topology.reserve(count);
            result.integrity.reserve(count);
            std::uint64_t compressed{};
            std::uint64_t decoded{};
            for (std::size_t i = 0; i < count; ++i) {
                if (cancellation.IsCancellationRequested())
                    return WorldIndexReject<CellRows>(WorldArtifactErrors::Cancelled);
                const auto row = layout.cells + i * WorldIndexCellBytes;
                if (!Zero(bytes, row + 13, 1) || !Zero(bytes, row + 52, 4) || !Zero(bytes, row + 88, 8))
                    return WorldIndexReject<CellRows>(WorldArtifactErrors::Invalid);
                const auto layer = StreamingLayerId::Create(WorldIndexRead<std::uint16_t>(bytes, row + 14));
                if (layer.HasError())
                    return Result<CellRows>::Failure(layer.ErrorValue());
                const StreamingCellId cell{WorldIndexRead<std::int32_t>(bytes, row), WorldIndexRead<std::int32_t>(bytes, row + 4),
                                           WorldIndexRead<std::int32_t>(bytes, row + 8), std::to_integer<std::uint8_t>(bytes[row + 12]),
                                           layer.Value()};
                if (i && !StreamingCellCanonicalLess{}(result.topology.back().id, cell))
                    return WorldIndexReject<CellRows>(WorldArtifactErrors::Invalid, "world.index cell order is not canonical.");
                std::array<std::uint8_t, 16> asset{};
                for (std::size_t byte = 0; byte < asset.size(); ++byte)
                    asset[byte] = std::to_integer<std::uint8_t>(bytes[row + 16 + byte]);
                CookedWorldCellManifestEntry entry{.cell = cell,
                                                   .uncompressedSize = WorldIndexRead<std::uint64_t>(bytes, row + 32),
                                                   .compressedSize = WorldIndexRead<std::uint64_t>(bytes, row + 40),
                                                   .payloadCrc32 = WorldIndexRead<std::uint32_t>(bytes, row + 48)};
                for (std::size_t byte = 0; byte < entry.artifactHash.bytes.size(); ++byte)
                    entry.artifactHash.bytes[byte] = std::to_integer<std::uint8_t>(bytes[row + 56 + byte]);
                if (!entry.compressedSize || !entry.uncompressedSize || entry.artifactHash == Sha256Digest{})
                    return WorldIndexReject<CellRows>(WorldArtifactErrors::Invalid);
                if (entry.compressedSize > limits.cooked.maximumCompressedBytes - compressed ||
                    entry.uncompressedSize > limits.cooked.maximumUncompressedBytes - decoded)
                    return WorldIndexReject<CellRows>(WorldArtifactErrors::CapacityExceeded);
                compressed += entry.compressedSize;
                decoded += entry.uncompressedSize;
                result.topology.push_back({cell, {Assets::AssetId::FromBytes(asset)}});
                result.integrity.push_back(entry);
            }
            return Result<CellRows>::Success(std::move(result));
        }

        /** @brief Reuses the existing canonical grid and partition authority after wire validation completes. */
        Result<WorldPartitionDescriptor> Descriptor(const std::span<const std::byte> bytes,
                                                    const std::span<const WorldLayerDescriptor> layers,
                                                    const std::span<const WorldPartitionCellDescriptor> cells,
                                                    const WorldIndexArtifactLimits &limits) {
            SerializedWorldPartitionId guid{};
            for (std::size_t i = 0; i < guid.size(); ++i)
                guid[i] = std::to_integer<std::uint8_t>(bytes[16 + i]);
            const auto partition = WorldPartitionId::Create(guid);
            if (partition.HasError())
                return Result<WorldPartitionDescriptor>::Failure(partition.ErrorValue());
            const auto minimum = Coordinate(bytes, 40);
            const auto maximum = Coordinate(bytes, 52);
            const auto origin = Coordinate(bytes, 68);
            if (minimum.HasError())
                return Result<WorldPartitionDescriptor>::Failure(minimum.ErrorValue());
            if (maximum.HasError())
                return Result<WorldPartitionDescriptor>::Failure(maximum.ErrorValue());
            if (origin.HasError())
                return Result<WorldPartitionDescriptor>::Failure(origin.ErrorValue());
            const auto size = WorldIndexMillimeters(std::bit_cast<float>(WorldIndexRead<std::uint32_t>(bytes, 64)));
            if (size.HasError())
                return Result<WorldPartitionDescriptor>::Failure(size.ErrorValue());
            const WorldCellBounds bounds{WorldIndexRead<std::int32_t>(bytes, 80), WorldIndexRead<std::int32_t>(bytes, 84),
                                         WorldIndexRead<std::int32_t>(bytes, 88), WorldIndexRead<std::int32_t>(bytes, 92),
                                         WorldIndexRead<std::int32_t>(bytes, 96), WorldIndexRead<std::int32_t>(bytes, 100)};
            const auto grid =
                WorldCellQuantizationPolicy::Create(origin.Value(), size.Value(), bounds, std::to_integer<std::uint8_t>(bytes[105]));
            if (grid.HasError())
                return Result<WorldPartitionDescriptor>::Failure(grid.ErrorValue());
            return WorldPartitionDescriptor::Create({}, partition.Value(), {minimum.Value(), maximum.Value()}, grid.Value(), layers, cells,
                                                    limits.partition);
        }
    }  // namespace

    /** @copydoc DecodeWorldIndexArtifact */
    Result<DecodedWorldIndex> DecodeWorldIndexArtifact(const std::span<const std::byte> bytes, const WorldIndexArtifactLimits &limits,
                                                       const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return WorldIndexReject<DecodedWorldIndex>(WorldArtifactErrors::Cancelled);
        const auto layout = Header(bytes, limits);
        if (layout.HasError())
            return Result<DecodedWorldIndex>::Failure(layout.ErrorValue());
        const auto hash = WorldIndexDigest(bytes, true, cancellation);
        if (hash.HasError())
            return Result<DecodedWorldIndex>::Failure(hash.ErrorValue());
        for (std::size_t i = 0; i < hash.Value().bytes.size(); ++i)
            if (static_cast<std::byte>(hash.Value().bytes[i]) != bytes[WorldIndexHashOffset + i])
                return WorldIndexReject<DecodedWorldIndex>(WorldArtifactErrors::Invalid, "world.index digest does not match its bytes.");
        std::size_t cursor{};
        auto name = Name(bytes, layout.Value(), 0, WorldIndexRead<std::uint32_t>(bytes, 36), cursor, limits);
        if (name.HasError())
            return Result<DecodedWorldIndex>::Failure(name.ErrorValue());
        auto layers = Layers(bytes, layout.Value(), cursor, limits, cancellation);
        if (layers.HasError())
            return Result<DecodedWorldIndex>::Failure(layers.ErrorValue());
        if (cursor != layout.Value().total - layout.Value().strings)
            return WorldIndexReject<DecodedWorldIndex>(WorldArtifactErrors::Invalid, "world.index contains unreferenced string bytes.");
        auto volumes = Volumes(bytes, layout.Value(), cancellation);
        if (volumes.HasError())
            return Result<DecodedWorldIndex>::Failure(volumes.ErrorValue());
        auto cells = Cells(bytes, layout.Value(), limits, cancellation);
        if (cells.HasError())
            return Result<DecodedWorldIndex>::Failure(cells.ErrorValue());
        auto ownedCells = std::move(cells).Value();
        auto descriptor = Descriptor(bytes, layers.Value(), ownedCells.topology, limits);
        if (descriptor.HasError())
            return Result<DecodedWorldIndex>::Failure(descriptor.ErrorValue());
        const auto artifact = WorldIndexDigest(bytes, false, cancellation);
        if (artifact.HasError())
            return Result<DecodedWorldIndex>::Failure(artifact.ErrorValue());
        return Result<DecodedWorldIndex>::Success({std::move(descriptor).Value(), std::move(name).Value(),
                                                   static_cast<WorldIndexCoordinateSystem>(std::to_integer<std::uint8_t>(bytes[104])),
                                                   std::move(volumes).Value(), std::move(ownedCells.integrity), hash.Value(),
                                                   artifact.Value()});
    }
}  // namespace Horo::WorldStreaming
