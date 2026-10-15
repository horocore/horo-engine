#pragma once

#include "Horo/WorldStreaming/WorldArtifactErrors.h"
#include "Horo/WorldStreaming/WorldIndexArtifact.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <utf8proc.h>

namespace Horo::WorldStreaming::Detail {
    inline constexpr std::size_t WorldIndexHeaderBytes = 192;
    inline constexpr std::size_t WorldIndexHashOffset = 152;
    inline constexpr std::size_t WorldIndexLayerBytes = 32;
    inline constexpr std::size_t WorldIndexVolumeBytes = 64;
    inline constexpr std::size_t WorldIndexCellBytes = 96;

    /** @brief Retains a typed artifact failure without a partially exposed projection. */
    template <typename T> Result<T> WorldIndexReject(const ErrorCodeDescriptor &error, std::string message = {}) {
        return Result<T>::Failure(MakeError(error, std::move(message)));
    }

    /** @brief Reads a little-endian scalar only after the owning packed-section validation proved its range. */
    template <typename T> T WorldIndexRead(const std::span<const std::byte> bytes, const std::size_t offset) noexcept {
        std::uint64_t value{};
        for (std::size_t index = 0; index < sizeof(T); ++index)
            value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(bytes[offset + index])) << (index * 8U);
        using U = std::make_unsigned_t<T>;
        return std::bit_cast<T>(static_cast<U>(value));
    }

    /** @brief Writes one fixed-width scalar into a previously admitted zero-filled record. */
    template <typename T> void WorldIndexWrite(std::vector<std::byte> &bytes, const std::size_t offset, const T value) noexcept {
        using U = std::make_unsigned_t<T>;
        const auto bits = std::bit_cast<U>(value);
        for (std::size_t index = 0; index < sizeof(T); ++index)
            bytes[offset + index] = static_cast<std::byte>((static_cast<std::uint64_t>(bits) >> (index * 8U)) & 0xffU);
    }

    /** @brief Tests canonical finite floating-point representation, including positive zero. */
    inline bool WorldIndexCanonicalFloat(const float value) noexcept {
        return std::isfinite(value) && (value != 0.0F || !std::signbit(value));
    }

    /** @brief Rejects submillimeter or range-losing float coordinates before building an exact global coordinate. */
    inline Result<std::int64_t> WorldIndexMillimeters(const float meters) {
        const long double scaled = static_cast<long double>(meters) * 1000.0L;
        const long double rounded = std::round(scaled);
        const long double exclusiveMaximum = std::ldexp(1.0L, 63);
        if (!WorldIndexCanonicalFloat(meters) || rounded < -exclusiveMaximum || rounded >= exclusiveMaximum)
            return WorldIndexReject<std::int64_t>(WorldArtifactErrors::Invalid, "World coordinate cannot be represented in millimeters.");
        const auto millimeters = static_cast<std::int64_t>(rounded);
        const float roundTrip = static_cast<float>(static_cast<double>(millimeters) / 1000.0);
        if (roundTrip != meters)
            return WorldIndexReject<std::int64_t>(WorldArtifactErrors::Invalid, "World coordinate loses canonical millimeter precision.");
        return Result<std::int64_t>::Success(millimeters);
    }

    /** @brief Admits an exact millimeter value to the existing version-one float-meter wire representation. */
    inline Result<float> WorldIndexMeters(const std::int64_t millimeters) {
        const float meters = static_cast<float>(static_cast<double>(millimeters) / 1000.0);
        const auto decoded = WorldIndexMillimeters(meters);
        if (decoded.HasError() || decoded.Value() != millimeters)
            return WorldIndexReject<float>(WorldArtifactErrors::Unsupported,
                                           "Version-one world.index cannot represent this coordinate exactly.");
        return Result<float>::Success(meters);
    }

    /** @brief Normalizes bounded UTF-8 names with the repository's existing pinned Unicode implementation. */
    inline Result<std::string> WorldIndexName(const std::string_view text, const std::size_t maximumBytes) {
        if (text.empty() || text.find('\0') != std::string_view::npos)
            return WorldIndexReject<std::string>(WorldArtifactErrors::Invalid, "World and layer names must be nonempty UTF-8 without NUL.");
        if (text.size() > maximumBytes || text.size() > static_cast<std::size_t>(std::numeric_limits<utf8proc_ssize_t>::max()))
            return WorldIndexReject<std::string>(WorldArtifactErrors::CapacityExceeded);
        constexpr auto options = static_cast<utf8proc_option_t>(UTF8PROC_STABLE | UTF8PROC_COMPOSE | UTF8PROC_REJECTNA);
        const auto *input = reinterpret_cast<const utf8proc_uint8_t *>(text.data());
        const auto length = static_cast<utf8proc_ssize_t>(text.size());
        const auto count = utf8proc_decompose(input, length, nullptr, 0, options);
        if (count < 0)
            return WorldIndexReject<std::string>(WorldArtifactErrors::Invalid, "World name is not a supported UTF-8 scalar sequence.");
        if (static_cast<std::size_t>(count) > maximumBytes ||
            static_cast<std::size_t>(count) >= std::numeric_limits<std::size_t>::max() / sizeof(utf8proc_int32_t))
            return WorldIndexReject<std::string>(WorldArtifactErrors::CapacityExceeded);
        std::vector<utf8proc_int32_t> buffer(static_cast<std::size_t>(count) + 1);
        const auto decoded = utf8proc_decompose(input, length, buffer.data(), count, options);
        if (decoded < 0)
            return WorldIndexReject<std::string>(WorldArtifactErrors::Invalid);
        const auto size = utf8proc_reencode(buffer.data(), decoded, options);
        if (size < 0)
            return WorldIndexReject<std::string>(WorldArtifactErrors::Invalid);
        if (static_cast<std::size_t>(size) > maximumBytes)
            return WorldIndexReject<std::string>(WorldArtifactErrors::CapacityExceeded);
        return Result<std::string>::Success(std::string{reinterpret_cast<const char *>(buffer.data()), static_cast<std::size_t>(size)});
    }

    /** @brief Hashes bounded pieces while zeroing only the exact embedded digest field when requested. */
    inline Result<Sha256Digest> WorldIndexDigest(const std::span<const std::byte> bytes, const bool zeroHash,
                                                 const CancellationToken &cancellation) {
        Sha256Builder builder;
        const std::array<std::byte, 32> zero{};
        for (std::size_t offset = 0; offset < bytes.size();) {
            if (cancellation.IsCancellationRequested())
                return WorldIndexReject<Sha256Digest>(WorldArtifactErrors::Cancelled);
            const bool hashField = zeroHash && offset == WorldIndexHashOffset;
            const auto boundary = zeroHash && offset < WorldIndexHashOffset ? WorldIndexHashOffset : bytes.size();
            const auto count = hashField ? zero.size() : std::min<std::size_t>(65536, boundary - offset);
            const auto piece = hashField ? std::span<const std::byte>{zero} : bytes.subspan(offset, count);
            if (!builder.Update(piece))
                return WorldIndexReject<Sha256Digest>(WorldArtifactErrors::CapacityExceeded);
            offset += count;
        }
        return Result<Sha256Digest>::Success(builder.Finalize());
    }

    /** @brief One admitted exact packed layout; all record widths remain the ADR-023 version-one values. */
    struct WorldIndexLayout final {
        std::size_t layers{};
        std::size_t volumes{};
        std::size_t cells{};
        std::size_t strings{};
        std::size_t total{};
    };

    /** @brief Computes all table offsets with checked aggregate admission before allocating any table storage. */
    inline Result<WorldIndexLayout> WorldIndexPackedLayout(const std::size_t layers, const std::size_t volumes, const std::size_t cells,
                                                           const std::size_t strings, const WorldIndexArtifactLimits &limits) {
        if (!limits.maximumArtifactBytes || !limits.maximumStringBytes || !limits.partition.maximumLayers ||
            !limits.partition.maximumCells || !limits.partition.maximumLayerNameBytes || !layers || !cells || !strings)
            return WorldIndexReject<WorldIndexLayout>(WorldArtifactErrors::Invalid);
        const auto maximum = std::min<std::size_t>(limits.maximumArtifactBytes, std::numeric_limits<std::uint32_t>::max());
        if (layers > limits.partition.maximumLayers || volumes > limits.maximumVolumes || cells > limits.partition.maximumCells ||
            cells > limits.cooked.maximumCellEntries || strings > limits.maximumStringBytes || maximum < WorldIndexHeaderBytes)
            return WorldIndexReject<WorldIndexLayout>(WorldArtifactErrors::CapacityExceeded);
        WorldIndexLayout result{.layers = WorldIndexHeaderBytes};
        std::size_t position = WorldIndexHeaderBytes;
        const std::array counts{layers, volumes, cells, strings};
        const std::array<std::size_t, 4> strides{WorldIndexLayerBytes, WorldIndexVolumeBytes, WorldIndexCellBytes, 1};
        std::array<std::size_t *, 4> ends{&result.volumes, &result.cells, &result.strings, &result.total};
        for (std::size_t i = 0; i < counts.size(); ++i) {
            if (counts[i] > (maximum - position) / strides[i])
                return WorldIndexReject<WorldIndexLayout>(WorldArtifactErrors::CapacityExceeded);
            position += counts[i] * strides[i];
            *ends[i] = position;
        }
        return Result<WorldIndexLayout>::Success(result);
    }

    /** @brief Validates one inert static volume against canonical layer ordinals and version-one finite semantics. */
    inline bool WorldIndexValidVolume(const WorldIndexVolume &volume, const std::size_t layers) noexcept {
        if (!volume.id || static_cast<unsigned>(volume.kind) > 3 || static_cast<unsigned>(volume.shape) > 2 ||
            !WorldIndexCanonicalFloat(volume.priority) || volume.priority < 0 || !volume.layerMask ||
            (layers < 32 && (volume.layerMask >> layers) != 0))
            return false;
        for (std::size_t i = 0; i < 3; ++i)
            if (!WorldIndexCanonicalFloat(volume.origin[i]) || !WorldIndexCanonicalFloat(volume.extents[i]) || volume.extents[i] < 0)
                return false;
        return volume.extents[0] > 0;
    }
}  // namespace Horo::WorldStreaming::Detail
