#pragma once
#include "Horo/Terrain/FoliageAuthoringSource.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Terrain::FoliageAuthoringTest {
    template <typename T> T Id(std::uint8_t marker) {
        SerializedTerrainIdentity bytes{};
        bytes[0] = marker;
        auto result = T::Create(bytes);
        REQUIRE(result.HasValue());
        return result.Value();
    }

    template <typename T> T Rev(std::uint64_t number) {
        auto result = T::Create(number);
        REQUIRE(result.HasValue());
        return result.Value();
    }

    inline Assets::AssetId Asset(std::uint8_t marker) {
        SerializedTerrainIdentity bytes{};
        bytes[0] = marker;
        return Assets::AssetId::FromBytes(bytes);
    }

    inline TerrainConfigurationSnapshot Configuration() {
        auto profile = GetTerrainTierProfile(TerrainFeatureTier::Baseline);
        REQUIRE(profile.HasValue());
        auto result = TerrainConfigurationSnapshot::Create({.configuration = Rev<TerrainConfigurationRevision>(1),
                                                            .capability = Rev<TerrainCapabilityRevision>(1),
                                                            .tier = TerrainFeatureTier::Baseline,
                                                            .limits = profile.Value().limits});
        REQUIRE(result.HasValue());
        return result.Value();
    }

    inline FoliageDefinitionCapabilitySet Capabilities() {
        return {FoliageDefinitionCapabilityBit<FoliageDefinitionCapability::CpuCulling>};
    }

    inline FoliageTypeSource Type() {
        FoliageTypeDefinitionData data;
        data.type = Id<FoliageTypeId>(2);
        data.revision = Rev<FoliageDefinitionRevision>(1);
        data.assets.meshLods[0] = Id<FoliageMeshAssetId>(3);
        data.assets.meshLodCount = 1;
        data.assets.material = Id<FoliageMaterialAssetId>(4);
        data.placement.densityPerSquareKilometer = 10'000;
        data.placement.minimumAltitudeMillimeters = -2'000;
        data.placement.maximumAltitudeMillimeters = 2'000;
        data.placement.maximumSlopeMilliDegrees = 90'000;
        data.placement.coordinateQuantumMillimeters = 1;
        data.placement.minimumSeparationMillimeters = 1;
        data.culling.meshLodDistanceMillimeters[0] = 10'000;
        data.culling.cullDistanceMillimeters = 20'000;
        data.maximumInstances = 1'000;
        return {Asset(5), data};
    }

    inline FoliageAuthoringSource Source() {
        auto type = Type();
        auto encoded = EncodeFoliageTypeSource(type, Configuration(), Capabilities());
        REQUIRE(encoded.HasValue());
        auto digest = ComputeSha256(encoded.Value());
        FoliageAuthoringSource source;
        source.definitions.push_back({type.asset, type.definition.type, type.definition.revision, digest});
        source.placements.push_back({Id<FoliageInstanceId>(7), type.definition.type, 1, 1, 0.25F, 1.0F, 0.5F});
        source.placements.push_back({Id<FoliageInstanceId>(8), type.definition.type, 2, 2, 0, 1.0F, 0});
        return source;
    }
}  // namespace Horo::Terrain::FoliageAuthoringTest
