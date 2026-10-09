#pragma once

#include "../../../support/TypedIdentityTestSupport.h"
#include "Horo/Terrain/TerrainProducerSnapshot.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Terrain::ProducerSnapshotTests {
    template <typename T> T Rev(const std::uint64_t value = 1) {
        return T::Create(value).Value();
    }

    template <typename T> T Id(const std::uint8_t marker = 1) {
        SerializedTerrainIdentity bytes{};
        bytes[0] = marker;
        return T::Create(bytes).Value();
    }

    inline Sha256Digest Digest(const std::uint8_t marker) {
        Sha256Digest value{};
        value.bytes[0] = marker;
        return value;
    }

    inline TerrainCanonicalSource Source(const bool holed = false) {
        TerrainCanonicalSource source;
        source.dataset = Id<TerrainDatasetId>();
        SerializedTerrainIdentity asset{};
        asset[0] = 9;
        source.sourceAsset = Assets::AssetId::FromBytes(asset);
        source.revision = Rev<TerrainSourceRevision>();
        source.capability = Rev<TerrainCapabilityRevision>();
        source.width = source.height = 5;
        source.coordinates.originX = source.coordinates.originZ = -20;
        source.coordinates.spacingX = source.coordinates.spacingZ = 10;
        source.heightsMeters.assign(25, 1);
        source.holes.assign(25, holed ? 1 : 0);
        return source;
    }

    inline CookedTerrainSourceArtifacts Cook(const TerrainCanonicalSource &source) {
        TerrainSourceArtifactProfile profile;
        profile.tiles.interiorQuads = 4;
        profile.tiles.targetDigest = Digest(7);
        profile.tiles.toolchainDigest = Digest(8);
        auto cooked = CookTerrainSourceArtifacts(source, profile, {}, {});
        REQUIRE(cooked.HasValue());
        return std::move(cooked).Value();
    }

    inline TerrainPayloadManifest Manifest(const CookedTerrainSourceArtifacts &terrain) {
        TerrainPayloadManifestRequest request;
        request.terrain = &terrain;
        request.content = Rev<TerrainContentRevision>();
        request.terrainRequirements = {TerrainPayloadRequirement::Required, TerrainPayloadRequirement::Required,
                                       TerrainPayloadRequirement::Required};
        auto result = GenerateTerrainPayloadManifest(request);
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    struct Fixture final {
        TerrainCanonicalSource source;
        CookedTerrainSourceArtifacts terrain;
        TerrainPayloadManifest manifest;
        std::vector<TerrainProducerTileSelection> selections;

        explicit Fixture(const bool holed = false) : source(Source(holed)), terrain(Cook(source)), manifest(Manifest(terrain)) {}

        TerrainProducerSnapshotRequest Request(const TerrainProducerConsumer consumer = TerrainProducerConsumer::Collision,
                                               const std::uint64_t attempt = 1) {
            const auto role = consumer == TerrainProducerConsumer::Collision ? TerrainSourceArtifactRole::Collision
                                                                             : TerrainSourceArtifactRole::Navigation;
            selections.clear();
            for (const auto &artifact : terrain.Artifacts())
                if (artifact.role == role)
                    selections.push_back({artifact.tile, artifact.digest});
            TerrainProducerSnapshotRequest request;
            request.lifecycle = TerrainRuntimeLifecycle::Active;
            request.terrain = &terrain;
            request.manifest = &manifest;
            request.tiles = selections;
            auto &h = request.header;
            h.consumer = consumer;
            h.terrain = {source.dataset, {0, 1}};
            h.revision = {Rev<TerrainContentRevision>(), Rev<TerrainResidencyRevision>(), Rev<TerrainMutationRevision>(),
                          source.capability};
            h.request = Rev<TerrainProducerRequestGeneration>(attempt);
            h.world = Id<WorldStreaming::WorldPartitionId>(11);
            h.cell = WorldStreaming::StreamingFence{h.world,
                                                    Rev<WorldStreaming::PartitionEpoch>(),
                                                    {-1, 0, -1, 0, WorldStreaming::StreamingLayerId::Create(0).Value()},
                                                    Rev<WorldStreaming::StreamingGeneration>()};
            h.origin = {Rev<WorldStreaming::OriginFrameId>(), Rev<WorldStreaming::OriginFrameRevision>(),
                        Rev<WorldStreaming::OriginGeneration>()};
            h.targetDigest = Digest(7);
            h.manifestDigest = manifest.Digest();
            constexpr std::array capabilities{TerrainFoliageCapability::TerrainRuntime, TerrainFoliageCapability::FoliageRuntime,
                                              TerrainFoliageCapability::PhysicsCollision, TerrainFoliageCapability::NavigationBlocking};
            h.capabilities = TerrainFoliageCapabilitySet::Create(capabilities).Value();
            return request;
        }
    };

    template <typename T> void ErrorIs(const Result<T> &result, const ErrorCodeDescriptor &expected) {
        Tests::RequireFailureIdentity(result, expected);
    }

}  // namespace Horo::Terrain::ProducerSnapshotTests
