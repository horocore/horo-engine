#include "Horo/Terrain/FoliagePlacementCook.h"

#include <algorithm>
#include <array>
#include <barrier>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Terrain {
    namespace {
        template <typename Identity> Identity Id(const std::uint8_t marker) {
            SerializedTerrainIdentity bytes{};
            bytes[0] = marker;
            return Identity::Create(bytes).Value();
        }

        template <typename Revision> Revision Rev(const std::uint64_t value) {
            return Revision::Create(value).Value();
        }

        constexpr FoliageDefinitionCapabilitySet Capabilities() {
            return {FoliageDefinitionCapabilityBit<FoliageDefinitionCapability::CpuCulling>};
        }

        FoliageTypeDefinition Definition(const std::uint64_t seed = 7U, const std::uint32_t density = 10'000U,
                                         const FoliageSurfaceAlignment alignment = FoliageSurfaceAlignment::Upright) {
            auto profile = GetTerrainTierProfile(TerrainFeatureTier::Baseline).Value();
            auto configuration = TerrainConfigurationSnapshot::Create({.configuration = Rev<TerrainConfigurationRevision>(1),
                                                                       .capability = Rev<TerrainCapabilityRevision>(1),
                                                                       .tier = TerrainFeatureTier::Baseline,
                                                                       .limits = profile.limits});
            REQUIRE(configuration.HasValue());
            FoliageTypeDefinitionData data;
            data.type = Id<FoliageTypeId>(2);
            data.revision = Rev<FoliageDefinitionRevision>(1);
            data.assets.meshLods[0] = Id<FoliageMeshAssetId>(3);
            data.assets.meshLodCount = 1;
            data.assets.material = Id<FoliageMaterialAssetId>(4);
            data.placement.seed = seed;
            data.placement.densityPerSquareKilometer = density;
            data.placement.minimumAltitudeMillimeters = 0;
            data.placement.maximumAltitudeMillimeters = 2'000;
            data.placement.minimumSlopeMilliDegrees = 0;
            data.placement.maximumSlopeMilliDegrees = 40'000U;
            data.placement.minimumSeparationMillimeters = 100U;
            data.placement.coordinateQuantumMillimeters = 10U;
            data.placement.alignment = alignment;
            data.culling.meshLodDistanceMillimeters[0] = 10'000U;
            data.culling.cullDistanceMillimeters = 20'000U;
            data.maximumInstances = 1'000U;
            auto definition = FoliageTypeDefinition::Create(data, configuration.Value(), Capabilities());
            REQUIRE(definition.HasValue());
            return std::move(definition).Value();
        }

        struct Fixture final {
            std::vector<FoliagePlacementSample> samples = std::vector<FoliagePlacementSample>(25U);
            std::vector<FoliageExclusionRectangle> exclusions{};
            FoliageTypeDefinition definition = Definition();

            Fixture() {
                for (auto &sample : samples) {
                    sample.altitudeMillimeters = 1'000;
                    sample.density = 65'535U;
                }
            }

            FoliagePlacementCookRequest Request(const std::uint64_t revision = 1U) const {
                FoliagePlacementGrid grid;
                grid.tile = {.dataset = Id<TerrainDatasetId>(1), .tile = {0, 0, 0}};
                grid.sourceRevision = Rev<TerrainSourceRevision>(1);
                grid.capability = Rev<TerrainCapabilityRevision>(1);
                grid.spacingXMillimeters = 10'000U;
                grid.spacingZMillimeters = 10'000U;
                grid.width = 5U;
                grid.height = 5U;
                grid.samples = samples;
                grid.exclusions = exclusions;
                return {grid,
                        definition,
                        Capabilities(),
                        {.version = 1, .cellsPerCluster = 2, .radiusMillimeters = 2'000},
                        {},
                        Rev<TerrainContentRevision>(revision),
                        {},
                        {}};
            }
        };

        template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &descriptor) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().domain.Value() == descriptor.domain.Value());
            CHECK(result.ErrorValue().code.Value() == descriptor.code.Value());
            CHECK(result.ErrorValue().severity == descriptor.defaultSeverity);
        }
    }  // namespace

    TEST_CASE("Foliage cook is identical for equivalent sources and exclusion order", "[terrain][foliage][cook]") {
        Fixture fixture;
        fixture.exclusions = {{1'000, 1'000, 4'000, 4'000}, {30'000, 30'000, 35'000, 35'000}};
        const auto first = CookFoliagePlacement(fixture.Request(), {});
        REQUIRE(first.HasValue());
        REQUIRE_FALSE(first.Value().Instances().empty());
        std::ranges::reverse(fixture.exclusions);
        const auto repeated = CookFoliagePlacement(fixture.Request(), {});
        REQUIRE(repeated.HasValue());
        CHECK(repeated.Value().Fingerprint() == first.Value().Fingerprint());
        CHECK(repeated.Value().ResultDigest() == first.Value().ResultDigest());
        CHECK(std::ranges::equal(repeated.Value().Instances(), first.Value().Instances()));
        for (const auto &instance : first.Value().Instances()) {
            CHECK(instance.id.IsValid());
            CHECK(instance.cluster.IsValid());
            CHECK(instance.type == fixture.definition.Data().type);
            CHECK(instance.altitudeMillimeters == 1'000);
        }
    }

    TEST_CASE("Foliage placement respects clustering separation and transform invariants", "[terrain][foliage][cook]") {
        Fixture fixture;
        fixture.definition = Definition(7U, 10'000U, FoliageSurfaceAlignment::SurfaceNormal);
        for (auto &sample : fixture.samples) {
            sample.altitudeMillimeters = 2'000;
            sample.slopeMilliDegrees = 40'000U;
            sample.normalXPermille = 100;
            sample.normalYPermille = 995;
        }
        const auto placed = CookFoliagePlacement(fixture.Request(), {});
        REQUIRE(placed.HasValue());
        REQUIRE_FALSE(placed.Value().Instances().empty());
        const auto instances = placed.Value().Instances();
        for (std::size_t index = 0; index < instances.size(); ++index) {
            const auto &instance = instances[index];
            CHECK(instance.altitudeMillimeters == 2'000);
            CHECK(instance.slopeMilliDegrees == 40'000U);
            CHECK(instance.normalXPermille == 100);
            CHECK(instance.normalYPermille == 995);
            CHECK(instance.normalZPermille == 0);
            CHECK(instance.scalePermille >= fixture.definition.Data().minimumScalePermille);
            CHECK(instance.scalePermille <= fixture.definition.Data().maximumScalePermille);
            CHECK(instance.yawMilliDegrees < 360'000U);
            const auto centerX = (instance.xMillimeters / 20'000) * 20'000 + 10'000;
            const auto centerZ = (instance.zMillimeters / 20'000) * 20'000 + 10'000;
            const auto clusterX = instance.xMillimeters - centerX;
            const auto clusterZ = instance.zMillimeters - centerZ;
            CHECK(clusterX * clusterX + clusterZ * clusterZ <= 2'020LL * 2'020LL);
            for (std::size_t prior = 0; prior < index; ++prior) {
                const auto dx = instance.xMillimeters - instances[prior].xMillimeters;
                const auto dz = instance.zMillimeters - instances[prior].zMillimeters;
                CHECK(dx * dx + dz * dz >= 100LL * 100LL);
                CHECK(instance.id != instances[prior].id);
            }
        }
    }

    TEST_CASE("Foliage placement admits inclusive altitude and slope boundaries", "[terrain][foliage][cook]") {
        Fixture fixture;
        for (auto &sample : fixture.samples) {
            sample.altitudeMillimeters = 2'000;
            sample.slopeMilliDegrees = 40'000U;
        }
        const auto upper = CookFoliagePlacement(fixture.Request(), {});
        REQUIRE(upper.HasValue());
        CHECK_FALSE(upper.Value().Instances().empty());
        for (auto &sample : fixture.samples)
            sample.altitudeMillimeters = 2'001;
        const auto above = CookFoliagePlacement(fixture.Request(), {});
        REQUIRE(above.HasValue());
        CHECK(above.Value().Instances().empty());
        for (auto &sample : fixture.samples) {
            sample.altitudeMillimeters = 2'000;
            sample.slopeMilliDegrees = 40'001U;
        }
        const auto tooSteep = CookFoliagePlacement(fixture.Request(), {});
        REQUIRE(tooSteep.HasValue());
        CHECK(tooSteep.Value().Instances().empty());
        for (auto &sample : fixture.samples) {
            sample.altitudeMillimeters = 0;
            sample.slopeMilliDegrees = 0;
        }
        const auto lower = CookFoliagePlacement(fixture.Request(), {});
        REQUIRE(lower.HasValue());
        CHECK_FALSE(lower.Value().Instances().empty());
        for (auto &sample : fixture.samples)
            sample.altitudeMillimeters = -1;
        const auto below = CookFoliagePlacement(fixture.Request(), {});
        REQUIRE(below.HasValue());
        CHECK(below.Value().Instances().empty());
    }

    TEST_CASE("Foliage independent worker cooks emit the same complete generation", "[terrain][foliage][cook]") {
        Fixture fixture;
        const auto request = fixture.Request();
        std::array<std::optional<Result<CookedFoliagePlacement>>, 2> results;
        std::jthread first([&results, &request] {
            results[0].emplace(CookFoliagePlacement(request, {}));
        });
        std::jthread second([&results, &request] {
            results[1].emplace(CookFoliagePlacement(request, {}));
        });
        first.join();
        second.join();
        REQUIRE(results[0].has_value());
        REQUIRE(results[1].has_value());
        REQUIRE(results[0]->HasValue());
        REQUIRE(results[1]->HasValue());
        CHECK(results[0]->Value().Fingerprint() == results[1]->Value().Fingerprint());
        CHECK(results[0]->Value().ResultDigest() == results[1]->Value().ResultDigest());
        CHECK(std::ranges::equal(results[0]->Value().Instances(), results[1]->Value().Instances()));
    }

    TEST_CASE("Foliage cook fingerprints every authored placement input", "[terrain][foliage][cook]") {
        Fixture fixture;
        const auto baseline = CookFoliagePlacement(fixture.Request(), {});
        REQUIRE(baseline.HasValue());
        fixture.samples[0].normalXPermille = 100;
        fixture.samples[0].normalYPermille = 995;
        const auto normalChanged = CookFoliagePlacement(fixture.Request(), {});
        REQUIRE(normalChanged.HasValue());
        CHECK(normalChanged.Value().Fingerprint() != baseline.Value().Fingerprint());
        fixture.samples[0].hole = true;
        const auto holeChanged = CookFoliagePlacement(fixture.Request(), {});
        REQUIRE(holeChanged.HasValue());
        CHECK(holeChanged.Value().Fingerprint() != normalChanged.Value().Fingerprint());
        fixture.samples[0].density = 0U;
        const auto densityChanged = CookFoliagePlacement(fixture.Request(), {});
        REQUIRE(densityChanged.HasValue());
        CHECK(densityChanged.Value().Fingerprint() != holeChanged.Value().Fingerprint());
        fixture.exclusions.emplace_back(0, 0, 9'000, 9'000);
        const auto excluded = CookFoliagePlacement(fixture.Request(), {});
        REQUIRE(excluded.HasValue());
        CHECK(excluded.Value().Fingerprint() != densityChanged.Value().Fingerprint());
    }

    TEST_CASE("Foliage cook fingerprints rule and publication envelope", "[terrain][foliage][cook]") {
        Fixture fixture;
        const auto baseline = CookFoliagePlacement(fixture.Request(), {});
        REQUIRE(baseline.HasValue());
        auto ruleChanged = fixture.Request();
        ruleChanged.clustering.radiusMillimeters = 1'000U;
        const auto clustered = CookFoliagePlacement(ruleChanged, {});
        REQUIRE(clustered.HasValue());
        CHECK(clustered.Value().Fingerprint() != baseline.Value().Fingerprint());
        auto toolchainChanged = fixture.Request();
        toolchainChanged.toolchainDigest.bytes[0] = 1U;
        const auto toolchain = CookFoliagePlacement(toolchainChanged, {});
        REQUIRE(toolchain.HasValue());
        CHECK(toolchain.Value().Fingerprint() != baseline.Value().Fingerprint());
        auto dependencyChanged = fixture.Request();
        dependencyChanged.layerDependencyDigest.bytes[0] = 1U;
        const auto dependency = CookFoliagePlacement(dependencyChanged, {});
        REQUIRE(dependency.HasValue());
        CHECK(dependency.Value().Fingerprint() != baseline.Value().Fingerprint());
        dependencyChanged = fixture.Request();
        dependencyChanged.splineDependencyDigest.bytes[0] = 1U;
        const auto spline = CookFoliagePlacement(dependencyChanged, {});
        REQUIRE(spline.HasValue());
        CHECK(spline.Value().Fingerprint() != baseline.Value().Fingerprint());
        auto tierChanged = fixture.Request();
        tierChanged.tier = TerrainFeatureTier::Standard;
        const auto tier = CookFoliagePlacement(tierChanged, {});
        REQUIRE(tier.HasValue());
        CHECK(tier.Value().Fingerprint() != baseline.Value().Fingerprint());
        fixture.definition = Definition(8U);
        const auto seedChanged = CookFoliagePlacement(fixture.Request(), {});
        REQUIRE(seedChanged.HasValue());
        CHECK(seedChanged.Value().Fingerprint() != baseline.Value().Fingerprint());
    }

    TEST_CASE("Foliage republication changes provenance without rearranging baked identities", "[terrain][foliage][cook]") {
        Fixture fixture;
        const auto first = CookFoliagePlacement(fixture.Request(), {});
        REQUIRE(first.HasValue());
        auto next = fixture.Request(2U);
        next.grid.sourceRevision = Rev<TerrainSourceRevision>(2U);
        next.grid.capability = Rev<TerrainCapabilityRevision>(2U);
        next.targetDigest.bytes[0] = 7U;
        next.toolchainDigest.bytes[0] = 9U;
        const auto republished = CookFoliagePlacement(next, {});
        REQUIRE(republished.HasValue());
        CHECK(republished.Value().Fingerprint() != first.Value().Fingerprint());
        CHECK(republished.Value().ResultDigest() == first.Value().ResultDigest());
        CHECK(std::ranges::equal(republished.Value().Instances(), first.Value().Instances()));
    }

    TEST_CASE("Foliage density budget is exact and negative quantization stays canonical", "[terrain][foliage][cook]") {
        Fixture fixture;
        auto request = fixture.Request();
        request.grid.originXMillimeters = -20'000;
        request.grid.originZMillimeters = -20'000;
        request.limits.maximumCandidates = 16U;
        const auto placed = CookFoliagePlacement(request, {});
        REQUIRE(placed.HasValue());
        REQUIRE_FALSE(placed.Value().Instances().empty());
        for (const auto &instance : placed.Value().Instances()) {
            CHECK(instance.xMillimeters >= -20'000);
            CHECK(instance.zMillimeters >= -20'000);
            CHECK(instance.xMillimeters < 20'000);
            CHECK(instance.zMillimeters < 20'000);
            CHECK(instance.xMillimeters % 10 == 0);
            CHECK(instance.zMillimeters % 10 == 0);
        }
        request.limits.maximumCandidates = 15U;
        RequireError(CookFoliagePlacement(request, {}), FoliagePlacementCookErrors::LimitExceeded);
    }

    TEST_CASE("Foliage altitude slope density holes and exclusion suppress placements", "[terrain][foliage][cook]") {
        Fixture fixture;
        const auto baseline = CookFoliagePlacement(fixture.Request(), {});
        REQUIRE(baseline.HasValue());
        REQUIRE_FALSE(baseline.Value().Instances().empty());
        for (auto &sample : fixture.samples)
            sample.altitudeMillimeters = 3'000;
        CHECK(CookFoliagePlacement(fixture.Request(), {}).Value().Instances().empty());
        for (auto &sample : fixture.samples) {
            sample.altitudeMillimeters = 1'000;
            sample.slopeMilliDegrees = 50'000U;
        }
        CHECK(CookFoliagePlacement(fixture.Request(), {}).Value().Instances().empty());
        for (auto &sample : fixture.samples) {
            sample.slopeMilliDegrees = 0U;
            sample.density = 0U;
        }
        CHECK(CookFoliagePlacement(fixture.Request(), {}).Value().Instances().empty());
        for (auto &sample : fixture.samples) {
            sample.density = 65'535U;
            sample.hole = true;
        }
        CHECK(CookFoliagePlacement(fixture.Request(), {}).Value().Instances().empty());
        for (auto &sample : fixture.samples)
            sample.hole = false;
        fixture.exclusions.emplace_back(0, 0, 40'000, 40'000);
        CHECK(CookFoliagePlacement(fixture.Request(), {}).Value().Instances().empty());
    }

    TEST_CASE("Foliage constraints and bounded work reject without partial output", "[terrain][foliage][cook]") {
        Fixture fixture;
        auto request = fixture.Request();
        request.limits.maximumInstances = 1U;
        RequireError(CookFoliagePlacement(request, {}), FoliagePlacementCookErrors::LimitExceeded);
        request = fixture.Request();
        request.limits.maximumCandidates = 1U;
        RequireError(CookFoliagePlacement(request, {}), FoliagePlacementCookErrors::LimitExceeded);
        request = fixture.Request();
        request.limits.maximumClusters = 1U;
        RequireError(CookFoliagePlacement(request, {}), FoliagePlacementCookErrors::LimitExceeded);
        request = fixture.Request();
        request.limits.maximumPredicateChecks = 1U;
        RequireError(CookFoliagePlacement(request, {}), FoliagePlacementCookErrors::LimitExceeded);
        request = fixture.Request();
        request.grid.samples = std::span{fixture.samples}.first(24U);
        RequireError(CookFoliagePlacement(request, {}), FoliagePlacementCookErrors::LimitExceeded);
        request = fixture.Request();
        request.grid.spacingXMillimeters = UINT32_MAX;
        request.grid.spacingZMillimeters = UINT32_MAX;
        RequireError(CookFoliagePlacement(request, {}), FoliagePlacementCookErrors::LimitExceeded);
        request = fixture.Request();
        request.clustering.cellsPerCluster = 0U;
        RequireError(CookFoliagePlacement(request, {}), FoliagePlacementCookErrors::InvalidInput);
        request = fixture.Request();
        request.clustering.cellsPerCluster = 5U;
        RequireError(CookFoliagePlacement(request, {}), FoliagePlacementCookErrors::InvalidInput);
        request = fixture.Request();
        request.capabilities = {};
        RequireError(CookFoliagePlacement(request, {}), FoliagePlacementCookErrors::InvalidInput);
        fixture.exclusions = {{0, 0, 10, 10}, {0, 0, 10, 10}};
        RequireError(CookFoliagePlacement(fixture.Request(), {}), FoliagePlacementCookErrors::InvalidInput);
    }

    TEST_CASE("Foliage owner keeps prior generation on cancellation, stale replacement and close", "[terrain][foliage][cook]") {
        Fixture fixture;
        auto first = CookFoliagePlacement(fixture.Request(), {});
        REQUIRE(first.HasValue());
        FoliagePlacementCookOwner owner;
        REQUIRE(owner.Publish(std::move(first).Value(), std::nullopt).HasValue());
        const auto previousDigest = owner.Current()->ResultDigest();
        CancellationSource cancelled;
        cancelled.RequestCancellation();
        RequireError(CookFoliagePlacement(fixture.Request(2U), cancelled.Token()), FoliagePlacementCookErrors::Cancelled);
        CHECK(owner.Current()->ResultDigest() == previousDigest);
        auto budgetLimited = fixture.Request(2U);
        budgetLimited.limits.maximumPredicateChecks = 1U;
        RequireError(CookFoliagePlacement(budgetLimited, {}), FoliagePlacementCookErrors::LimitExceeded);
        CHECK(owner.Current()->ResultDigest() == previousDigest);
        CHECK(owner.Current()->ContentRevision() == Rev<TerrainContentRevision>(1U));
        auto second = CookFoliagePlacement(fixture.Request(2U), {});
        REQUIRE(second.HasValue());
        RequireError(owner.Publish(second.Value(), Rev<TerrainContentRevision>(9U)), FoliagePlacementCookErrors::Stale);
        CHECK(owner.Current()->ContentRevision() == Rev<TerrainContentRevision>(1U));
        REQUIRE(owner.Publish(std::move(second).Value(), Rev<TerrainContentRevision>(1U)).HasValue());
        CHECK(owner.Current()->ContentRevision() == Rev<TerrainContentRevision>(2U));
        owner.Close();
        auto third = CookFoliagePlacement(fixture.Request(3U), {});
        REQUIRE(third.HasValue());
        RequireError(owner.Publish(std::move(third).Value(), Rev<TerrainContentRevision>(2U)), FoliagePlacementCookErrors::Closed);
        CHECK(owner.Current()->ContentRevision() == Rev<TerrainContentRevision>(2U));
    }

    TEST_CASE("Foliage dense single-cell cook observes concurrent cancellation", "[terrain][foliage][cook]") {
        Fixture fixture;
        fixture.definition = Definition(7U, 100'000U);
        for (auto &sample : fixture.samples)
            sample.altitudeMillimeters = 3'000;
        for (std::int64_t index = 0; index < 100; ++index)
            fixture.exclusions.emplace_back(2'000'000 + index * 10, 2'000'000, 2'000'005 + index * 10, 2'000'005);
        auto request = fixture.Request();
        request.grid.width = 2U;
        request.grid.height = 2U;
        request.grid.spacingXMillimeters = 1'000'000U;
        request.grid.spacingZMillimeters = 1'000'000U;
        request.grid.samples = std::span{fixture.samples}.first(4U);
        request.clustering.cellsPerCluster = 1U;
        request.limits.maximumCandidates = 100'000U;
        CancellationSource source;
        std::barrier start{2};
        std::optional<Result<CookedFoliagePlacement>> result;
        std::jthread worker([&start, &result, &request, &source] {
            start.arrive_and_wait();
            result.emplace(CookFoliagePlacement(request, source.Token()));
        });
        start.arrive_and_wait();
        source.RequestCancellation();
        worker.join();
        REQUIRE(result.has_value());
        RequireError(*result, FoliagePlacementCookErrors::Cancelled);
    }
}  // namespace Horo::Terrain
