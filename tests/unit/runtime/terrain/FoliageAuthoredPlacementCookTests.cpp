#include "FoliageAuthoringTestSupport.h"
#include "Horo/Terrain/FoliageClusterCook.h"
#include "Horo/Terrain/FoliagePlacementCook.h"

#include <algorithm>
#include <limits>

namespace Horo::Terrain {
    namespace {
        using namespace FoliageAuthoringTest;

        struct AuthoredFixture final {
            std::vector<FoliagePlacementSample> samples{16};
            FoliageAuthoringSource source = Source();
            FoliageTypeDefinition definition = FoliageTypeDefinition::Create(Type().definition, Configuration(), Capabilities()).Value();

            AuthoredFixture() {
                for (auto &sample : samples) {
                    sample.density = 65'535;
                    sample.altitudeMillimeters = 1'000;
                }
            }

            FoliageAuthoredPlacementCookRequest Request(std::uint64_t revision = 1) const {
                FoliagePlacementGrid grid;
                grid.tile = {Id<TerrainDatasetId>(1), {0, 0, 0}};
                grid.sourceRevision = Rev<TerrainSourceRevision>(1);
                grid.capability = Rev<TerrainCapabilityRevision>(1);
                grid.spacingXMillimeters = 1'000;
                grid.spacingZMillimeters = 1'000;
                grid.width = 4;
                grid.height = 4;
                grid.samples = samples;
                FoliagePlacementCookRequest target{grid,
                                                   definition,
                                                   Capabilities(),
                                                   {.version = 1, .cellsPerCluster = 2},
                                                   {},
                                                   Rev<TerrainContentRevision>(revision)};
                return {target, source.placements, 0, 0};
            }
        };
    }  // namespace

    TEST_CASE("Authored foliage cooking preserves IDs through canonical order and rebake", "[foliage-authoring][cook]") {
        AuthoredFixture fixture;
        auto first = CookAuthoredFoliagePlacement(fixture.Request());
        REQUIRE(first.HasValue());
        REQUIRE(first.Value().Instances().size() == 2);
        CHECK(first.Value().Instances()[0].id == fixture.source.placements[0].id);
        CHECK(first.Value().Instances()[0].altitudeMillimeters == 1'250);
        std::ranges::reverse(fixture.source.placements);
        auto reordered = CookAuthoredFoliagePlacement(fixture.Request());
        REQUIRE(reordered.HasValue());
        CHECK(reordered.Value().ResultDigest() == first.Value().ResultDigest());
        CHECK(reordered.Value().Fingerprint() == first.Value().Fingerprint());
        fixture.samples[5].altitudeMillimeters = 1'100;
        auto rebaked = CookAuthoredFoliagePlacement(fixture.Request(2));
        REQUIRE(rebaked.HasValue());
        CHECK(rebaked.Value().Instances()[0].id == first.Value().Instances()[0].id);
        CHECK(rebaked.Value().Instances()[0].altitudeMillimeters == 1'350);
        FoliagePlacementCookOwner owner;
        REQUIRE(owner.Publish(first.Value(), {}).HasValue());
        REQUIRE(owner.Publish(rebaked.Value(), Rev<TerrainContentRevision>(1)).HasValue());
        CHECK(owner.Current()->Instances()[0].id == first.Value().Instances()[0].id);
    }

    TEST_CASE("Authored placements enter the actual sealed cluster cook with stable instance IDs", "[foliage-authoring][cook]") {
        AuthoredFixture fixture;
        auto request = fixture.Request();
        Sha256Digest target{}, toolchain{}, geometry{};
        target.bytes[0] = 1;
        toolchain.bytes[0] = 2;
        geometry.bytes[0] = 3;
        request.target.targetDigest = target;
        request.target.toolchainDigest = toolchain;
        auto placement = CookAuthoredFoliagePlacement(request);
        REQUIRE(placement.HasValue());
        FoliageClusterCookSource input{&placement.Value(), geometry, 500};
        FoliageClusterCookRequest clusters{Id<TerrainDatasetId>(1),
                                           Rev<TerrainContentRevision>(1),
                                           {Configuration(), target, toolchain},
                                           std::span{&input, 1}};
        auto cooked = CookFoliageClusters(clusters, {});
        REQUIRE(cooked.HasValue());
        std::vector<FoliageInstanceId> actual;
        for (const auto &cluster : cooked.Value().Clusters())
            for (const auto &instance : cluster.instances)
                actual.push_back(instance.id);
        std::ranges::sort(actual);
        REQUIRE(actual.size() == 2);
        CHECK(actual[0] == fixture.source.placements[0].id);
        CHECK(actual[1] == fixture.source.placements[1].id);
    }

    TEST_CASE("Authored foliage rejection preserves old sealed output and intent", "[foliage-authoring][cook]") {
        AuthoredFixture fixture;
        auto first = CookAuthoredFoliagePlacement(fixture.Request());
        REQUIRE(first.HasValue());
        FoliagePlacementCookOwner owner;
        REQUIRE(owner.Publish(first.Value(), {}).HasValue());
        const auto before = owner.Current()->ResultDigest();
        fixture.source.placements[1].id = fixture.source.placements[0].id;
        CHECK(CookAuthoredFoliagePlacement(fixture.Request(2)).HasError());
        CHECK(owner.Current()->ResultDigest() == before);
        fixture.source = Source();
        fixture.samples[5].hole = true;
        CHECK(CookAuthoredFoliagePlacement(fixture.Request(2)).HasError());
        CHECK(owner.Current()->ResultDigest() == before);
        fixture.samples[5].hole = false;
        fixture.samples[5].altitudeMillimeters = std::numeric_limits<std::int64_t>::max();
        CHECK(CookAuthoredFoliagePlacement(fixture.Request(2)).HasError());
        fixture.samples[5].altitudeMillimeters = 1'000;
        auto request = fixture.Request(2);
        request.target.limits.maximumInstances = 1;
        CHECK(CookAuthoredFoliagePlacement(request).HasError());
        CancellationSource cancellation;
        cancellation.RequestCancellation();
        CHECK(CookAuthoredFoliagePlacement(fixture.Request(2), cancellation.Token()).HasError());
        owner.Close();
        CHECK(owner.Publish(first.Value(), Rev<TerrainContentRevision>(1)).HasError());
        CHECK(owner.Current()->ResultDigest() == before);
    }
}  // namespace Horo::Terrain
