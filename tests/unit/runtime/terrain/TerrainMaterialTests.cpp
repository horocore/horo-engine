#include "Horo/Terrain/TerrainMaterial.h"
#include "support/AllocationProbe.h"
#include "support/TerrainMaterialFixtures.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <numeric>

using namespace Horo;
using namespace Horo::Terrain;
using namespace Horo::Tests::TerrainMaterials;
using Horo::Tests::RequireError;

TEST_CASE("Terrain material layers preserve semantic order and independent tier caps", "[terrain][material]") {
    for (const auto tier :
         {TerrainFeatureTier::Baseline, TerrainFeatureTier::Standard, TerrainFeatureTier::High, TerrainFeatureTier::Ultra}) {
        const auto configuration = Configuration(tier);
        const auto cap = configuration.Data().limits.maximumLayersPerTile;
        const auto exact = TerrainMaterialLayerSet::Create(LayerData(cap), configuration);
        REQUIRE(exact.HasValue());
        CHECK(exact.Value().Data().layerCount == cap);
        if (cap < TerrainDescriptorHardLimits::LayersPerTile)
            RequireError(TerrainMaterialLayerSet::Create(LayerData(static_cast<std::uint8_t>(cap + 1)), configuration),
                         TerrainErrors::LimitExceeded);
    }
    auto reducedData = Configuration().Data();
    reducedData.limits.maximumLayersPerTile = 1;
    const auto reduced = TerrainConfigurationSnapshot::Create(reducedData).Value();
    RequireError(TerrainMaterialLayerSet::Create(LayerData(), reduced), TerrainErrors::LimitExceeded);
    auto data = LayerData();
    const auto owned = TerrainMaterialLayerSet::Create(data, Configuration()).Value();
    data.layers[0].uvScaleMilli = 9000;
    CHECK(owned.Data().layers[0].uvScaleMilli == 1000);
}

TEST_CASE("Terrain layers reject malformed identity duplicate meanings and noncanonical storage", "[terrain][material]") {
    auto data = LayerData();
    data.id = {};
    RequireError(TerrainMaterialLayerSet::Create(data, Configuration()), TerrainErrors::DescriptorInvalid);
    data = LayerData();
    data.layers[1].id = data.layers[0].id;
    RequireError(TerrainMaterialLayerSet::Create(data, Configuration()), TerrainErrors::IdentityConflict);
    data = LayerData();
    data.layers[2].material = data.layers[0].material;
    RequireError(TerrainMaterialLayerSet::Create(data, Configuration()), TerrainErrors::DescriptorInvalid);
    data = LayerData();
    data.layers[0].uvScaleMilli = 0;
    RequireError(TerrainMaterialLayerSet::Create(data, Configuration()), TerrainErrors::DescriptorInvalid);
    data = LayerData();
    data.layers[0].uvScaleMilli = 1'000'001;
    RequireError(TerrainMaterialLayerSet::Create(data, Configuration()), TerrainErrors::DescriptorInvalid);
    data = LayerData();
    data.layerCount = 0;
    RequireError(TerrainMaterialLayerSet::Create(data, Configuration()), TerrainErrors::DescriptorInvalid);
}

TEST_CASE("Terrain weights normalize exactly with stable ties without frame allocations", "[terrain][material]") {
    const std::array<std::uint32_t, 2> equal{1, 1};
    Tests::AllocationProbe::Measurement measured;
    TerrainMaterialWeights weights;
    {
        Tests::AllocationProbe::ScopedMeasurement allocation;
        weights = NormalizeTerrainMaterialWeights(equal).Value();
        measured = allocation.Snapshot();
    }
    CHECK(measured.requests == 0);
    CHECK(weights.values[0] == 32768);
    CHECK(weights.values[1] == 32767);
    std::array<std::uint32_t, 16> maximum;
    maximum.fill(std::numeric_limits<std::uint32_t>::max());
    const auto full = NormalizeTerrainMaterialWeights(maximum);
    REQUIRE(full.HasValue());
    CHECK(std::accumulate(full.Value().values.begin(), full.Value().values.end(), 0U) == 65535);
    CHECK(full.Value().values.back() == 4095);
    const std::array<std::uint32_t, 2> zeros{};
    RequireError(NormalizeTerrainMaterialWeights(zeros), TerrainErrors::DescriptorInvalid);
    RequireError(NormalizeTerrainMaterialWeights({}), TerrainErrors::DescriptorInvalid);
    const std::array<std::uint32_t, 17> excess{};
    RequireError(NormalizeTerrainMaterialWeights(excess), TerrainErrors::DescriptorInvalid);
}

TEST_CASE("Terrain canonical weights enforce exact semantic count tile mask and unused slots", "[terrain][material]") {
    const auto layers = TerrainMaterialLayerSet::Create(LayerData(), Configuration()).Value();
    const std::array<std::uint32_t, 2> raw{1, 0};
    auto weights = NormalizeTerrainMaterialWeights(raw).Value();
    REQUIRE(ValidateTerrainMaterialWeights(weights, layers, 1).HasValue());
    RequireError(ValidateTerrainMaterialWeights(weights, layers, 0), TerrainErrors::DescriptorInvalid);
    RequireError(ValidateTerrainMaterialWeights(weights, layers, 4), TerrainErrors::DescriptorInvalid);
    RequireError(ValidateTerrainMaterialWeights(weights, layers, 2), TerrainErrors::DescriptorInvalid);
    weights.values[2] = 1;
    RequireError(ValidateTerrainMaterialWeights(weights, layers, 3), TerrainErrors::DescriptorInvalid);
    weights.values[2] = 0;
    --weights.values[0];
    RequireError(ValidateTerrainMaterialWeights(weights, layers, 3), TerrainErrors::DescriptorInvalid);
    weights.layerCount = 1;
    RequireError(ValidateTerrainMaterialWeights(weights, layers, 3), TerrainErrors::DescriptorInvalid);
}
