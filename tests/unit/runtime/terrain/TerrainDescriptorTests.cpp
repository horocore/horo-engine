#include "Horo/Terrain/TerrainDescriptor.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string_view>
#include <type_traits>

namespace Horo::Terrain {
    namespace {
        std::span<const std::byte> Key(const std::string_view value) {
            return std::as_bytes(std::span{value.data(), value.size()});
        }

        TerrainProjectId Project() {
            SerializedTerrainIdentity bytes{};
            bytes.front() = 1;
            return TerrainProjectId::Create(bytes).Value();
        }

        TerrainDatasetId Dataset(const std::string_view key = "terrain/main") {
            return DeriveTerrainDatasetId(Project(), Key(key)).Value();
        }

        template <typename Revision> Revision MakeRevision(const std::uint64_t value) {
            return Revision::Create(value).Value();
        }

        TerrainConfigurationSnapshotData ConfigurationData(const TerrainFeatureTier tier = TerrainFeatureTier::Baseline) {
            const TerrainTierProfile profile = GetTerrainTierProfile(tier).Value();
            return {.configuration = MakeRevision<TerrainConfigurationRevision>(5),
                    .capability = MakeRevision<TerrainCapabilityRevision>(7),
                    .tier = tier,
                    .limits = profile.limits};
        }

        TerrainDatasetDescriptorData DescriptorData(const std::uint64_t content = 11, const std::uint64_t boundsRevision = 3) {
            return {.dataset = Dataset(),
                    .content = MakeRevision<TerrainContentRevision>(content),
                    .bounds = {.revision = MakeRevision<TerrainBoundsRevision>(boundsRevision),
                               .minimum = Math::WorldCoordinate64::FromMillimeters(-1'000, -250, -2'000),
                               .maximum = Math::WorldCoordinate64::FromMillimeters(4'000, 750, 3'000)},
                    .grid = {.samplesX = 1'025, .samplesZ = 2'049, .tileInteriorQuads = 128, .lodLevels = 4, .layersPerTile = 4},
                    .footprint = {.activeTerrainTiles = 64,
                                  .activeFoliageClusters = 128,
                                  .activeFoliageInstances = 32'768,
                                  .residentTerrainBytes = 64ULL * 1024 * 1024,
                                  .residentFoliageBytes = 32ULL * 1024 * 1024,
                                  .stagingBytes = 16ULL * 1024 * 1024,
                                  .retiringBytes = 16ULL * 1024 * 1024,
                                  .workItems = 131'072}};
        }

        TerrainFeatureTierSet Tiers(const std::uint8_t bits = TerrainFeatureTierBit<TerrainFeatureTier::Baseline>) {
            return {bits};
        }

        TerrainDescriptorAdmissionContext Context(const TerrainConfigurationSnapshotData &configuration) {
            return {.lifecycle = TerrainRuntimeLifecycle::Active,
                    .configuration = configuration.configuration,
                    .capability = configuration.capability,
                    .supportedTiers = Tiers()};
        }

        TerrainDatasetDescriptorData ExactLimitDescriptor(const TerrainConfigurationSnapshotData &configuration) {
            auto descriptor = DescriptorData();
            descriptor.grid = {.samplesX = configuration.limits.maximumSamplesPerAxis,
                               .samplesZ = configuration.limits.maximumSamplesPerAxis,
                               .tileInteriorQuads = configuration.limits.maximumTileInteriorQuads,
                               .lodLevels = configuration.limits.maximumLodLevels,
                               .layersPerTile = configuration.limits.maximumLayersPerTile};
            descriptor.footprint = {.activeTerrainTiles = configuration.limits.maximumActiveTerrainTiles,
                                    .activeFoliageClusters = configuration.limits.maximumActiveFoliageClusters,
                                    .activeFoliageInstances = configuration.limits.maximumActiveFoliageInstances,
                                    .residentTerrainBytes = configuration.limits.maximumResidentTerrainBytes,
                                    .residentFoliageBytes = configuration.limits.maximumResidentFoliageBytes,
                                    .stagingBytes = configuration.limits.maximumStagingBytes,
                                    .retiringBytes = configuration.limits.maximumRetiringBytes,
                                    .workItems = configuration.limits.maximumWorkItems};
            return descriptor;
        }

        template <typename Value> void RequireError(const Result<Value> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            const Error &error = result.ErrorValue();
            CHECK(error.domain.Value() == expected.domain.Value());
            CHECK(error.code.Value() == expected.code.Value());
        }
    }  // namespace

    TEST_CASE("Terrain tier profiles expose exact bounded versioned ceilings", "[unit][terrain][descriptor]") {
        const auto baseline = GetTerrainTierProfile(TerrainFeatureTier::Baseline).Value();
        const auto standard = GetTerrainTierProfile(TerrainFeatureTier::Standard).Value();
        const auto high = GetTerrainTierProfile(TerrainFeatureTier::High).Value();
        const auto ultra = GetTerrainTierProfile(TerrainFeatureTier::Ultra).Value();

        CHECK(baseline.revision == CurrentTerrainTierProfileRevision);
        CHECK(baseline.limits.maximumActiveTerrainTiles == 256);
        CHECK(standard.limits.maximumActiveTerrainTiles == 512);
        CHECK(high.limits.maximumActiveTerrainTiles == 1'024);
        CHECK(ultra.limits.maximumActiveTerrainTiles == TerrainDescriptorHardLimits::ActiveTerrainTiles);
        CHECK(ultra.limits.maximumActiveFoliageInstances == TerrainDescriptorHardLimits::ActiveFoliageInstances);
        CHECK(ultra.limits.maximumResidentTerrainBytes == TerrainDescriptorHardLimits::ResidentTerrainBytes);
        RequireError(GetTerrainTierProfile(TerrainFeatureTier::Count), TerrainErrors::TierInvalid);
        RequireError(GetTerrainTierProfile(static_cast<TerrainFeatureTier>(255)), TerrainErrors::TierInvalid);
    }

    TEST_CASE("Exact tier resolution never silently downgrades", "[unit][terrain][descriptor]") {
        const TerrainFeatureTierSet baselineAndHigh{static_cast<std::uint8_t>(TerrainFeatureTierBit<TerrainFeatureTier::Baseline> |
                                                                              TerrainFeatureTierBit<TerrainFeatureTier::High>)};
        CHECK(ResolveTerrainFeatureTier(TerrainFeatureTier::High, baselineAndHigh).Value() == TerrainFeatureTier::High);
        RequireError(ResolveTerrainFeatureTier(TerrainFeatureTier::Standard, baselineAndHigh), TerrainErrors::TierUnsupported);
        RequireError(ResolveTerrainFeatureTier(TerrainFeatureTier::Count, baselineAndHigh), TerrainErrors::TierInvalid);
        RequireError(ResolveTerrainFeatureTier(TerrainFeatureTier::Baseline, {}), TerrainErrors::TierInvalid);
    }

    TEST_CASE("Configuration snapshot owns fixed size inert data", "[unit][terrain][descriptor]") {
        const TerrainConfigurationSnapshotData source = ConfigurationData();
        const auto snapshot = TerrainConfigurationSnapshot::Create(source);
        REQUIRE(snapshot.HasValue());
        CHECK(snapshot.Value().Data() == source);
        static_assert(!std::is_default_constructible_v<TerrainConfigurationSnapshot>);
        static_assert(std::is_copy_constructible_v<TerrainConfigurationSnapshot>);
        static_assert(std::is_trivially_copyable_v<TerrainConfigurationSnapshot>);
    }

    TEST_CASE("Configuration rejects invalid revisions versions and tier limits", "[unit][terrain][descriptor]") {
        auto data = ConfigurationData();
        data.contractVersion++;
        RequireError(TerrainConfigurationSnapshot::Create(data), TerrainErrors::DescriptorInvalid);
        data = ConfigurationData();
        data.tierProfileRevision++;
        RequireError(TerrainConfigurationSnapshot::Create(data), TerrainErrors::DescriptorInvalid);
        data = ConfigurationData();
        data.configuration = {};
        RequireError(TerrainConfigurationSnapshot::Create(data), TerrainErrors::DescriptorInvalid);
        data = ConfigurationData();
        data.limits.maximumActiveTerrainTiles = 0;
        RequireError(TerrainConfigurationSnapshot::Create(data), TerrainErrors::LimitProfileInvalid);
        data = ConfigurationData();
        data.limits.maximumActiveTerrainTiles++;
        RequireError(TerrainConfigurationSnapshot::Create(data), TerrainErrors::LimitProfileInvalid);
        data = ConfigurationData();
        data.limits.maximumTileInteriorQuads = 127;
        RequireError(TerrainConfigurationSnapshot::Create(data), TerrainErrors::LimitProfileInvalid);
        data = ConfigurationData();
        data.limits.maximumActiveFoliageClusters = 0;
        RequireError(TerrainConfigurationSnapshot::Create(data), TerrainErrors::LimitProfileInvalid);

        data = ConfigurationData();
        data.limits.maximumActiveFoliageClusters = 0;
        data.limits.maximumActiveFoliageInstances = 0;
        data.limits.maximumResidentFoliageBytes = 0;
        const auto withoutFoliage = TerrainConfigurationSnapshot::Create(data);
        REQUIRE(withoutFoliage.HasValue());
        auto foliageDescriptor = DescriptorData();
        RequireError(TerrainDatasetDescriptor::Create(foliageDescriptor, withoutFoliage.Value()), TerrainErrors::LimitExceeded);
        foliageDescriptor.footprint.activeFoliageClusters = 0;
        foliageDescriptor.footprint.activeFoliageInstances = 0;
        foliageDescriptor.footprint.residentFoliageBytes = 0;
        CHECK(TerrainDatasetDescriptor::Create(foliageDescriptor, withoutFoliage.Value()).HasValue());
    }

    TEST_CASE("Shared descriptor validates normal Terrain and Foliage footprint", "[unit][terrain][descriptor]") {
        const auto configuration = TerrainConfigurationSnapshot::Create(ConfigurationData()).Value();
        const TerrainDatasetDescriptorData source = DescriptorData();
        const auto descriptor = TerrainDatasetDescriptor::Create(source, configuration);
        REQUIRE(descriptor.HasValue());
        CHECK(descriptor.Value().Data() == source);
        static_assert(!std::is_default_constructible_v<TerrainDatasetDescriptor>);
        static_assert(std::is_trivially_copyable_v<TerrainDatasetDescriptor>);

        auto terrainOnly = source;
        terrainOnly.footprint.activeFoliageClusters = 0;
        terrainOnly.footprint.activeFoliageInstances = 0;
        terrainOnly.footprint.residentFoliageBytes = 0;
        CHECK(TerrainDatasetDescriptor::Create(terrainOnly, configuration).HasValue());
    }

    TEST_CASE("Descriptor rejects invalid identities bounds dimensions and foliage shape", "[unit][terrain][descriptor]") {
        const auto configuration = TerrainConfigurationSnapshot::Create(ConfigurationData()).Value();
        auto data = DescriptorData();
        data.dataset = {};
        RequireError(TerrainDatasetDescriptor::Create(data, configuration), TerrainErrors::DescriptorInvalid);
        data = DescriptorData();
        data.bounds.maximum = Math::WorldCoordinate64::FromMillimeters(-1'000, 750, 3'000);
        RequireError(TerrainDatasetDescriptor::Create(data, configuration), TerrainErrors::DescriptorInvalid);
        data = DescriptorData();
        data.bounds.maximum = Math::WorldCoordinate64::FromMillimeters(4'000, -250, 3'000);
        CHECK(TerrainDatasetDescriptor::Create(data, configuration).HasValue());
        data = DescriptorData();
        data.bounds.revision = {};
        RequireError(TerrainDatasetDescriptor::Create(data, configuration), TerrainErrors::DescriptorInvalid);
        data = DescriptorData();
        data.grid.samplesX = 1;
        RequireError(TerrainDatasetDescriptor::Create(data, configuration), TerrainErrors::DescriptorInvalid);
        data = DescriptorData();
        data.grid.tileInteriorQuads = 96;
        RequireError(TerrainDatasetDescriptor::Create(data, configuration), TerrainErrors::DescriptorInvalid);
        data = DescriptorData();
        data.grid.samplesX++;
        RequireError(TerrainDatasetDescriptor::Create(data, configuration), TerrainErrors::DescriptorInvalid);
        data = DescriptorData();
        data.footprint.activeFoliageInstances = 0;
        RequireError(TerrainDatasetDescriptor::Create(data, configuration), TerrainErrors::DescriptorInvalid);
    }

    TEST_CASE("Descriptor accepts the exact tier limits", "[unit][terrain][descriptor]") {
        const TerrainConfigurationSnapshotData configurationData = ConfigurationData();
        const auto configuration = TerrainConfigurationSnapshot::Create(configurationData).Value();
        const TerrainDatasetDescriptorData exact = ExactLimitDescriptor(configurationData);
        REQUIRE(TerrainDatasetDescriptor::Create(exact, configuration).HasValue());
    }

    TEST_CASE("Descriptor rejects every one-over grid boundary", "[unit][terrain][descriptor]") {
        const TerrainConfigurationSnapshotData configurationData = ConfigurationData();
        const auto configuration = TerrainConfigurationSnapshot::Create(configurationData).Value();
        const TerrainDatasetDescriptorData exact = ExactLimitDescriptor(configurationData);
        auto over = exact;
        over.grid.samplesX++;
        RequireError(TerrainDatasetDescriptor::Create(over, configuration), TerrainErrors::DescriptorInvalid);
        over = exact;
        over.grid.samplesZ++;
        RequireError(TerrainDatasetDescriptor::Create(over, configuration), TerrainErrors::DescriptorInvalid);
        over = exact;
        over.grid.tileInteriorQuads++;
        RequireError(TerrainDatasetDescriptor::Create(over, configuration), TerrainErrors::DescriptorInvalid);
        over = exact;
        over.grid.lodLevels++;
        RequireError(TerrainDatasetDescriptor::Create(over, configuration), TerrainErrors::DescriptorInvalid);
        over = exact;
        over.grid.layersPerTile++;
        RequireError(TerrainDatasetDescriptor::Create(over, configuration), TerrainErrors::DescriptorInvalid);
    }

    TEST_CASE("Descriptor rejects every one-over resource boundary", "[unit][terrain][descriptor]") {
        const TerrainConfigurationSnapshotData configurationData = ConfigurationData();
        const auto configuration = TerrainConfigurationSnapshot::Create(configurationData).Value();
        const TerrainDatasetDescriptorData exact = ExactLimitDescriptor(configurationData);
        auto over = exact;
        over.footprint.activeTerrainTiles++;
        RequireError(TerrainDatasetDescriptor::Create(over, configuration), TerrainErrors::LimitExceeded);
        over = exact;
        over.footprint.activeFoliageClusters++;
        RequireError(TerrainDatasetDescriptor::Create(over, configuration), TerrainErrors::LimitExceeded);
        over = exact;
        over.footprint.activeFoliageInstances++;
        RequireError(TerrainDatasetDescriptor::Create(over, configuration), TerrainErrors::LimitExceeded);
        over = exact;
        over.footprint.residentTerrainBytes++;
        RequireError(TerrainDatasetDescriptor::Create(over, configuration), TerrainErrors::LimitExceeded);
        over = exact;
        over.footprint.residentFoliageBytes++;
        RequireError(TerrainDatasetDescriptor::Create(over, configuration), TerrainErrors::LimitExceeded);
        over = exact;
        over.footprint.stagingBytes++;
        RequireError(TerrainDatasetDescriptor::Create(over, configuration), TerrainErrors::LimitExceeded);
        over = exact;
        over.footprint.retiringBytes++;
        RequireError(TerrainDatasetDescriptor::Create(over, configuration), TerrainErrors::LimitExceeded);
        over = exact;
        over.footprint.workItems++;
        RequireError(TerrainDatasetDescriptor::Create(over, configuration), TerrainErrors::LimitExceeded);
    }

    TEST_CASE("Insert admission is exact tier revision and lifecycle fenced", "[unit][terrain][descriptor]") {
        const TerrainConfigurationSnapshotData configurationData = ConfigurationData();
        const auto configuration = TerrainConfigurationSnapshot::Create(configurationData).Value();
        const auto descriptor = TerrainDatasetDescriptor::Create(DescriptorData(), configuration).Value();
        auto context = Context(configurationData);
        REQUIRE(ValidateTerrainDescriptorAdmission(descriptor, configuration, {}, context).HasValue());

        context.configuration = MakeRevision<TerrainConfigurationRevision>(6);
        RequireError(ValidateTerrainDescriptorAdmission(descriptor, configuration, {}, context), TerrainErrors::RevisionStale);
        context = Context(configurationData);
        context.capability = MakeRevision<TerrainCapabilityRevision>(8);
        RequireError(ValidateTerrainDescriptorAdmission(descriptor, configuration, {}, context), TerrainErrors::RevisionStale);
        context = Context(configurationData);
        context.supportedTiers = Tiers(TerrainFeatureTierBit<TerrainFeatureTier::High>);
        RequireError(ValidateTerrainDescriptorAdmission(descriptor, configuration, {}, context), TerrainErrors::TierUnsupported);
        for (const TerrainRuntimeLifecycle lifecycle :
             {TerrainRuntimeLifecycle::Cancelled, TerrainRuntimeLifecycle::Closing, TerrainRuntimeLifecycle::Closed}) {
            context = Context(configurationData);
            context.lifecycle = lifecycle;
            RequireError(ValidateTerrainDescriptorAdmission(descriptor, configuration, {}, context), TerrainErrors::LifecycleUnavailable);
        }
        context = Context(configurationData);
        context.currentDataset = Dataset();
        RequireError(ValidateTerrainDescriptorAdmission(descriptor, configuration, {}, context), TerrainErrors::ReplacementInvalid);
    }

    TEST_CASE("Replacement requires exact successor content and coherent bounds revision", "[unit][terrain][descriptor]") {
        const TerrainConfigurationSnapshotData configurationData = ConfigurationData();
        const auto configuration = TerrainConfigurationSnapshot::Create(configurationData).Value();
        auto candidateData = DescriptorData(12, 3);
        auto candidate = TerrainDatasetDescriptor::Create(candidateData, configuration).Value();
        auto context = Context(configurationData);
        context.currentDataset = Dataset();
        context.currentContent = MakeRevision<TerrainContentRevision>(11);
        context.currentBounds = DescriptorData(11, 3).bounds;
        TerrainDescriptorAdmissionRequest request{TerrainDescriptorAdmissionKind::Replace, MakeRevision<TerrainContentRevision>(11)};
        REQUIRE(ValidateTerrainDescriptorAdmission(candidate, configuration, request, context).HasValue());

        request.expectedCurrentContent = MakeRevision<TerrainContentRevision>(10);
        RequireError(ValidateTerrainDescriptorAdmission(candidate, configuration, request, context), TerrainErrors::RevisionStale);
        request.expectedCurrentContent = MakeRevision<TerrainContentRevision>(11);
        context.currentDataset = Dataset("terrain/other");
        RequireError(ValidateTerrainDescriptorAdmission(candidate, configuration, request, context), TerrainErrors::IdentityUnknown);
        context.currentDataset = Dataset();

        candidateData = DescriptorData(13, 3);
        candidate = TerrainDatasetDescriptor::Create(candidateData, configuration).Value();
        RequireError(ValidateTerrainDescriptorAdmission(candidate, configuration, request, context), TerrainErrors::RevisionStale);

        candidateData = DescriptorData(12, 3);
        candidateData.bounds.maximum = Math::WorldCoordinate64::FromMillimeters(4'001, 750, 3'000);
        candidate = TerrainDatasetDescriptor::Create(candidateData, configuration).Value();
        RequireError(ValidateTerrainDescriptorAdmission(candidate, configuration, request, context), TerrainErrors::RevisionStale);
        candidateData.bounds.revision = MakeRevision<TerrainBoundsRevision>(4);
        candidate = TerrainDatasetDescriptor::Create(candidateData, configuration).Value();
        REQUIRE(ValidateTerrainDescriptorAdmission(candidate, configuration, request, context).HasValue());

        candidateData = DescriptorData(12, 4);
        candidate = TerrainDatasetDescriptor::Create(candidateData, configuration).Value();
        RequireError(ValidateTerrainDescriptorAdmission(candidate, configuration, request, context), TerrainErrors::RevisionStale);
    }

    TEST_CASE("Replacement fences missing state and exhausted revisions", "[unit][terrain][descriptor]") {
        const TerrainConfigurationSnapshotData configurationData = ConfigurationData();
        const auto configuration = TerrainConfigurationSnapshot::Create(configurationData).Value();
        const auto candidate = TerrainDatasetDescriptor::Create(DescriptorData(12), configuration).Value();
        auto context = Context(configurationData);
        TerrainDescriptorAdmissionRequest request{TerrainDescriptorAdmissionKind::Replace, MakeRevision<TerrainContentRevision>(11)};
        RequireError(ValidateTerrainDescriptorAdmission(candidate, configuration, request, context), TerrainErrors::ReplacementInvalid);

        context.currentDataset = Dataset();
        context.currentContent = MakeRevision<TerrainContentRevision>(std::numeric_limits<std::uint64_t>::max());
        context.currentBounds = DescriptorData().bounds;
        request.expectedCurrentContent = context.currentContent;
        RequireError(ValidateTerrainDescriptorAdmission(candidate, configuration, request, context), TerrainErrors::GenerationExhausted);

        context.currentContent = MakeRevision<TerrainContentRevision>(11);
        request.expectedCurrentContent = context.currentContent;
        context.currentBounds->revision = MakeRevision<TerrainBoundsRevision>(std::numeric_limits<std::uint64_t>::max());
        auto changedBoundsData = DescriptorData(12);
        changedBoundsData.bounds.maximum = Math::WorldCoordinate64::FromMillimeters(4'001, 750, 3'000);
        const auto changedBounds = TerrainDatasetDescriptor::Create(changedBoundsData, configuration).Value();
        RequireError(ValidateTerrainDescriptorAdmission(changedBounds, configuration, request, context),
                     TerrainErrors::GenerationExhausted);

        request.kind = TerrainDescriptorAdmissionKind::Count;
        RequireError(ValidateTerrainDescriptorAdmission(candidate, configuration, request, context), TerrainErrors::DescriptorInvalid);
    }

    TEST_CASE("Terrain descriptor errors participate in the stable registry", "[unit][terrain][descriptor]") {
        const auto descriptors = TerrainErrors::Descriptors();
        CHECK(descriptors.size() == 37);
        CHECK(std::ranges::find(descriptors, &TerrainErrors::DescriptorInvalid) != descriptors.end());
        CHECK(std::ranges::find(descriptors, &TerrainErrors::TierUnsupported) != descriptors.end());
        CHECK(std::ranges::find(descriptors, &TerrainErrors::RevisionStale) != descriptors.end());
    }
}  // namespace Horo::Terrain
