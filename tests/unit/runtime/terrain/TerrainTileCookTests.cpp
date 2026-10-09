#include "Horo/Terrain/TerrainTileCook.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace Horo::Terrain {
    namespace {
        TerrainCanonicalSource Source() {
            SerializedTerrainIdentity projectBytes{};
            projectBytes[0] = 7;
            const auto project = TerrainProjectId::Create(projectBytes).Value();
            constexpr std::string_view key = "terrain/tile-cook-test";
            TerrainCanonicalSource source;
            source.dataset = DeriveTerrainDatasetId(project, std::as_bytes(std::span{key.data(), key.size()})).Value();
            std::array<std::uint8_t, 16> assetBytes{};
            assetBytes[0] = 2;
            source.sourceAsset = Assets::AssetId::FromBytes(assetBytes);
            source.revision = TerrainSourceRevision::Create(1).Value();
            source.capability = TerrainCapabilityRevision::Create(1).Value();
            source.width = 5;
            source.height = 5;
            source.coordinates.space = TerrainCoordinateSpace::ProjectedMeters;
            source.coordinates.projectedCrs = "EPSG:32632";
            source.coordinates.originX = -10;
            source.coordinates.originZ = 20;
            source.coordinates.spacingX = 0.5;
            source.coordinates.spacingZ = 2;
            source.layerCount = 2;
            for (std::uint32_t z = 0; z < source.height; ++z) {
                for (std::uint32_t x = 0; x < source.width; ++x) {
                    source.heightsMeters.push_back(static_cast<float>(x + z * 2));
                    source.weights.push_back(static_cast<std::uint16_t>(x * 100));
                    source.weights.push_back(static_cast<std::uint16_t>(65'535 - x * 100));
                    source.holes.push_back(x == 2 && z == 2 ? 1 : 0);
                }
            }
            return source;
        }

        Sha256Digest Digest(const std::uint8_t marker) {
            Sha256Digest digest;
            digest.bytes[0] = marker;
            return digest;
        }

        TerrainTileCookProfile Profile() {
            TerrainTileCookProfile profile;
            profile.interiorQuads = 2;
            profile.lodLevels = 2;
            profile.targetDigest = Digest(1);
            profile.toolchainDigest = Digest(2);
            return profile;
        }

        TerrainTileCookDependency Dependency(const std::uint8_t marker) {
            std::array<std::uint8_t, 16> bytes{};
            bytes[0] = marker;
            return {.asset = Assets::AssetId::FromBytes(bytes), .artifactDigest = Digest(marker)};
        }

        template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &code) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().domain.Value() == code.domain.Value());
            CHECK(result.ErrorValue().code.Value() == code.code.Value());
        }
    }  // namespace

    TEST_CASE("Terrain tile cook partitions every channel and validates matching seams", "[terrain][cook]") {
        const auto source = Source();
        const auto cooked = CookTerrainTiles(source, Profile(), {}, {});
        REQUIRE(cooked.HasValue());
        const auto &tiles = cooked.Value().tiles;
        REQUIRE(tiles.size() == 5);
        CHECK((tiles[0].id.tile == TerrainTileCoordinate{-10, 5, 0}));
        CHECK((tiles[1].id.tile == TerrainTileCoordinate{-9, 5, 0}));
        CHECK((tiles[2].id.tile == TerrainTileCoordinate{-10, 6, 0}));
        CHECK((tiles[4].id.tile == TerrainTileCoordinate{-5, 2, 1}));
        CHECK(tiles[0].samplesX == 3);
        CHECK(tiles[4].samplesX == 3);
        CHECK(tiles[0].seams[1] == tiles[1].seams[0]);
        CHECK(tiles[0].seams[3] == tiles[2].seams[2]);
        CHECK(VerifyCookedTerrainTiles(cooked.Value()).HasValue());
        CHECK(tiles[0].payload != tiles[1].payload);
        // V1 layout keeps signed placement, exact projected CRS and interleaved channel bytes.
        CHECK(tiles[0].payload[151] == 2);
        CHECK(tiles[0].payload[152] == 1);
        CHECK(tiles[0].payload[153] == static_cast<std::uint8_t>(TerrainCoordinateSpace::ProjectedMeters));
        CHECK(tiles[0].payload[154] == 10);
        CHECK(tiles[0].payload[302] == 1);  // Center sample is a hole after height and both weights.
    }

    TEST_CASE("Clean and incremental Terrain cooks converge independent of dependency order and cache state", "[terrain][cook]") {
        const auto source = Source();
        const std::array dependencies{Dependency(3), Dependency(4)};
        const std::array reverse{dependencies[1], dependencies[0]};
        const auto clean = CookTerrainTiles(source, Profile(), dependencies, {});
        REQUIRE(clean.HasValue());
        const auto reordered = CookTerrainTiles(source, Profile(), reverse, {}, &clean.Value());
        REQUIRE(reordered.HasValue());
        CHECK(reordered.Value().sourceDigest == clean.Value().sourceDigest);
        CHECK(reordered.Value().fingerprint == clean.Value().fingerprint);
        CHECK(reordered.Value().manifestDigest == clean.Value().manifestDigest);
        for (std::size_t index = 0; index < clean.Value().tiles.size(); ++index)
            CHECK(reordered.Value().tiles[index].payload == clean.Value().tiles[index].payload);
    }

    TEST_CASE("Source replacement changes provenance without mutating prior Terrain tiles", "[terrain][cook]") {
        auto source = Source();
        const auto prior = CookTerrainTiles(source, Profile(), {}, {});
        REQUIRE(prior.HasValue());
        source.revision = TerrainSourceRevision::Create(2).Value();
        source.heightsMeters[0] = -50;
        const auto replacement = CookTerrainTiles(source, Profile(), {}, {}, &prior.Value());
        REQUIRE(replacement.HasValue());
        CHECK(replacement.Value().sourceRevision == source.revision);
        CHECK(replacement.Value().manifestDigest != prior.Value().manifestDigest);
        CHECK(VerifyCookedTerrainTiles(prior.Value()).HasValue());
        CHECK(VerifyCookedTerrainTiles(replacement.Value()).HasValue());
    }

    TEST_CASE("Malformed Terrain source, dependency and finite budgets fail closed", "[terrain][cook]") {
        auto source = Source();
        source.weights[0] = 1;
        RequireError(CookTerrainTiles(source, Profile(), {}, {}), TerrainTileCookErrors::InvalidSource);
        source = Source();
        source.holes[0] = 2;
        RequireError(CookTerrainTiles(source, Profile(), {}, {}), TerrainTileCookErrors::InvalidSource);
        source = Source();
        const std::array duplicate{Dependency(3), Dependency(3)};
        RequireError(CookTerrainTiles(source, Profile(), duplicate, {}), TerrainTileCookErrors::InvalidProfile);
        auto profile = Profile();
        profile.maximumTiles = 4;
        RequireError(CookTerrainTiles(source, profile, {}, {}), TerrainTileCookErrors::LimitExceeded);
        profile = Profile();
        profile.maximumPayloadBytes = 100;
        RequireError(CookTerrainTiles(source, profile, {}, {}), TerrainTileCookErrors::LimitExceeded);
        profile = Profile();
        profile.maximumWorkItems = 5;
        RequireError(CookTerrainTiles(source, profile, {}, {}), TerrainTileCookErrors::LimitExceeded);
    }

    TEST_CASE("Cancellation and corrupt prior Terrain candidates never publish a new cook", "[terrain][cook]") {
        const auto source = Source();
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        RequireError(CookTerrainTiles(source, Profile(), {}, cancellation.Token()), TerrainTileCookErrors::Cancelled);
        auto prior = CookTerrainTiles(source, Profile(), {}, {});
        REQUIRE(prior.HasValue());
        auto corrupt = prior.Value();
        corrupt.tiles[0].payload.back() ^= 1;
        RequireError(VerifyCookedTerrainTiles(corrupt), TerrainTileCookErrors::CorruptPrevious);
        RequireError(CookTerrainTiles(source, Profile(), {}, {}, &corrupt), TerrainTileCookErrors::CorruptPrevious);
    }

    TEST_CASE("Partial edge tiles preserve final samples and optional channels remain explicit", "[terrain][cook]") {
        auto source = Source();
        source.width = 6;
        source.height = 4;
        source.heightsMeters.resize(24, 1.0F);
        source.layerCount = 0;
        source.weights.clear();
        source.holes.clear();
        const auto cooked = CookTerrainTiles(source, Profile(), {}, {});
        REQUIRE(cooked.HasValue());
        REQUIRE(cooked.Value().tiles.size() == 8);
        CHECK(cooked.Value().tiles[2].samplesX == 2);
        CHECK(cooked.Value().tiles[6].samplesX == 3);
        CHECK(cooked.Value().tiles[0].payload[151] == 0);
        CHECK(cooked.Value().tiles[0].payload[152] == 0);
        CHECK(VerifyCookedTerrainTiles(cooked.Value()).HasValue());
    }

    TEST_CASE("Terrain verification rejects oversized layout and invalid counts before payload work", "[terrain][cook]") {
        auto cooked = CookTerrainTiles(Source(), Profile(), {}, {});
        REQUIRE(cooked.HasValue());
        auto invalid = cooked.Value();
        // Exact layout length is checked before hashing, without allocating the global staging ceiling.
        invalid.tiles.front().payload.resize(invalid.tiles.front().payload.size() + 4096, 0);
        RequireError(VerifyCookedTerrainTiles(invalid), TerrainTileCookErrors::CorruptPrevious);
        invalid = cooked.Value();
        invalid.tiles.front().samplesX = std::numeric_limits<std::uint32_t>::max();
        RequireError(VerifyCookedTerrainTiles(invalid), TerrainTileCookErrors::CorruptPrevious);
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        RequireError(VerifyCookedTerrainTiles(cooked.Value(), cancellation.Token()), TerrainTileCookErrors::Cancelled);
    }

    TEST_CASE("World tile addresses admit the last signed tile and reject overflow", "[terrain][cook]") {
        auto source = Source();
        source.coordinates.originX = static_cast<double>(std::numeric_limits<std::int32_t>::max()) - 1.0;
        const auto lastValid = CookTerrainTiles(source, Profile(), {}, {});
        REQUIRE(lastValid.HasValue());
        CHECK(lastValid.Value().tiles[0].id.tile.x == std::numeric_limits<std::int32_t>::max() - 1);
        CHECK(lastValid.Value().tiles[1].id.tile.x == std::numeric_limits<std::int32_t>::max());
        CHECK(VerifyCookedTerrainTiles(lastValid.Value()).HasValue());

        source.coordinates.originX = static_cast<double>(std::numeric_limits<std::int32_t>::max());
        RequireError(CookTerrainTiles(source, Profile(), {}, {}), TerrainTileCookErrors::InvalidSource);
    }
}  // namespace Horo::Terrain
