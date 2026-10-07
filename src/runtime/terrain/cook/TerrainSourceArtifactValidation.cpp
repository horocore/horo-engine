#include "Horo/Terrain/TerrainSourceArtifacts.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <string_view>

namespace Horo::Terrain {
    namespace {
        std::uint64_t Read(const std::span<const std::uint8_t> bytes, const std::size_t offset, const std::uint8_t length = 8) {
            std::uint64_t value{};
            for (std::uint8_t byte = 0; byte < length; ++byte)
                value |= static_cast<std::uint64_t>(bytes[offset + byte]) << (byte * 8U);
            return value;
        }

        double Double(const std::span<const std::uint8_t> bytes, const std::size_t offset) {
            return std::bit_cast<double>(Read(bytes, offset));
        }

        bool Nonzero(const std::span<const std::uint8_t> bytes) {
            return std::ranges::any_of(bytes, [](const auto byte) {
                return byte != 0;
            });
        }

        /** @brief Projected artifacts preserve an explicit canonical EPSG identifier, never a geographic fallback. */
        bool ValidProjectedCrs(const std::span<const std::uint8_t> crs) {
            constexpr std::string_view prefix = "EPSG:";
            if (crs.size() <= prefix.size() || !std::equal(prefix.begin(), prefix.end(), crs.begin()))
                return false;
            for (std::size_t index = prefix.size(); index < crs.size(); ++index)
                if (crs[index] < '0' || crs[index] > '9')
                    return false;
            return true;
        }

        /** @brief Validate the coordinate-system identifier independently of its numeric transform. */
        bool ValidCoordinateSystem(const std::span<const std::uint8_t> bytes, const std::size_t length) {
            switch (static_cast<TerrainCoordinateSpace>(bytes[196])) {
                case TerrainCoordinateSpace::LocalMeters:
                    return length == 0;
                case TerrainCoordinateSpace::ProjectedMeters:
                    return ValidProjectedCrs(bytes.subspan(199, length));
                default:
                    return false;
            }
        }

        bool ValidCoordinates(const std::span<const std::uint8_t> bytes, const std::size_t length) {
            if (!ValidCoordinateSystem(bytes, length))
                return false;
            const auto start = 199 + length;
            for (std::size_t index = 0; index < 7; ++index)
                if (!std::isfinite(Double(bytes, start + index * 8)))
                    return false;
            return Double(bytes, start + 16) > 0 && Double(bytes, start + 24) > 0 && Double(bytes, start + 32) > 0 &&
                   Double(bytes, start + 48) >= 0;
        }

        /** @brief Validate all retained positions before any triangle dereferences them. */
        bool ValidVertices(const std::span<const std::uint8_t> bytes, const std::size_t start, const std::uint64_t vertices) {
            for (std::size_t vertex = 0; vertex < vertices; ++vertex)
                for (std::size_t axis = 0; axis < 3; ++axis)
                    if (!std::isfinite(Double(bytes, start + vertex * 24 + axis * 8)))
                        return false;
            return true;
        }

        /** @brief An upward finite triangle must cover a nonempty canonical source-grid rectangle. */
        bool ValidTriangle(const std::span<const std::uint8_t> bytes, const std::size_t start, const std::uint64_t vertices,
                           const std::size_t offset) {
            std::array<std::size_t, 3> indices{};
            for (std::size_t corner = 0; corner < 3; ++corner) {
                indices[corner] = Read(bytes, offset + corner * 4, 4);
                if (indices[corner] >= vertices)
                    return false;
            }
            const auto a = start + indices[0] * 24;
            const auto b = start + indices[1] * 24;
            const auto c = start + indices[2] * 24;
            const auto area = (Double(bytes, b + 16) - Double(bytes, a + 16)) * (Double(bytes, c) - Double(bytes, a)) -
                              (Double(bytes, b) - Double(bytes, a)) * (Double(bytes, c + 16) - Double(bytes, a + 16));
            if (!std::isfinite(area) || area <= 0 || Read(bytes, offset + 12, 4) >= Read(bytes, offset + 16, 4) ||
                Read(bytes, offset + 20, 4) >= Read(bytes, offset + 24, 4) ||
                Read(bytes, offset + 16, 4) >= TerrainDescriptorHardLimits::SamplesPerAxis ||
                Read(bytes, offset + 24, 4) >= TerrainDescriptorHardLimits::SamplesPerAxis)
                return false;
            return true;
        }

        bool ValidGeometry(const std::span<const std::uint8_t> bytes, const std::size_t start, const std::uint64_t vertices,
                           const std::uint64_t triangles) {
            if (!ValidVertices(bytes, start, vertices))
                return false;
            const auto triangleStart = start + vertices * 24;
            for (std::size_t triangle = 0; triangle < triangles; ++triangle) {
                if (!ValidTriangle(bytes, start, vertices, triangleStart + triangle * 28))
                    return false;
            }
            return true;
        }

        /** @brief The fixed header is read only after minimum size, schema and digest admission. */
        bool ValidProvenance(const std::span<const std::uint8_t> payload) {
            if (payload[35] > static_cast<std::uint8_t>(TerrainSourceArtifactRole::Navigation) || Read(payload, 36) == 0 ||
                Read(payload, 60) == 0 || !Nonzero(payload.subspan(44, 16)))
                return false;
            SerializedTerrainTileId tile{};
            std::copy_n(payload.begin() + 10, tile.size(), tile.begin());
            if (DeserializeTerrainTileId(tile).HasError())
                return false;
            for (std::size_t offset = 68; offset < 196; offset += 32)
                if (!Nonzero(payload.subspan(offset, 32)))
                    return false;
            return true;
        }

        /** @brief Bound counts before multiplication and require an exact, fully consumed geometry span. */
        bool ValidGeometryEnvelope(const std::span<const std::uint8_t> payload, const std::size_t crsLength) {
            const auto error = Double(payload, 383 + crsLength);
            const auto vertices = Read(payload, 392 + crsLength);
            const auto triangles = Read(payload, 400 + crsLength);
            const auto geometryStart = 408 + crsLength;
            if (!std::isfinite(error) || error < 0 || payload[391 + crsLength] != 1 || vertices < 4 ||
                vertices > TerrainDescriptorHardLimits::WorkItems || triangles > TerrainDescriptorHardLimits::WorkItems * 2 ||
                vertices * 24 + triangles * 28 != payload.size() - geometryStart)
                return false;
            return ValidGeometry(payload, geometryStart, vertices, triangles);
        }
    }  // namespace

    /** @copydoc VerifyTerrainSourceArtifactPayload */
    Result<void> VerifyTerrainSourceArtifactPayload(const std::span<const std::uint8_t> payload, const Sha256Digest &expectedDigest) {
        const auto corrupt = [] {
            return Result<void>::Failure(MakeError(TerrainTileCookErrors::CorruptPrevious));
        };
        constexpr std::array<std::uint8_t, 10> prefix{4, 0, 'H', 'T', 'S', 'G', 1, 0, 0, 0};
        if (payload.size() < 408 || payload.size() > TerrainDescriptorHardLimits::StagingBytes ||
            !std::equal(prefix.begin(), prefix.end(), payload.begin()) || ComputeSha256(std::as_bytes(payload)) != expectedDigest ||
            !ValidProvenance(payload))
            return corrupt();
        const auto crsLength = Read(payload, 197, 2);
        if (crsLength > payload.size() - 408 || !ValidCoordinates(payload, crsLength))
            return corrupt();
        if (!ValidGeometryEnvelope(payload, crsLength))
            return corrupt();
        return Result<void>::Success();
    }
}  // namespace Horo::Terrain
