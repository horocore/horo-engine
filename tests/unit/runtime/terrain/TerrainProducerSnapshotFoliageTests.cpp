#include "TerrainProducerSnapshotTestSupport.h"

#include <catch2/generators/catch_generators.hpp>

namespace Horo::Terrain {
    using namespace ProducerSnapshotTests;

    namespace {
        TerrainConfigurationSnapshot Configuration() {
            return TerrainConfigurationSnapshot::Create({.configuration = Rev<TerrainConfigurationRevision>(),
                                                         .capability = Rev<TerrainCapabilityRevision>(),
                                                         .limits = GetTerrainTierProfile(TerrainFeatureTier::Baseline).Value().limits})
                .Value();
        }

        constexpr FoliageDefinitionCapabilitySet DefinitionCapabilities{
            FoliageDefinitionCapabilityBit<FoliageDefinitionCapability::CpuCulling> |
            FoliageDefinitionCapabilityBit<FoliageDefinitionCapability::Collision> |
            FoliageDefinitionCapabilityBit<FoliageDefinitionCapability::NavigationBlocking>};

        FoliageTypeDefinition Definition(const FoliageCollisionShape shape, const bool blocksNavigation = true,
                                         const std::uint64_t revision = 1) {
            FoliageTypeDefinitionData data;
            data.type = Id<FoliageTypeId>(2);
            data.revision = Rev<FoliageDefinitionRevision>(revision);
            data.assets = {.meshLods = {Id<FoliageMeshAssetId>(3)}, .meshLodCount = 1, .material = Id<FoliageMaterialAssetId>(4)};
            data.placement = {.alignment = FoliageSurfaceAlignment::SurfaceNormal,
                              .seed = 7,
                              .densityPerSquareKilometer = 100'000,
                              .maximumAltitudeMillimeters = 2'000,
                              .maximumSlopeMilliDegrees = 40'000,
                              .minimumSeparationMillimeters = 100,
                              .coordinateQuantumMillimeters = 10};
            data.culling = {.meshLodDistanceMillimeters = {10'000}, .cullDistanceMillimeters = 20'000};
            data.maximumInstances = 1'000;
            data.minimumScalePermille = data.maximumScalePermille = 1'101;
            if (shape != FoliageCollisionShape::None)
                data.collision = {shape, 250, 2'000, true, blocksNavigation};
            auto result = FoliageTypeDefinition::Create(data, Configuration(), DefinitionCapabilities);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        CookedFoliageClusterSet Foliage(const TerrainTileId tile, const FoliageTypeDefinition &definition) {
            std::vector<FoliagePlacementSample> samples(25);
            for (auto &sample : samples) {
                sample.altitudeMillimeters = 1'000;
                sample.density = 65'535;
                sample.slopeMilliDegrees = 10'000;
                sample.normalXPermille = -100;
                sample.normalYPermille = 995;
            }
            FoliagePlacementGrid grid{.tile = tile,
                                      .sourceRevision = Rev<TerrainSourceRevision>(),
                                      .capability = Rev<TerrainCapabilityRevision>(),
                                      .originXMillimeters = -20'000,
                                      .originZMillimeters = -20'000,
                                      .spacingXMillimeters = 10'000,
                                      .spacingZMillimeters = 10'000,
                                      .width = 5,
                                      .height = 5,
                                      .samples = samples};
            const FoliagePlacementCookRequest
                request{grid,      definition, DefinitionCapabilities, {.cellsPerCluster = 2}, {}, Rev<TerrainContentRevision>(),
                        Digest(7), Digest(8)};
            auto placement = CookFoliagePlacement(request, {});
            REQUIRE(placement.HasValue());
            const FoliageClusterCookSource source{&placement.Value(), Digest(9), 3'000};
            const FoliageClusterCookRequest clusterRequest{tile.dataset,
                                                           Rev<TerrainContentRevision>(),
                                                           {Configuration(), Digest(7), Digest(8)},
                                                           std::span{&source, 1}};
            auto cooked = CookFoliageClusters(clusterRequest, {});
            REQUIRE(cooked.HasValue());
            return std::move(cooked).Value();
        }

        TerrainPayloadManifest WithFoliage(const Fixture &fixture, const CookedFoliageClusterSet &foliage) {
            const std::array dependencies{TerrainTileCookDependency{fixture.source.sourceAsset, Digest(9)}};
            TerrainPayloadManifestRequest request;
            request.terrain = &fixture.terrain;
            request.foliage = &foliage;
            request.content = Rev<TerrainContentRevision>();
            request.foliageDependencies = dependencies;
            request.verifiedFoliageDependencyClosure = TerrainPayloadDependencyClosureDigest(dependencies).Value();
            request.terrainRequirements =
                request.foliageRequirements = {TerrainPayloadRequirement::Required, TerrainPayloadRequirement::Required,
                                               TerrainPayloadRequirement::Required};
            auto manifest = GenerateTerrainPayloadManifest(request);
            REQUIRE(manifest.HasValue());
            return std::move(manifest).Value();
        }

        void RejectInvalidFoliageInputs(TerrainProducerSnapshotRequest request, const Fixture &fixture, const FoliageCollisionShape shape,
                                        CookedFoliageClusterSet &foliage, const int malformed) {
            const auto admitted = request;
            request.limits.maximumInstances = 1;
            ErrorIs(CaptureTerrainProducerSnapshot(request), TerrainProducerErrors::Limit);
            request = admitted;
            const std::array staleDefinitions{Definition(shape, true, 2)};
            request.definitions = staleDefinitions;
            ErrorIs(CaptureTerrainProducerSnapshot(request), TerrainProducerErrors::Stale);
            request = admitted;
            request.manifest = &fixture.manifest;
            request.header.manifestDigest = fixture.manifest.Digest();
            ErrorIs(CaptureTerrainProducerSnapshot(request), TerrainProducerErrors::Stale);
            request = admitted;
            auto &instance = const_cast<CookedFoliageCluster &>(foliage.Clusters().front()).instances.front();
            const auto original = instance;
            switch (malformed) {
                case 0:
                    instance.id = {};
                    break;
                case 1:
                    instance.scalePermille = 0;
                    break;
                default:
                    instance.normalYPermille = 0;
                    break;
            }
            ErrorIs(CaptureTerrainProducerSnapshot(request), TerrainProducerErrors::Invalid);
            instance = original;
        }
    }  // namespace

    TEST_CASE("Producer foliage preserves exact primitives and integer placement without native conversion",
              "[terrain][producer][foliage]") {
        Fixture fixture;
        const auto shape = GENERATE(FoliageCollisionShape::Cylinder, FoliageCollisionShape::Capsule, FoliageCollisionShape::None);
        const auto malformed = GENERATE(0, 1, 2);
        const std::array definitions{Definition(shape)};
        auto foliage = Foliage(fixture.terrain.Tiles().tiles.front().id, definitions.front());
        const auto manifest = WithFoliage(fixture, foliage);
        std::vector<TerrainProducerClusterSelection> selections;
        for (const auto &cluster : foliage.Clusters())
            selections.push_back({cluster.id, cluster.digest});
        REQUIRE_FALSE(selections.empty());
        for (const auto consumer : {TerrainProducerConsumer::Collision, TerrainProducerConsumer::Navigation}) {
            auto request = fixture.Request(consumer);
            request.manifest = &manifest;
            request.header.manifestDigest = manifest.Digest();
            request.foliage = &foliage;
            request.clusters = selections;
            request.definitions = definitions;
            auto snapshot = CaptureTerrainProducerSnapshot(request);
            REQUIRE(snapshot.HasValue());
            if (shape == FoliageCollisionShape::None) {
                CHECK(snapshot.Value().Foliage().empty());
            } else {
                REQUIRE_FALSE(snapshot.Value().Foliage().empty());
                const auto &item = snapshot.Value().Foliage().front();
                CHECK(item.geometry == definitions.front().Data().collision);
                CHECK(item.alignment == FoliageSurfaceAlignment::SurfaceNormal);
                CHECK(item.instance.scalePermille == 1'101);
                CHECK(item.instance.normalXPermille == -100);
                CHECK(item.instance.normalYPermille == 995);
                CHECK(item.placementRevision == Rev<TerrainContentRevision>());
                CHECK(item.sourceRevision == fixture.source.revision);
                CHECK(item.instance == foliage.Clusters().front().instances.front());
            }
            RejectInvalidFoliageInputs(request, fixture, shape, foliage, malformed);
        }
    }

}  // namespace Horo::Terrain
