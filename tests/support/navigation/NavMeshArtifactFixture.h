#pragma once

#include "Horo/Navigation/NavMeshData.h"
#include "navigation/NavigationTestAssertions.h"

#include <array>
#include <vector>

namespace Horo::Navigation::TestSupport {
    struct ArtifactFixture final {
        NavMeshArtifactHeader header{
            .coordinateFrame = {.origin = Math::WorldCoordinate64::FromMillimeters(12'000, 0, -8'000), .tileSizeMeters = 32.0F},
            .profile = {.id = Id<NavigationAgentProfileId>(7), .contentDigest = Digest(3)},
            .tileCount = 1,
            .vertexCount = 3,
            .polygonCount = 1,
            .polygonVertexIndexCount = 3,
            .offMeshLinkCount = 1,
            .provenanceCount = 1,
            .portableEncodedBytes = 128,
            .portableDecodedBytes = 128,
            .payloadDigest = Digest(10),
        };
        std::vector<NavMeshTileDescriptor> tiles{{
            .key = {.x = -2, .z = 4, .layer = 1},
            .bounds = {.minimum = {-64.0F, -1.0F, 128.0F}, .maximum = {-32.0F, 2.0F, 160.0F}},
            .vertices = {0, 3},
            .polygons = {0, 1},
            .polygonVertexIndices = {0, 3},
            .polygonAdjacencies = {0, 0},
            .offMeshLinks = {0, 1},
            .provenance = {0, 1},
            .providerPayloads = {0, 0},
            .payloadDigest = Digest(20),
        }};
        std::vector<Sha256Digest> observedTileDigests{Digest(20)};
        std::vector<Math::Vec3> vertices{{-64.0F, 0.0F, 128.0F}, {-32.0F, 0.0F, 128.0F}, {-64.0F, 0.0F, 160.0F}};
        std::vector<NavMeshPolygon> polygons{{.vertexIndices = {0, 3}, .adjacencies = {0, 0}, .area = Id<NavigationAreaId>(11)}};
        std::vector<std::uint32_t> polygonVertexIndices{0, 1, 2};
        std::vector<std::uint32_t> polygonAdjacencies;
        std::vector<NavMeshOffMeshLink> offMeshLinks{{
            .start = {-56.0F, 0.0F, 136.0F},
            .end = {-48.0F, 0.0F, 144.0F},
            .radiusMeters = 0.25F,
            .startPolygon = 0,
            .endPolygon = 0,
            .area = Id<NavigationAreaId>(11),
            .bidirectional = true,
        }};
        std::vector<NavMeshSourceProvenance> provenance{{
            .kind = NavigationSourceProducerKind::StaticCollider,
            .producer = Id<NavigationSourceProducerId>(21),
            .contribution = Id<NavigationSourceContributionId>(22),
            .revision = Id<NavigationSourceRevision>(23),
            .sourceDigest = Digest(30),
            .polygons = {0, 1},
        }};
        std::vector<NavMeshProviderPayloadDescriptor> providerPayloads;
        std::vector<std::byte> providerPayloadBytes;

        [[nodiscard]] NavMeshArtifactView View() const {
            return {
                .header = header,
                .observedPayloadDigest = header.payloadDigest,
                .tiles = tiles,
                .observedTilePayloadDigests = observedTileDigests,
                .tables =
                    {
                        .vertices = vertices,
                        .polygons = polygons,
                        .polygonVertexIndices = polygonVertexIndices,
                        .polygonAdjacencies = polygonAdjacencies,
                        .offMeshLinks = offMeshLinks,
                        .provenance = provenance,
                        .providerPayloads = providerPayloads,
                    },
                .providerPayloadBytes = providerPayloadBytes,
            };
        }

        void AppendProviderPayload(const std::uint32_t formatVersion, const std::span<const std::byte> bytes) {
            const auto byteOffset = providerPayloadBytes.size();
            providerPayloadBytes.insert(providerPayloadBytes.end(), bytes.begin(), bytes.end());
            providerPayloads.push_back({
                .providerFingerprint = Digest(80),
                .formatVersion = formatVersion,
                .byteOrder = NavMeshByteOrder::LittleEndian,
                .compression = NavMeshCompression::None,
                .byteOffset = byteOffset,
                .encodedBytes = bytes.size(),
                .decodedBytes = bytes.size(),
                .payloadDigest = ComputeSha256(bytes),
            });
            tiles.front().providerPayloads.count = static_cast<std::uint32_t>(providerPayloads.size());
            header.providerPayloadCount = static_cast<std::uint32_t>(providerPayloads.size());
            header.providerEncodedBytes = providerPayloadBytes.size();
            header.providerDecodedBytes = providerPayloadBytes.size();
        }

        void AddProviderPayload() {
            constexpr std::array Bytes{std::byte{0x10}, std::byte{0x20}, std::byte{0x30}};
            AppendProviderPayload(4, Bytes);
        }
    };
}  // namespace Horo::Navigation::TestSupport
