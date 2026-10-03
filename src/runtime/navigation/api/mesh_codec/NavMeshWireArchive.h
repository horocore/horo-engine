#pragma once

#include "Horo/Navigation/NavMeshData.h"

#include <bit>
#include <type_traits>

namespace Horo::Navigation::MeshWire {
    /** @brief Bounded little-endian scalar writer; no native layout or padding enters the wire. */
    class Writer final {
    public:
        explicit Writer(const std::size_t maximum) : maximum_(maximum) {}

        template <class T> void operator()(const T value) {
            using Unsigned = std::make_unsigned_t<T>;
            const auto bits = std::bit_cast<Unsigned>(value);
            if (sizeof(T) > maximum_ - bytes.size()) {
                valid = false;
                return;
            }
            for (std::size_t index = 0; index < sizeof(T); ++index) {
                bytes.push_back(static_cast<std::byte>((bits >> (index * 8)) & 0xffU));
            }
        }

        void Raw(const std::span<const std::byte> input) {
            if (input.size() > maximum_ - bytes.size()) {
                valid = false;
                return;
            }
            bytes.insert(bytes.end(), input.begin(), input.end());
        }

        std::vector<std::byte> bytes;
        bool valid{true};

        std::size_t maximum_{};
    };

    /** @brief Reader never advances past its exact input; failure is sticky through field traversal. */
    class Reader final {
    public:
        explicit Reader(const std::span<const std::byte> input) : input_(input) {}

        template <class T> void operator()(T &value) {
            using Unsigned = std::make_unsigned_t<T>;
            if (sizeof(T) > Remaining()) {
                valid = false;
                return;
            }
            Unsigned bits{};
            for (std::size_t index = 0; index < sizeof(T); ++index) {
                bits |= static_cast<Unsigned>(std::to_integer<std::uint8_t>(input_[position++])) << (index * 8);
            }
            value = std::bit_cast<T>(bits);
        }

        [[nodiscard]] std::size_t Remaining() const noexcept {
            return input_.size() - position;
        }

        [[nodiscard]] std::span<const std::byte> Raw(const std::size_t count) noexcept {
            if (count > Remaining()) {
                valid = false;
                return {};
            }
            const auto result = input_.subspan(position, count);
            position += count;
            return result;
        }

        std::size_t position{};
        bool valid{true};

        std::span<const std::byte> input_;
    };

    /** @brief Explicit scalar transformation shared by encoding and decoding field traversal. */
    template <class Archive, class T> void Scalar(Archive &archive, T &value) {
        if constexpr (std::is_enum_v<T>) {
            auto bits = static_cast<std::underlying_type_t<T>>(value);
            archive(bits);
            value = static_cast<T>(bits);
        } else if constexpr (std::is_same_v<T, float>) {
            auto bits = std::bit_cast<std::uint32_t>(value);
            archive(bits);
            value = std::bit_cast<float>(bits);
        } else {
            archive(value);
        }
    }

    /** @brief Stable identities are persisted by value, never by runtime handle layout. */
    template <class Archive, class Identity> void IdentityField(Archive &archive, Identity &identity) {
        auto value = identity.Value();
        archive(value);
        const auto parsed = Identity::Create(value);
        if (parsed.HasError()) {
            archive.valid = false;
        } else {
            identity = parsed.Value();
        }
    }

    template <class Archive> void Fields(Archive &archive, Sha256Digest &digest) {
        for (auto &byte : digest.bytes)
            archive(byte);
    }

    template <class Archive> void Fields(Archive &archive, Math::Vec3 &point) {
        Scalar(archive, point.x);
        Scalar(archive, point.y);
        Scalar(archive, point.z);
    }

    template <class Archive> void Fields(Archive &archive, Math::Aabb &bounds) {
        Fields(archive, bounds.minimum);
        Fields(archive, bounds.maximum);
    }

    template <class Archive> void Fields(Archive &archive, NavMeshTableRange &range) {
        archive(range.first);
        archive(range.count);
    }

    template <class Archive> void Fields(Archive &archive, NavMeshTileDescriptor &tile) {
        archive(tile.key.x);
        archive(tile.key.z);
        archive(tile.key.layer);
        Fields(archive, tile.bounds);
        Fields(archive, tile.vertices);
        Fields(archive, tile.polygons);
        Fields(archive, tile.polygonVertexIndices);
        Fields(archive, tile.polygonAdjacencies);
        Fields(archive, tile.offMeshLinks);
        Fields(archive, tile.provenance);
        Fields(archive, tile.providerPayloads);
        Fields(archive, tile.payloadDigest);
    }

    template <class Archive> void Fields(Archive &archive, NavigationAgentBuildGeometry &geometry) {
        Scalar(archive, geometry.radiusMeters);
        Scalar(archive, geometry.heightMeters);
        Scalar(archive, geometry.maxSlopeDegrees);
        Scalar(archive, geometry.stepHeightMeters);
        Scalar(archive, geometry.cellSizeMeters);
        Scalar(archive, geometry.cellHeightMeters);
        Scalar(archive, geometry.minimumRegionSizeMeters);
    }

    template <class Archive> void Fields(Archive &archive, NavMeshArtifactHeader &header) {
        archive(header.formatVersion.major);
        archive(header.formatVersion.minor);
        Scalar(archive, header.byteOrder);
        Scalar(archive, header.compression);
        Scalar(archive, header.coordinateFrame.representation);
        auto millimeters = header.coordinateFrame.origin.Millimeters();
        for (auto &axis : millimeters)
            archive(axis);
        header.coordinateFrame.origin = Math::WorldCoordinate64::FromMillimeters(millimeters[0], millimeters[1], millimeters[2]);
        Scalar(archive, header.coordinateFrame.tileSizeMeters);
        IdentityField(archive, header.profile.id);
        Fields(archive, header.profile.buildGeometry);
        Fields(archive, header.profile.contentDigest);
        archive(header.tileCount);
        archive(header.vertexCount);
        archive(header.polygonCount);
        archive(header.polygonVertexIndexCount);
        archive(header.polygonAdjacencyCount);
        archive(header.offMeshLinkCount);
        archive(header.provenanceCount);
        archive(header.providerPayloadCount);
        archive(header.portableEncodedBytes);
        archive(header.portableDecodedBytes);
        archive(header.providerEncodedBytes);
        archive(header.providerDecodedBytes);
        Fields(archive, header.payloadDigest);
    }

    template <class Archive> void Fields(Archive &archive, NavMeshPolygon &polygon) {
        Fields(archive, polygon.vertexIndices);
        Fields(archive, polygon.adjacencies);
        IdentityField(archive, polygon.area);
    }

    template <class Archive> void Fields(Archive &archive, NavMeshOffMeshLink &link) {
        Fields(archive, link.start);
        Fields(archive, link.end);
        Scalar(archive, link.radiusMeters);
        archive(link.startPolygon);
        archive(link.endPolygon);
        IdentityField(archive, link.area);
        auto direction = static_cast<std::uint8_t>(link.bidirectional);
        archive(direction);
        archive.valid = archive.valid && direction <= 1;
        link.bidirectional = direction == 1;
    }

    template <class Archive> void Fields(Archive &archive, NavMeshSourceProvenance &row) {
        Scalar(archive, row.kind);
        IdentityField(archive, row.producer);
        IdentityField(archive, row.contribution);
        IdentityField(archive, row.revision);
        Fields(archive, row.sourceDigest);
        Fields(archive, row.polygons);
    }

    template <class Archive> void Fields(Archive &archive, NavMeshProviderPayloadDescriptor &row) {
        Fields(archive, row.providerFingerprint);
        archive(row.formatVersion);
        Scalar(archive, row.byteOrder);
        Scalar(archive, row.compression);
        archive(row.byteOffset);
        archive(row.encodedBytes);
        archive(row.decodedBytes);
        Fields(archive, row.payloadDigest);
    }

    template <class Archive> void Fields(Archive &archive, std::uint32_t &value) {
        archive(value);
    }
}  // namespace Horo::Navigation::MeshWire
