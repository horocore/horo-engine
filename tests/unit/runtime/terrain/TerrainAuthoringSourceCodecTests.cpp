#include "FoliageAuthoringTestSupport.h"
#include "Horo/Terrain/TerrainAuthoringSourceCodec.h"

#include <Horo/Foundation/Sha256.h>
#include <bit>
#include <catch2/catch_test_macros.hpp>

using namespace Horo;
using namespace Horo::Terrain;

namespace {
    template <class T> T Id(std::uint8_t value) {
        SerializedTerrainIdentity bytes{};
        bytes.back() = value;
        return T::Create(bytes).Value();
    }

    Assets::AssetId Asset(std::uint8_t value) {
        SerializedTerrainIdentity bytes{};
        bytes.back() = value;
        return Assets::AssetId::FromBytes(bytes);
    }

    TerrainConfigurationSnapshot Configuration() {
        return TerrainConfigurationSnapshot::Create({.configuration = TerrainConfigurationRevision::Create(1).Value(),
                                                     .capability = TerrainCapabilityRevision::Create(1).Value(),
                                                     .tier = TerrainFeatureTier::Ultra,
                                                     .limits = GetTerrainTierProfile(TerrainFeatureTier::Ultra).Value().limits})
            .Value();
    }

    TerrainAuthoringLayerSet Layers() {
        TerrainAuthoringLayerSet result{.asset = Asset(2),
                                        .layers = {.id = Id<TerrainLayerSetId>(2),
                                                   .revision = TerrainContentRevision::Create(7).Value(),
                                                   .layerCount = 16}};
        for (std::uint8_t i = 0; i < 16; ++i)
            result.layers.layers[i] = {Id<TerrainLayerId>(i + 1), Id<TerrainMaterialAssetId>(i + 1), 1000};
        return result;
    }

    TerrainAuthoringSource Source(const TerrainAuthoringLayerSet &layers) {
        TerrainAuthoringSource value;
        value.source = {.dataset = Id<TerrainDatasetId>(1),
                        .sourceAsset = Asset(1),
                        .revision = TerrainSourceRevision::Create(9).Value(),
                        .capability = TerrainCapabilityRevision::Create(41).Value(),
                        .width = 3,
                        .height = 3,
                        .heightsMeters = {0, 1, 2, 3, 4, 5, 6, 7, 8},
                        .weights = std::vector<std::uint16_t>(9 * 16),
                        .holes = {0, 0, 0, 0, 1, 0, 0, 0, 0},
                        .layerCount = 16};
        const auto bytes = EncodeTerrainAuthoringLayerSet(layers, Configuration()).Value();
        TerrainAuthoringLayerBinding binding{.asset = layers.asset,
                                             .id = layers.layers.id,
                                             .revision = layers.layers.revision,
                                             .digest = ComputeSha256(bytes)};
        for (std::size_t i = 0; i < 16; ++i)
            binding.order[i] = layers.layers.layers[i].id;
        value.layers = binding;
        for (std::size_t i = 0; i < 9; ++i)
            value.source.weights[i * 16 + 15] = 65535;
        return value;
    }

    void Reseal(std::vector<std::byte> &bytes) {
        const auto digest = ComputeSha256(std::span<const std::byte>(bytes).first(bytes.size() - 32));
        for (std::size_t i = 0; i < 32; ++i)
            bytes[bytes.size() - 32 + i] = static_cast<std::byte>(digest.bytes[i]);
    }
}  // namespace

TEST_CASE("Canonical source codec preserves all16semantic channels holes and stable revisions", "[terrain][source-codec]") {
    const auto layers = Layers();
    const auto source = Source(layers);
    auto bytes = EncodeTerrainAuthoringSource(source);
    REQUIRE(bytes.HasValue());
    REQUIRE(IsTerrainAuthoringSource(bytes.Value()));
    const auto capability = TerrainCapabilityRevision::Create(99).Value();
    auto decoded = DecodeTerrainAuthoringSource(bytes.Value(), source.source.sourceAsset, capability);
    REQUIRE(decoded.HasValue());
    REQUIRE(decoded.Value().source.weights == source.source.weights);
    REQUIRE(decoded.Value().source.holes == source.source.holes);
    REQUIRE(decoded.Value().source.heightsMeters == source.source.heightsMeters);
    REQUIRE(decoded.Value().source.revision == source.source.revision);
    REQUIRE(decoded.Value().source.capability == capability);
    REQUIRE(decoded.Value().layers == source.layers);
    REQUIRE(EncodeTerrainAuthoringSource(decoded.Value()).Value() == bytes.Value());
    const auto layerBytes = EncodeTerrainAuthoringLayerSet(layers, Configuration()).Value();
    REQUIRE(ValidateTerrainAuthoringLayerBinding(decoded.Value(), layers, ComputeSha256(layerBytes)).HasValue());
    REQUIRE(DecodeTerrainAuthoringLayerSet(layerBytes, layers.asset, Configuration()).Value().layers == layers.layers);
}

TEST_CASE("Canonical source codec charges exact byte and channel work boundaries before allocation", "[terrain][source-codec]") {
    const auto source = Source(Layers());
    const auto bytes = EncodeTerrainAuthoringSource(source).Value();
    TerrainAuthoringCodecLimits limits;
    limits.maximumBytes = bytes.size();
    REQUIRE(EncodeTerrainAuthoringSource(source, limits).HasValue());
    REQUIRE(DecodeTerrainAuthoringSource(bytes, source.source.sourceAsset, source.source.capability, limits).HasValue());
    --limits.maximumBytes;
    REQUIRE(EncodeTerrainAuthoringSource(source, limits).HasError());
    REQUIRE(DecodeTerrainAuthoringSource(bytes, source.source.sourceAsset, source.source.capability, limits).HasError());
    limits = {};
    limits.maximumSamples = 8;
    REQUIRE(EncodeTerrainAuthoringSource(source, limits).HasError());
    limits = {};
    limits.maximumWorkItems = 9 * 18;
    REQUIRE(EncodeTerrainAuthoringSource(source, limits).HasError());
    ++limits.maximumWorkItems;
    REQUIRE(EncodeTerrainAuthoringSource(source, limits).HasValue());
}

TEST_CASE("Malformed canonical source captures fail closed without channel repair", "[terrain][source-codec]") {
    const auto source = Source(Layers());
    auto bytes = EncodeTerrainAuthoringSource(source).Value();
    auto asset = source.source.sourceAsset;
    SECTION("Integrity") {
        bytes.back() ^= std::byte{1};
    }
    SECTION("Truncated") {
        bytes.resize(16);
    }
    SECTION("Excess") {
        bytes.push_back(std::byte{});
    }
    SECTION("Unknown version with valid integrity") {
        bytes[8] = std::byte{2};
        Reseal(bytes);
    }
    SECTION("Invalid normalized weight with valid integrity") {
        bytes[456] = std::byte{1};
        Reseal(bytes);
    }
    SECTION("Foreign actual asset") {
        asset = Asset(9);
    }
    REQUIRE(DecodeTerrainAuthoringSource(bytes, asset, source.source.capability).HasError());
}

TEST_CASE("Layer binding never substitutes different material meaning or reorders stored channels", "[terrain][source-codec]") {
    auto layers = Layers();
    auto source = Source(layers);
    const auto bytes = EncodeTerrainAuthoringLayerSet(layers, Configuration()).Value();
    const auto digest = ComputeSha256(bytes);
    SECTION("Changed revision") {
        layers.layers.revision = TerrainContentRevision::Create(8).Value();
    }
    SECTION("Changed semantic order") {
        std::swap(layers.layers.layers[0], layers.layers.layers[15]);
    }
    SECTION("Changed actual asset") {
        layers.asset = Asset(3);
    }
    SECTION("Changed source digest") {
        source.layers->digest.bytes[0] ^= 1;
    }
    REQUIRE(ValidateTerrainAuthoringLayerBinding(source, layers, digest).HasError());
    layers = Layers();
    layers.layers.layers[15].id = layers.layers.layers[0].id;
    REQUIRE(EncodeTerrainAuthoringLayerSet(layers, Configuration()).HasError());
}

TEST_CASE("Canonical terrain source preserves actual authored foliage with aggregate admission", "[terrain][source-codec]") {
    auto source = Source(Layers());
    source.foliage = FoliageAuthoringTest::Source();
    const auto encoded = EncodeTerrainAuthoringSource(source);
    REQUIRE(encoded.HasValue());
    const auto decoded = DecodeTerrainAuthoringSource(encoded.Value(), source.source.sourceAsset, source.source.capability);
    REQUIRE(decoded.HasValue());
    CHECK(decoded.Value().foliage.placements == source.foliage.placements);
    CHECK(decoded.Value().foliage.definitions == source.foliage.definitions);
    CHECK(EncodeTerrainAuthoringSource(decoded.Value()).Value() == encoded.Value());
    TerrainAuthoringCodecLimits limits;
    const auto footprint = MeasureFoliageAuthoringSource(source.foliage.placements.size(), source.foliage.definitions.size());
    REQUIRE(footprint.HasValue());
    limits.maximumWorkItems = footprint.Value().workItems + 9 * 18;
    CHECK(EncodeTerrainAuthoringSource(source, limits).HasError());
    CHECK(DecodeTerrainAuthoringSource(encoded.Value(), source.source.sourceAsset, source.source.capability, limits).HasError());
    ++limits.maximumWorkItems;
    CHECK(EncodeTerrainAuthoringSource(source, limits).HasValue());
    CHECK(DecodeTerrainAuthoringSource(encoded.Value(), source.source.sourceAsset, source.source.capability, limits).HasValue());
    limits = {};
    limits.foliage.maximumPlacements = 1;
    CHECK(EncodeTerrainAuthoringSource(source, limits).HasError());
    CHECK(DecodeTerrainAuthoringSource(encoded.Value(), source.source.sourceAsset, source.source.capability, limits).HasError());
}
