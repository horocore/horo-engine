#pragma once

#include "WorldDependencyArtifactModel.h"
#include "WorldIndexWire.h"

#include <optional>

namespace Horo::WorldStreaming::Detail {
    inline constexpr std::size_t DependencyHeaderBytes = 192;
    inline constexpr std::size_t DependencyHashOffset = 144;
    inline constexpr std::array<std::size_t, 8> DependencyRowBytes{64, 16, 16, 8, 32, 64, 24, 16};

    /** @brief Admitted packed table layout; every offset addresses owned bytes, not native structure layout. */
    struct DependencyLayout final {
        std::array<std::size_t, 8> counts{};
        std::array<std::size_t, 8> offsets{};
        std::size_t total{};
    };

    /** @brief Bounds decoded table storage plus the independent closure verification scratch before allocating it. */
    inline std::optional<std::size_t> DependencyAllocationRemaining(const std::array<std::size_t, 8> &counts, const std::size_t maximum) {
        const std::array<std::size_t, 8> strides{sizeof(WorldDependencyObjectProjection) + sizeof(std::size_t),
                                                 sizeof(StreamingCellId),
                                                 2 * sizeof(StreamingCellId),
                                                 sizeof(std::vector<WorldDependencyEndpoint>),
                                                 sizeof(WorldDependencyEndpoint),
                                                 sizeof(WorldDependencyCandidate),
                                                 2 * sizeof(WorldDependencyCellProjection),
                                                 2 * sizeof(StreamingCellId)};
        std::size_t total{};
        for (std::size_t table = 0; table < counts.size(); ++table) {
            if (counts[table] > (maximum - total) / strides[table])
                return std::nullopt;
            total += counts[table] * strides[table];
        }
        return maximum - total;
    }

    /** @brief Checks every count and multiplication before deriving any byte offset or allocating a native table. */
    inline Result<DependencyLayout> AdmitDependencyLayout(const std::array<std::size_t, 8> &counts,
                                                          const WorldDependencyArtifactLimits &limits) {
        if (!limits.maximumArtifactBytes || !limits.maximumDecodedAllocationBytes || !limits.maximumObjects || !limits.maximumCells ||
            !limits.maximumCoverageCells || !limits.maximumPlacementCells || !limits.maximumBundleMembers ||
            !limits.maximumSoftReferences || !limits.maximumHardCellEdges || !limits.maximumDerivationSteps || !counts[0] || !counts[6])
            return WorldIndexReject<DependencyLayout>(WorldArtifactErrors::Invalid);
        const std::array<std::size_t, 8> ceilings{limits.maximumObjects,        limits.maximumCoverageCells,
                                                  limits.maximumPlacementCells, limits.maximumBundleMembers / 2,
                                                  limits.maximumBundleMembers,  limits.maximumSoftReferences,
                                                  limits.maximumCells,          limits.maximumHardCellEdges};
        const auto maximum = std::min<std::size_t>(limits.maximumArtifactBytes, std::numeric_limits<std::uint32_t>::max());
        if (maximum < DependencyHeaderBytes || !DependencyAllocationRemaining(counts, limits.maximumDecodedAllocationBytes).has_value())
            return WorldIndexReject<DependencyLayout>(WorldArtifactErrors::CapacityExceeded);
        DependencyLayout layout{.counts = counts, .total = DependencyHeaderBytes};
        for (std::size_t table = 0; table < counts.size(); ++table) {
            if (counts[table] > ceilings[table] || counts[table] > (maximum - layout.total) / DependencyRowBytes[table])
                return WorldIndexReject<DependencyLayout>(WorldArtifactErrors::CapacityExceeded);
            layout.offsets[table] = layout.total;
            layout.total += counts[table] * DependencyRowBytes[table];
        }
        return Result<DependencyLayout>::Success(layout);
    }

    /** @brief Hashes the exact sibling while zeroing only its own fixed embedded digest field. */
    inline Result<Sha256Digest> DependencyDigest(const std::span<const std::byte> bytes, const CancellationToken &cancellation) {
        Sha256Builder builder;
        const std::array<std::byte, 32> zero{};
        for (std::size_t offset = 0; offset < bytes.size();) {
            if (cancellation.IsCancellationRequested())
                return WorldIndexReject<Sha256Digest>(WorldArtifactErrors::Cancelled);
            const bool embedded = offset == DependencyHashOffset;
            const auto boundary = offset < DependencyHashOffset ? DependencyHashOffset : bytes.size();
            const auto size = embedded ? zero.size() : std::min<std::size_t>(65536, boundary - offset);
            if (!builder.Update(embedded ? std::span<const std::byte>{zero} : bytes.subspan(offset, size)))
                return WorldIndexReject<Sha256Digest>(WorldArtifactErrors::CapacityExceeded);
            offset += size;
        }
        return Result<Sha256Digest>::Success(builder.Finalize());
    }

    /** @brief Copies an existing canonical cell tuple and leaves its one-byte record padding zero. */
    inline void WriteDependencyCell(std::vector<std::byte> &bytes, const std::size_t offset, const StreamingCellId &cell) {
        const auto encoded = SerializeStreamingCellId(cell);
        for (std::size_t position = 0; position < encoded.size(); ++position)
            bytes[offset + position] = static_cast<std::byte>(encoded[position]);
    }

    /** @brief Reuses the sole typed cell identity decoder after checking this sibling's extra padding byte. */
    inline Result<StreamingCellId> ReadDependencyCell(const std::span<const std::byte> bytes, const std::size_t offset) {
        if (bytes[offset + 15] != std::byte{})
            return WorldIndexReject<StreamingCellId>(WorldArtifactErrors::Invalid);
        SerializedStreamingCellId value{};
        for (std::size_t position = 0; position < value.size(); ++position)
            value[position] = std::to_integer<std::uint8_t>(bytes[offset + position]);
        return DeserializeStreamingCellId(value);
    }

    /** @brief Encodes the existing exact authored endpoint representation without hashing or truncating identity. */
    inline void WriteDependencyEndpoint(std::vector<std::byte> &bytes, const std::size_t offset, const WorldDependencyEndpoint &endpoint) {
        for (std::size_t position = 0; position < 16; ++position)
            bytes[offset + position] = static_cast<std::byte>(endpoint.address.page.Bytes()[position]);
        WorldIndexWrite(bytes, offset + 16, endpoint.address.object);
        WorldIndexWrite(bytes, offset + 24, endpoint.revision.Value());
    }

    /** @brief Restores only validated durable endpoint fields and preserves the original revision factory error. */
    inline Result<WorldDependencyEndpoint> ReadDependencyEndpoint(const std::span<const std::byte> bytes, const std::size_t offset) {
        std::array<std::uint8_t, 16> page{};
        for (std::size_t position = 0; position < page.size(); ++position)
            page[position] = std::to_integer<std::uint8_t>(bytes[offset + position]);
        auto revision = WorldAuthoringRevision::Create(WorldIndexRead<std::uint64_t>(bytes, offset + 24));
        if (revision.HasError())
            return Result<WorldDependencyEndpoint>::Failure(revision.ErrorValue());
        WorldDependencyEndpoint result{{Assets::AssetId::FromBytes(page), WorldIndexRead<std::uint64_t>(bytes, offset + 16)},
                                       revision.Value()};
        if (!result.IsValid())
            return WorldIndexReject<WorldDependencyEndpoint>(WorldArtifactErrors::Invalid);
        return Result<WorldDependencyEndpoint>::Success(result);
    }

    /** @brief Requires the exact next packed slice, preventing aliases, gaps and offset-plus-count overflow. */
    inline bool TakeDependencySlice(const std::uint32_t offset, const std::uint32_t count, const std::size_t total,
                                    std::size_t &next) noexcept {
        if (next > total || offset != next || count > total - next)
            return false;
        next += count;
        return true;
    }
}  // namespace Horo::WorldStreaming::Detail
