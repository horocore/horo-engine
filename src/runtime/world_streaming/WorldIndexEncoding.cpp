#include "Horo/WorldStreaming/WorldIndexArtifact.h"
#include "WorldIndexWire.h"

#include <algorithm>
#include <ranges>

namespace Horo::WorldStreaming {
    namespace {
        using namespace Detail;

        /** @brief Owns normalized presentation strings in the sole canonical wire order. */
        Result<std::vector<std::string>> Names(const WorldPartitionDescriptor &descriptor, const std::string_view world,
                                               const WorldIndexArtifactLimits &limits) {
            if (descriptor.Layers().size() > limits.partition.maximumLayers || world.size() > limits.maximumStringBytes)
                return WorldIndexReject<std::vector<std::string>>(WorldArtifactErrors::CapacityExceeded);
            std::size_t submitted = world.size();
            for (const auto &layer : descriptor.Layers()) {
                if (layer.name.size() > limits.maximumStringBytes - submitted)
                    return WorldIndexReject<std::vector<std::string>>(WorldArtifactErrors::CapacityExceeded);
                submitted += layer.name.size();
            }
            std::vector<std::string> result;
            result.reserve(descriptor.Layers().size() + 1);
            std::size_t total{};
            for (std::size_t index = 0; index <= descriptor.Layers().size(); ++index) {
                const auto source = index ? std::string_view{descriptor.Layers()[index - 1].name} : world;
                auto normalized = WorldIndexName(source, limits.maximumStringBytes - total);
                if (normalized.HasError())
                    return Result<std::vector<std::string>>::Failure(normalized.ErrorValue());
                total += normalized.Value().size();
                if (index && total - result.front().size() > limits.partition.maximumLayerNameBytes)
                    return WorldIndexReject<std::vector<std::string>>(WorldArtifactErrors::CapacityExceeded);
                result.push_back(std::move(normalized).Value());
            }
            return Result<std::vector<std::string>>::Success(std::move(result));
        }

        /** @brief Encodes exact millimeter coordinates without introducing implicit precision loss. */
        Result<void> Coordinate(std::vector<std::byte> &bytes, const std::size_t offset, const Math::WorldCoordinate64 &coordinate) {
            const auto millimeters = coordinate.Millimeters();
            for (std::size_t axis = 0; axis < millimeters.size(); ++axis) {
                const auto meters = WorldIndexMeters(millimeters[axis]);
                if (meters.HasError())
                    return Result<void>::Failure(meters.ErrorValue());
                WorldIndexWrite(bytes, offset + axis * 4, std::bit_cast<std::uint32_t>(meters.Value()));
            }
            return Result<void>::Success();
        }

        /** @brief Fills all fixed header fields in a bounded zero-initialized artifact. */
        Result<void> Header(EncodedWorldIndex &result, const WorldPartitionDescriptor &descriptor, const WorldIndexLayout &layout,
                            const WorldIndexCoordinateSystem coordinates, const std::size_t worldNameBytes, const std::size_t volumes) {
            auto &bytes = result.bytes;
            WorldIndexWrite(bytes, 0, std::uint32_t{0x58444e49});
            WorldIndexWrite(bytes, 4, std::uint16_t{1});
            WorldIndexWrite(bytes, 12, std::uint32_t{WorldIndexHeaderBytes});
            for (std::size_t i = 0; i < 16; ++i)
                bytes[16 + i] = static_cast<std::byte>(descriptor.Partition().Bytes()[i]);
            WorldIndexWrite(bytes, 36, static_cast<std::uint32_t>(worldNameBytes));
            if (const auto minimum = Coordinate(bytes, 40, descriptor.Bounds().minimum); minimum.HasError())
                return minimum;
            if (const auto maximum = Coordinate(bytes, 52, descriptor.Bounds().maximum); maximum.HasError())
                return maximum;
            const auto cellSize = WorldIndexMeters(descriptor.Grid().BaseCellSizeMillimeters());
            if (cellSize.HasError())
                return Result<void>::Failure(cellSize.ErrorValue());
            WorldIndexWrite(bytes, 64, std::bit_cast<std::uint32_t>(cellSize.Value()));
            if (const auto origin = Coordinate(bytes, 68, descriptor.Grid().Origin()); origin.HasError())
                return origin;
            const auto bounds = descriptor.Grid().Bounds();
            const std::array axes{bounds.minimumX, bounds.maximumX, bounds.minimumY, bounds.maximumY, bounds.minimumZ, bounds.maximumZ};
            for (std::size_t i = 0; i < axes.size(); ++i)
                WorldIndexWrite(bytes, 80 + i * 4, axes[i]);
            bytes[104] = static_cast<std::byte>(coordinates);
            bytes[105] = static_cast<std::byte>(descriptor.Grid().LodLevels());
            WorldIndexWrite(bytes, 108, static_cast<std::uint32_t>(layout.layers));
            WorldIndexWrite(bytes, 112, static_cast<std::uint32_t>(descriptor.Layers().size()));
            WorldIndexWrite(bytes, 116, static_cast<std::uint32_t>(layout.volumes));
            WorldIndexWrite(bytes, 120, static_cast<std::uint32_t>(volumes));
            WorldIndexWrite(bytes, 124, static_cast<std::uint32_t>(layout.cells));
            WorldIndexWrite(bytes, 128, static_cast<std::uint32_t>(descriptor.Cells().size()));
            WorldIndexWrite(bytes, 132, static_cast<std::uint32_t>(layout.strings));
            WorldIndexWrite(bytes, 136, static_cast<std::uint32_t>(layout.total - layout.strings));
            WorldIndexWrite(bytes, 144, static_cast<std::uint64_t>(layout.total));
            return Result<void>::Success();
        }

        /** @brief Copies normalized names and fixed layer metadata without discretionary gaps. */
        Result<void> Layers(EncodedWorldIndex &result, const WorldPartitionDescriptor &descriptor, const WorldIndexLayout &layout,
                            const std::span<const std::string> names, const CancellationToken &cancellation) {
            auto &bytes = result.bytes;
            std::size_t position{};
            for (std::size_t index = 0; index < names.size(); ++index) {
                if (cancellation.IsCancellationRequested())
                    return WorldIndexReject<void>(WorldArtifactErrors::Cancelled);
                const auto &name = names[index];
                if (index) {
                    const auto &layer = descriptor.Layers()[index - 1];
                    const auto row = layout.layers + (index - 1) * WorldIndexLayerBytes;
                    WorldIndexWrite(bytes, row, layer.id.Value());
                    bytes[row + 2] = static_cast<std::byte>(layer.ownership);
                    WorldIndexWrite(bytes, row + 4, static_cast<std::uint32_t>(layer.flags));
                    WorldIndexWrite(bytes, row + 8, std::bit_cast<std::uint32_t>(layer.priorityMultiplier));
                    WorldIndexWrite(bytes, row + 12, static_cast<std::uint32_t>(position));
                    WorldIndexWrite(bytes, row + 16, static_cast<std::uint32_t>(name.size()));
                }
                for (std::size_t i = 0; i < name.size(); ++i)
                    bytes[layout.strings + position + i] = static_cast<std::byte>(static_cast<unsigned char>(name[i]));
                position += name.size();
            }
            return Result<void>::Success();
        }

        /** @brief Encodes the complete sorted static-volume table, preserving every version-one logical field. */
        Result<void> Volumes(EncodedWorldIndex &result, const WorldPartitionDescriptor &descriptor, const WorldIndexLayout &layout,
                             const std::span<const WorldIndexVolume> input, const CancellationToken &cancellation) {
            std::vector<WorldIndexVolume> volumes(input.begin(), input.end());
            std::ranges::sort(volumes, {}, &WorldIndexVolume::id);
            std::uint32_t previous{};
            for (std::size_t i = 0; i < volumes.size(); ++i) {
                if (cancellation.IsCancellationRequested())
                    return WorldIndexReject<void>(WorldArtifactErrors::Cancelled);
                const auto &volume = volumes[i];
                if (volume.id <= previous || !WorldIndexValidVolume(volume, descriptor.Layers().size()))
                    return WorldIndexReject<void>(WorldArtifactErrors::Invalid, "World static-volume metadata is invalid or duplicated.");
                previous = volume.id;
                const auto row = layout.volumes + i * WorldIndexVolumeBytes;
                WorldIndexWrite(result.bytes, row, volume.id);
                result.bytes[row + 4] = static_cast<std::byte>(volume.kind);
                result.bytes[row + 5] = static_cast<std::byte>(volume.shape);
                result.bytes[row + 6] = static_cast<std::byte>(volume.unloadOutside);
                for (std::size_t axis = 0; axis < 3; ++axis) {
                    WorldIndexWrite(result.bytes, row + 8 + axis * 4, std::bit_cast<std::uint32_t>(volume.origin[axis]));
                    WorldIndexWrite(result.bytes, row + 20 + axis * 4, std::bit_cast<std::uint32_t>(volume.extents[axis]));
                }
                WorldIndexWrite(result.bytes, row + 32, std::bit_cast<std::uint32_t>(volume.priority));
                WorldIndexWrite(result.bytes, row + 36, volume.layerMask);
            }
            return Result<void>::Success();
        }

        /** @brief Encodes exact existing canonical cells; hard edges deliberately remain in the bound sibling format. */
        Result<void> Cells(EncodedWorldIndex &result, const CookedWorldIndexManifest &manifest, const WorldIndexLayout &layout,
                           const CancellationToken &cancellation) {
            for (std::size_t i = 0; i < manifest.Cells().size(); ++i) {
                if (cancellation.IsCancellationRequested())
                    return WorldIndexReject<void>(WorldArtifactErrors::Cancelled);
                const auto &cell = manifest.Cells()[i];
                const auto &topology = manifest.Descriptor().Cells()[i];
                const auto row = layout.cells + i * WorldIndexCellBytes;
                WorldIndexWrite(result.bytes, row, cell.cell.x);
                WorldIndexWrite(result.bytes, row + 4, cell.cell.y);
                WorldIndexWrite(result.bytes, row + 8, cell.cell.z);
                result.bytes[row + 12] = static_cast<std::byte>(cell.cell.lod);
                WorldIndexWrite(result.bytes, row + 14, cell.cell.layer.Value());
                for (std::size_t byte = 0; byte < 16; ++byte)
                    result.bytes[row + 16 + byte] = static_cast<std::byte>(topology.package.chunkAsset.Bytes()[byte]);
                WorldIndexWrite(result.bytes, row + 32, cell.uncompressedSize);
                WorldIndexWrite(result.bytes, row + 40, cell.compressedSize);
                WorldIndexWrite(result.bytes, row + 48, cell.payloadCrc32);
                for (std::size_t byte = 0; byte < 32; ++byte)
                    result.bytes[row + 56 + byte] = static_cast<std::byte>(cell.artifactHash.bytes[byte]);
            }
            return Result<void>::Success();
        }

        /** @brief Seals the normative zero-field digest, then hashes the exact final artifact for sibling binding. */
        Result<void> Seal(EncodedWorldIndex &result, const CancellationToken &cancellation) {
            const auto manifestHash = WorldIndexDigest(result.bytes, true, cancellation);
            if (manifestHash.HasError())
                return Result<void>::Failure(manifestHash.ErrorValue());
            result.manifestHash = manifestHash.Value();
            for (std::size_t i = 0; i < result.manifestHash.bytes.size(); ++i)
                result.bytes[WorldIndexHashOffset + i] = static_cast<std::byte>(result.manifestHash.bytes[i]);
            const auto artifactHash = WorldIndexDigest(result.bytes, false, cancellation);
            if (artifactHash.HasError())
                return Result<void>::Failure(artifactHash.ErrorValue());
            result.artifactHash = artifactHash.Value();
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc EncodeWorldIndexArtifact */
    Result<EncodedWorldIndex> EncodeWorldIndexArtifact(const CookedWorldIndexManifest &manifest, const std::string_view name,
                                                       const WorldIndexCoordinateSystem coordinates,
                                                       const std::span<const WorldIndexVolume> volumes,
                                                       const WorldIndexArtifactLimits &limits, const CancellationToken &cancellation) {
        if (cancellation.IsCancellationRequested())
            return WorldIndexReject<EncodedWorldIndex>(WorldArtifactErrors::Cancelled);
        if (static_cast<unsigned>(coordinates) > 3)
            return WorldIndexReject<EncodedWorldIndex>(WorldArtifactErrors::Unsupported);
        const auto &descriptor = manifest.Descriptor();
        if (volumes.size() > limits.maximumVolumes || manifest.Cells().size() > limits.cooked.maximumCellEntries ||
            manifest.TotalCompressedBytes() > limits.cooked.maximumCompressedBytes ||
            manifest.TotalUncompressedBytes() > limits.cooked.maximumUncompressedBytes)
            return WorldIndexReject<EncodedWorldIndex>(WorldArtifactErrors::CapacityExceeded);
        auto names = Names(descriptor, name, limits);
        if (names.HasError())
            return Result<EncodedWorldIndex>::Failure(names.ErrorValue());
        std::size_t strings{};
        for (const auto &value : names.Value())
            strings += value.size();
        const auto layout = WorldIndexPackedLayout(descriptor.Layers().size(), volumes.size(), manifest.Cells().size(), strings, limits);
        if (layout.HasError())
            return Result<EncodedWorldIndex>::Failure(layout.ErrorValue());
        EncodedWorldIndex result{.bytes = std::vector<std::byte>(layout.Value().total)};
        if (const auto header = Header(result, descriptor, layout.Value(), coordinates, names.Value().front().size(), volumes.size());
            header.HasError())
            return Result<EncodedWorldIndex>::Failure(header.ErrorValue());
        if (const auto layers = Layers(result, descriptor, layout.Value(), names.Value(), cancellation); layers.HasError())
            return Result<EncodedWorldIndex>::Failure(layers.ErrorValue());
        if (const auto volume = Volumes(result, descriptor, layout.Value(), volumes, cancellation); volume.HasError())
            return Result<EncodedWorldIndex>::Failure(volume.ErrorValue());
        if (const auto cells = Cells(result, manifest, layout.Value(), cancellation); cells.HasError())
            return Result<EncodedWorldIndex>::Failure(cells.ErrorValue());
        if (const auto sealed = Seal(result, cancellation); sealed.HasError())
            return Result<EncodedWorldIndex>::Failure(sealed.ErrorValue());
        return Result<EncodedWorldIndex>::Success(std::move(result));
    }
}  // namespace Horo::WorldStreaming
