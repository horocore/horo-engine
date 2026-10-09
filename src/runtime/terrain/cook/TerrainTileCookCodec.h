#pragma once

/** @file TerrainTileCookCodec.h @brief Target-private canonical Terrain cook byte writer. */

#include "Horo/Terrain/TerrainTileCook.h"

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace Horo::Terrain::Detail {
    /** @brief Emits v1 little-endian fields to owned bytes or buffered SHA-256 state. */
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

    private:
        std::vector<std::uint8_t> *bytes_{};
        Sha256Builder *hash_{};
        std::array<std::byte, 4096> pending_{};
        std::size_t pendingSize_{};
        bool valid_{true};
    };

    inline void WriteCoordinates(CanonicalWriter &writer, const TerrainSourceCoordinates &coordinates) {
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

    inline void WriteProfile(CanonicalWriter &writer, const TerrainTileCookProfile &profile) {
        writer.Unsigned(profile.interiorQuads, 4);
        writer.Byte(profile.lodLevels);
        writer.Byte(static_cast<std::uint8_t>(profile.tier));
        writer.Bytes(profile.targetDigest.bytes);
        writer.Bytes(profile.toolchainDigest.bytes);
        writer.Unsigned(profile.maximumTiles, 4);
        writer.Unsigned(profile.maximumPayloadBytes, 8);
        writer.Unsigned(profile.maximumWorkItems, 8);
    }

    [[nodiscard]] bool Nonzero(const Sha256Digest &digest);
    void WriteSample(CanonicalWriter &writer, const TerrainCanonicalSource &source, std::size_t x, std::size_t z);
    [[nodiscard]] Sha256Digest SourceDigest(const TerrainCanonicalSource &source);
    [[nodiscard]] std::vector<std::uint32_t> SamplePositions(std::uint32_t begin, std::uint32_t end, std::uint32_t stride);
    [[nodiscard]] std::optional<std::int32_t> WorldTileOrigin(double origin, double spacing, std::uint32_t tileQuads,
                                                              std::uint64_t tileCount);
    [[nodiscard]] Sha256Digest EdgeDigest(const TerrainCanonicalSource &source, const std::vector<std::uint32_t> &xs,
                                          const std::vector<std::uint32_t> &zs, std::uint32_t stride, bool alongZ, bool highEdge);
    [[nodiscard]] bool ValidCoordinates(const TerrainSourceCoordinates &coordinates);
    [[nodiscard]] bool ValidProfile(const TerrainTileCookProfile &profile);
    [[nodiscard]] Sha256Digest Fingerprint(const TerrainCanonicalSource &source, const Sha256Digest &sourceDigest,
                                           const TerrainTileCookProfile &profile,
                                           const std::vector<TerrainTileCookDependency> &dependencies);
    [[nodiscard]] Sha256Digest ManifestDigest(const CookedTerrainTileSet &cooked);
    /** @brief Hashes borrowed bytes in cooperative 4096-byte chunks, without allocation. */
    [[nodiscard]] Result<Sha256Digest> HashPayload(std::span<const std::uint8_t> bytes, const CancellationToken &cancellation);
}  // namespace Horo::Terrain::Detail
