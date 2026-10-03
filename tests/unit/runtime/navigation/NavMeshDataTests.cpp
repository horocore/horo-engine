#include "Horo/Navigation/NavMeshData.h"
#include "Horo/Navigation/NavigationErrors.h"
#include "navigation/NavMeshArtifactFixture.h"
#include "navigation/NavigationTestAssertions.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Navigation {
    static_assert(!std::is_copy_constructible_v<NavMeshData>);
    static_assert(!std::is_copy_assignable_v<NavMeshData>);
    static_assert(std::is_nothrow_move_constructible_v<NavMeshData>);
    static_assert(!std::is_move_assignable_v<NavMeshData>);

    namespace {
        using TestSupport::Digest;
        using TestSupport::Id;
        using TestSupport::RequireError;

        [[nodiscard]] NavMeshProviderPayloadCompatibility ProviderCompatibility(const std::uint32_t formatVersion = 4,
                                                                                const std::uint8_t fingerprintSeed = 80) {
            return {
                .providerFingerprint = Digest(fingerprintSeed),
                .formatVersion = formatVersion,
                .byteOrder = NavMeshByteOrder::LittleEndian,
                .compression = NavMeshCompression::None,
            };
        }

        using TestSupport::ArtifactFixture;

    }  // namespace

    TEST_CASE("NavMesh artifact owns versioned portable tile data independently of caller storage",
              "[unit][navigation][navmesh_artifact][lifecycle]") {
        ArtifactFixture fixture;
        auto artifact = std::move(NavMeshData::Create(fixture.View())).Value();
        fixture.vertices.front() = {99.0F, 99.0F, 99.0F};
        fixture.tiles.clear();

        auto moved = std::move(artifact);
        REQUIRE(moved.Header().formatVersion == CurrentNavMeshFormatVersion);
        REQUIRE(moved.Header().profile.id == Id<NavigationAgentProfileId>(7));
        REQUIRE(moved.Header().coordinateFrame.origin == Math::WorldCoordinate64::FromMillimeters(12'000, 0, -8'000));
        const auto tile = moved.ResolveTile({.x = -2, .z = 4, .layer = 1});
        REQUIRE(tile.HasValue());
        REQUIRE(tile.Value().tables.vertices.front() == Math::Vec3{-64.0F, 0.0F, 128.0F});
        REQUIRE(tile.Value().tables.polygons.front().area == Id<NavigationAreaId>(11));
        RequireError(moved.ResolveTile({.x = -2, .z = 4, .layer = 2}), NavigationErrors::NavMeshTileUnknown);
    }

    TEST_CASE("NavMesh fixed header rejects hostile lengths and counts before decoded table allocation",
              "[unit][navigation][navmesh_artifact][hostile]") {
        ArtifactFixture fixture;
        REQUIRE(ValidateNavMeshArtifactHeader(fixture.header).HasValue());

        fixture.header.tileCount = std::numeric_limits<std::uint32_t>::max();
        RequireError(ValidateNavMeshArtifactHeader(fixture.header), NavigationErrors::NavMeshArtifactCapacityExceeded);
        fixture = {};
        fixture.header.providerPayloadCount = 1;
        fixture.header.providerEncodedBytes = std::numeric_limits<std::uint64_t>::max();
        fixture.header.providerDecodedBytes = std::numeric_limits<std::uint64_t>::max();
        RequireError(ValidateNavMeshArtifactHeader(fixture.header), NavigationErrors::NavMeshArtifactCapacityExceeded);
        fixture = {};
        fixture.header.vertexCount = 4;
        RequireError(NavMeshData::Create(fixture.View()), NavigationErrors::NavMeshArtifactCorrupt);

        NavMeshArtifactLimits limits{};
        limits.maxTiles = 0;
        RequireError(ValidateNavMeshArtifactHeader(fixture.header, limits), NavigationErrors::NavMeshArtifactInvalid);
    }

    TEST_CASE("NavMesh artifact accepts exact project limits and rejects a one-byte storage underrun",
              "[unit][navigation][navmesh_artifact][boundary]") {
        ArtifactFixture fixture;
        NavMeshArtifactLimits limits{};
        limits.maxTiles = 1;
        limits.maxVertices = 3;
        limits.maxPolygons = 1;
        limits.maxPolygonVertexIndices = 3;
        limits.maxPolygonAdjacencies = 1;
        limits.maxOffMeshLinks = 1;
        limits.maxProvenanceRows = 1;
        limits.maxProviderPayloads = 1;
        limits.maxVerticesPerPolygon = 3;
        limits.maxPortableEncodedBytes = 128;
        limits.maxPortableDecodedBytes = 128;
        limits.maxProviderEncodedBytes = 1;
        limits.maxProviderDecodedBytes = 1;
        limits.maxOwnedBytes = 4'096;
        REQUIRE(NavMeshData::Create(fixture.View(), limits).HasValue());

        limits.maxOwnedBytes = 1;
        RequireError(ValidateNavMeshArtifactHeader(fixture.header, limits), NavigationErrors::NavMeshArtifactCapacityExceeded);
    }

    TEST_CASE("NavMesh artifact rejects unsupported neutral versions byte order compression and coordinate frames",
              "[unit][navigation][navmesh_artifact][version]") {
        ArtifactFixture fixture;
        fixture.header.formatVersion.major = 2;
        RequireError(ValidateNavMeshArtifactHeader(fixture.header), NavigationErrors::UnsupportedCookedVersion);
        fixture = {};
        fixture.header.formatVersion.minor = 1;
        RequireError(ValidateNavMeshArtifactHeader(fixture.header), NavigationErrors::UnsupportedCookedVersion);
        fixture = {};
        fixture.header.byteOrder = NavMeshByteOrder::BigEndian;
        RequireError(ValidateNavMeshArtifactHeader(fixture.header), NavigationErrors::UnsupportedCookedVersion);
        fixture = {};
        fixture.header.compression = NavMeshCompression::Lz4;
        fixture.header.portableEncodedBytes = 64;
        REQUIRE(ValidateNavMeshArtifactHeader(fixture.header).HasValue());
        fixture.header.coordinateFrame.representation = static_cast<NavMeshCoordinateFrame>(255);
        RequireError(ValidateNavMeshArtifactHeader(fixture.header), NavigationErrors::UnsupportedCookedVersion);
    }

    TEST_CASE("One tile validates independently of unowned malformed rows and rejects its own malformed range",
              "[unit][navigation][navmesh_artifact][tile]") {
        ArtifactFixture fixture;
        fixture.vertices.push_back({std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F});
        REQUIRE(ValidateNavMeshTile(fixture.View(), 0).HasValue());

        fixture.tiles.front().bounds.minimum.x += 1.0F;
        RequireError(ValidateNavMeshTile(fixture.View(), 0), NavigationErrors::NavMeshArtifactCorrupt);
        fixture = {};
        fixture.tiles.front().polygonVertexIndices = {std::numeric_limits<std::uint32_t>::max(), 3};
        RequireError(ValidateNavMeshTile(fixture.View(), 0), NavigationErrors::NavMeshArtifactCorrupt);
        RequireError(ValidateNavMeshTile(fixture.View(), 1), NavigationErrors::NavMeshTileUnknown);
    }

    TEST_CASE("Provider payloads remain opaque and exact version mismatch is a typed compatibility failure",
              "[unit][navigation][navmesh_artifact][provider]") {
        ArtifactFixture fixture;
        fixture.AddProviderPayload();
        auto artifact = std::move(NavMeshData::Create(fixture.View())).Value();
        const NavMeshTileKey tile{.x = -2, .z = 4, .layer = 1};
        auto compatibility = ProviderCompatibility();

        const auto payload = artifact.ResolveProviderPayload(tile, compatibility);
        REQUIRE(payload.HasValue());
        REQUIRE(payload.Value().bytes.size() == 3);
        compatibility.formatVersion = 5;
        RequireError(artifact.ResolveProviderPayload(tile, compatibility), NavigationErrors::NavMeshProviderPayloadIncompatible);
        compatibility.formatVersion = 4;
        compatibility.providerFingerprint = Digest(81);
        RequireError(artifact.ResolveProviderPayload(tile, compatibility), NavigationErrors::NavMeshProviderPayloadUnavailable);
    }

    TEST_CASE("Provider resolution considers every format for one provider fingerprint", "[unit][navigation][navmesh_artifact][provider]") {
        ArtifactFixture fixture;
        fixture.AddProviderPayload();
        fixture.providerPayloads.front().formatVersion = 3;
        constexpr std::array AdditionalBytes{std::byte{0x40}};
        fixture.AppendProviderPayload(4, AdditionalBytes);

        auto artifact = std::move(NavMeshData::Create(fixture.View())).Value();
        const auto payload = artifact.ResolveProviderPayload({.x = -2, .z = 4, .layer = 1}, ProviderCompatibility());
        REQUIRE(payload.HasValue());
        REQUIRE(payload.Value().bytes.size() == 1);
        REQUIRE(payload.Value().bytes.front() == std::byte{0x40});
    }

    TEST_CASE("Polygon adjacency accepts only the boundary sentinel or another polygon", "[unit][navigation][navmesh_artifact][polygon]") {
        ArtifactFixture fixture;
        fixture.header.polygonAdjacencyCount = 3;
        fixture.tiles.front().polygonAdjacencies = {0, 3};
        fixture.polygons.front().adjacencies = {0, 3};
        fixture.polygonAdjacencies.assign(3, NavMeshBoundaryAdjacency);
        REQUIRE(NavMeshData::Create(fixture.View()).HasValue());

        fixture.polygonAdjacencies.back() = 1;
        RequireError(NavMeshData::Create(fixture.View()), NavigationErrors::NavMeshArtifactCorrupt);
    }

    TEST_CASE("NavMesh artifact rejects malformed tile bounds indices provenance checksums and provider lengths",
              "[unit][navigation][navmesh_artifact][hostile]") {
        ArtifactFixture fixture;
        fixture.tiles.front().bounds.maximum.x = -1.0F;
        RequireError(NavMeshData::Create(fixture.View()), NavigationErrors::NavMeshArtifactCorrupt);
        fixture = {};
        fixture.polygonVertexIndices.back() = 9;
        RequireError(NavMeshData::Create(fixture.View()), NavigationErrors::NavMeshArtifactCorrupt);
        fixture = {};
        fixture.provenance.front().polygons = {0, 0};
        RequireError(NavMeshData::Create(fixture.View()), NavigationErrors::NavMeshArtifactCorrupt);
        fixture = {};
        fixture.observedTileDigests.front() = Digest(99);
        RequireError(NavMeshData::Create(fixture.View()), NavigationErrors::NavMeshArtifactCorrupt);
        fixture = {};
        fixture.AddProviderPayload();
        fixture.providerPayloads.front().encodedBytes = 4;
        RequireError(NavMeshData::Create(fixture.View()), NavigationErrors::NavMeshArtifactCorrupt);
    }
}  // namespace Horo::Navigation
