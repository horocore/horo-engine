#include "Horo/Navigation/NavMeshCodec.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "navigation/NavMeshArtifactFixture.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Navigation {
    TEST_CASE("NavMesh wire codec preserves neutral and opaque provider tables with actual digests", "[unit][navigation][navmesh_codec]") {
        TestSupport::ArtifactFixture fixture;
        fixture.AddProviderPayload();
        const auto encoded = EncodeNavMeshArtifact(fixture.View());
        REQUIRE(encoded.HasValue());
        auto decoded = DecodeNavMeshArtifact(encoded.Value());
        REQUIRE(decoded.HasValue());
        const auto &data = decoded.Value().data;
        REQUIRE(data.Header().coordinateFrame == fixture.header.coordinateFrame);
        REQUIRE(data.Header().profile == fixture.header.profile);
        REQUIRE(data.Header().payloadDigest == ComputeSha256(std::span<const std::byte>{encoded.Value()}.subspan(203)));
        REQUIRE(decoded.Value().encodedTiles.size() == 1);
        const auto &range = decoded.Value().encodedTiles.front();
        REQUIRE(data.Tiles().front().payloadDigest ==
                ComputeSha256(std::span<const std::byte>{encoded.Value()}.subspan(range.offset, range.bytes)));
        const auto tile = data.ResolveTile(fixture.tiles.front().key);
        REQUIRE(tile.HasValue());
        REQUIRE(std::ranges::equal(tile.Value().tables.vertices, fixture.vertices));
        REQUIRE(std::ranges::equal(tile.Value().tables.polygons, fixture.polygons));
        REQUIRE(std::ranges::equal(tile.Value().tables.offMeshLinks, fixture.offMeshLinks));
        REQUIRE(std::ranges::equal(tile.Value().tables.provenance, fixture.provenance));
        const auto provider =
            data.ResolveProviderPayload(fixture.tiles.front().key, {fixture.providerPayloads.front().providerFingerprint, 4});
        REQUIRE(provider.HasValue());
        REQUIRE(std::ranges::equal(provider.Value().bytes, fixture.providerPayloadBytes));
    }

    TEST_CASE("NavMesh wire codec checks hostile tile ranges and semantic corruption after valid outer checksums",
              "[unit][navigation][navmesh_codec][hostile]") {
        TestSupport::ArtifactFixture fixture;
        const auto encoded = EncodeNavMeshArtifact(fixture.View());
        REQUIRE(encoded.HasValue());
        auto corrupted = encoded.Value();
        const auto rehashBody = [&] {
            const auto digest = ComputeSha256(std::span<const std::byte>{corrupted}.subspan(203));
            for (std::size_t index = 0; index < digest.bytes.size(); ++index)
                corrupted[171 + index] = static_cast<std::byte>(digest.bytes[index]);
        };
        SECTION("hostile table count cannot allocate beyond header bounds") {
            for (std::size_t index = 241; index < 245; ++index)
                corrupted[index] = std::byte{0xff};
            rehashBody();
        }
        SECTION("nonfinite vertex is rejected by the shared neutral validator") {
            corrupted[333] = std::byte{0};
            corrupted[334] = std::byte{0};
            corrupted[335] = std::byte{0xc0};
            corrupted[336] = std::byte{0x7f};
            const auto digest = ComputeSha256(std::span<const std::byte>{corrupted}.subspan(333));
            for (std::size_t index = 0; index < digest.bytes.size(); ++index)
                corrupted[293 + index] = static_cast<std::byte>(digest.bytes[index]);
            rehashBody();
        }
        SECTION("missing tile fragment cannot be hidden by a valid body checksum") {
            corrupted[325] = std::byte{0xff};
            corrupted[326] = std::byte{0xff};
            rehashBody();
        }
        REQUIRE(DecodeNavMeshArtifact(corrupted).HasError());
    }

    TEST_CASE("NavMesh wire codec rejects every truncation and tampered body before publication",
              "[unit][navigation][navmesh_codec][hostile]") {
        TestSupport::ArtifactFixture fixture;
        const auto encoded = EncodeNavMeshArtifact(fixture.View());
        REQUIRE(encoded.HasValue());
        for (std::size_t size = 0; size < encoded.Value().size(); ++size) {
            REQUIRE(DecodeNavMeshArtifact(std::span<const std::byte>{encoded.Value()}.first(size)).HasError());
        }
        auto corrupted = encoded.Value();
        corrupted.back() ^= std::byte{1};
        REQUIRE(DecodeNavMeshArtifact(corrupted).HasError());
        corrupted = encoded.Value();
        corrupted.push_back(std::byte{0});
        REQUIRE(DecodeNavMeshArtifact(corrupted).HasError());
        corrupted = encoded.Value();
        corrupted[4] = std::byte{2};
        TestSupport::RequireError(DecodeNavMeshArtifact(corrupted), NavigationErrors::UnsupportedCookedVersion);
        corrupted = encoded.Value();
        corrupted[9] = std::byte{1};
        TestSupport::RequireError(DecodeNavMeshArtifact(corrupted), NavigationErrors::UnsupportedCookedVersion);
        NavMeshArtifactLimits limits;
        limits.maxVertices = 2;
        TestSupport::RequireError(DecodeNavMeshArtifact(encoded.Value(), limits), NavigationErrors::NavMeshArtifactCapacityExceeded);
        limits = {};
        limits.maxPortableEncodedBytes = 128;
        TestSupport::RequireError(EncodeNavMeshArtifact(fixture.View(), limits), NavigationErrors::NavMeshArtifactCapacityExceeded);
        limits = {};
        limits.maxPortableEncodedBytes = 200;
        TestSupport::RequireError(EncodeNavMeshArtifact(fixture.View(), limits), NavigationErrors::NavMeshArtifactCapacityExceeded);
        limits = {};
        limits.maxPortableDecodedBytes = 128;
        TestSupport::RequireError(EncodeNavMeshArtifact(fixture.View(), limits), NavigationErrors::NavMeshArtifactCapacityExceeded);
        limits = {};
        limits.maxPortableEncodedBytes = 1;
        TestSupport::RequireError(EncodeNavMeshArtifact(fixture.View(), limits), NavigationErrors::NavMeshArtifactCapacityExceeded);
    }
}  // namespace Horo::Navigation
