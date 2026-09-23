#include "Horo/Terrain/TerrainFoliageRegistry.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

namespace Horo::Terrain {
    namespace {
        template <typename Identity> Identity IdentityFrom(const std::uint8_t value) {
            SerializedTerrainIdentity bytes{};
            bytes.front() = value;
            return Identity::Create(bytes).Value();
        }

        template <typename Revision> Revision RevisionFrom(const std::uint64_t value) {
            return Revision::Create(value).Value();
        }

        TerrainProjectId Project() {
            return IdentityFrom<TerrainProjectId>(1);
        }

        TerrainDatasetId Dataset(const std::string_view key = "terrain/main") {
            return DeriveTerrainDatasetId(Project(), std::as_bytes(std::span{key.data(), key.size()})).Value();
        }

        FoliageTypeId FoliageType(const std::uint8_t value = 7) {
            return IdentityFrom<FoliageTypeId>(value);
        }

        TerrainConfigurationSnapshot Configuration() {
            const auto profile = GetTerrainTierProfile(TerrainFeatureTier::Baseline).Value();
            return TerrainConfigurationSnapshot::Create({.configuration = RevisionFrom<TerrainConfigurationRevision>(3),
                                                         .capability = RevisionFrom<TerrainCapabilityRevision>(5),
                                                         .tier = TerrainFeatureTier::Baseline,
                                                         .limits = profile.limits})
                .Value();
        }

        TerrainDatasetDescriptor Descriptor(const std::string_view key = "terrain/main", const std::uint64_t content = 11,
                                            const std::uint64_t boundsRevision = 3) {
            TerrainDatasetDescriptorData data{};
            data.dataset = Dataset(key);
            data.content = RevisionFrom<TerrainContentRevision>(content);
            data.bounds = {.revision = RevisionFrom<TerrainBoundsRevision>(boundsRevision),
                           .minimum = Math::WorldCoordinate64::FromMillimeters(-1'000, -250, -2'000),
                           .maximum = Math::WorldCoordinate64::FromMillimeters(4'000, 750, 3'000)};
            data.grid = {.samplesX = 1'025, .samplesZ = 2'049, .tileInteriorQuads = 128, .lodLevels = 4, .layersPerTile = 4};
            data.footprint = {.activeTerrainTiles = 64,
                              .activeFoliageClusters = 128,
                              .activeFoliageInstances = 32'768,
                              .residentTerrainBytes = 64ULL * 1024 * 1024,
                              .residentFoliageBytes = 32ULL * 1024 * 1024,
                              .stagingBytes = 16ULL * 1024 * 1024,
                              .retiringBytes = 16ULL * 1024 * 1024,
                              .workItems = 131'072};
            return TerrainDatasetDescriptor::Create(data, Configuration()).Value();
        }

        FoliageTypeDefinition Definition(const std::uint8_t identity = 7, const std::uint64_t revision = 9) {
            FoliageVisualAssets assets{};
            assets.meshLods[0] = IdentityFrom<FoliageMeshAssetId>(identity + 1U);
            assets.meshLodCount = 1;
            assets.material = IdentityFrom<FoliageMaterialAssetId>(identity + 2U);

            FoliageTypeDefinitionData data{};
            data.type = FoliageType(identity);
            data.revision = RevisionFrom<FoliageDefinitionRevision>(revision);
            data.assets = assets;
            data.placement = {.seed = 42,
                              .densityPerSquareKilometer = 50'000,
                              .minimumAltitudeMillimeters = -2'000,
                              .maximumAltitudeMillimeters = 8'000,
                              .minimumSlopeMilliDegrees = 0,
                              .maximumSlopeMilliDegrees = 45'000,
                              .minimumSeparationMillimeters = 500,
                              .coordinateQuantumMillimeters = 10};
            data.culling = {.meshLodDistanceMillimeters = {10'000, 0, 0, 0},
                            .cullDistanceMillimeters = 20'000,
                            .crossFadeDistanceMillimeters = 100};
            data.maximumInstances = 100;
            const auto capabilities =
                FoliageDefinitionCapabilitySet{FoliageDefinitionCapabilityBit<FoliageDefinitionCapability::CpuCulling>};
            return FoliageTypeDefinition::Create(data, Configuration(), capabilities).Value();
        }

        TerrainFoliageCapabilitySet Capabilities(const std::initializer_list<TerrainFoliageCapability> values) {
            return TerrainFoliageCapabilitySet::Create(std::span<const TerrainFoliageCapability>{values.begin(), values.size()}).Value();
        }

        TerrainFoliageRegistry Registry(const TerrainFoliageCapabilitySet capabilities, const TerrainFoliageRegistryLimits limits = {}) {
            return TerrainFoliageRegistry::Create(TerrainFoliageRegistryInstanceId::Create(19).Value(), capabilities, limits).Value();
        }

        TerrainDatasetRegistration DatasetRegistration(const std::string_view key = "terrain/main", const std::uint64_t content = 11,
                                                       const std::uint64_t boundsRevision = 3) {
            return {Descriptor(key, content, boundsRevision),
                    {static_cast<std::uint8_t>(TerrainFeatureTierBit<TerrainFeatureTier::Baseline> |
                                               TerrainFeatureTierBit<TerrainFeatureTier::Standard>)},
                    Capabilities({TerrainFoliageCapability::TerrainQuery})};
        }

        TerrainFoliageTypeRegistration FoliageRegistration(const std::uint8_t identity = 7, const std::uint64_t revision = 9) {
            return {Definition(identity, revision), Capabilities({TerrainFoliageCapability::FoliageQuery})};
        }

        template <typename Value> void RequireError(const Result<Value> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().domain.Value() == expected.domain.Value());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }
    }  // namespace

    TEST_CASE("Terrain/Foliage capability projection is explicit and never falls back", "[unit][terrain][registry]") {
        const auto available = Capabilities({TerrainFoliageCapability::TerrainQuery, TerrainFoliageCapability::FoliageQuery});
        const auto required = Capabilities({TerrainFoliageCapability::TerrainQuery});
        const auto projection = ProjectTerrainFoliageCapabilities(available, required);
        REQUIRE(projection.HasValue());
        CHECK(projection.Value().IsSatisfied());

        RequireError(ProjectTerrainFoliageCapabilities(available, Capabilities({TerrainFoliageCapability::RenderExtraction})),
                     TerrainErrors::CapabilityUnsupported);

        const std::array unknownCapability{
            static_cast<TerrainFoliageCapability>(static_cast<std::uint8_t>(TerrainFoliageCapability::Count))};
        const auto unknown = TerrainFoliageCapabilitySet::Create(unknownCapability);
        RequireError(unknown, TerrainErrors::RegistryDescriptorInvalid);
    }

    TEST_CASE("Registry publishes typed dataset and foliage snapshots with bounded queries", "[unit][terrain][registry]") {
        auto registry = Registry(Capabilities({TerrainFoliageCapability::TerrainQuery, TerrainFoliageCapability::FoliageQuery}));
        REQUIRE(registry.RegisterDataset(DatasetRegistration()).HasValue());
        REQUIRE(registry.RegisterFoliageType(FoliageRegistration()).HasValue());

        const auto snapshot = registry.Snapshot();
        REQUIRE(snapshot.HasValue());
        CHECK(snapshot.Value().Datasets().size() == 1);
        CHECK(snapshot.Value().FoliageTypes().size() == 1);
        const auto dataset = snapshot.Value().FindDataset(Dataset());
        const auto foliage = snapshot.Value().FindFoliageType(FoliageType());
        REQUIRE(dataset.HasValue());
        REQUIRE(foliage.HasValue());
        REQUIRE(snapshot.Value().Resolve(dataset.Value()).HasValue());
        REQUIRE(snapshot.Value().Resolve(foliage.Value()).HasValue());

        std::array<TerrainDatasetRegistryHandle, 1> datasetOutput{};
        const auto datasetQuery =
            snapshot.Value().QueryDatasets({.tier = TerrainFeatureTier::Standard,
                                            .requiredCapabilities = Capabilities({TerrainFoliageCapability::TerrainQuery})},
                                           datasetOutput);
        REQUIRE(datasetQuery.HasValue());
        CHECK(datasetQuery.Value().matches == 1);
        CHECK(datasetQuery.Value().candidatesExamined == 1);

        std::array<TerrainFoliageTypeRegistryHandle, 1> foliageOutput{};
        REQUIRE(snapshot.Value()
                    .QueryFoliageTypes({.requiredCapabilities = Capabilities({TerrainFoliageCapability::FoliageQuery})}, foliageOutput)
                    .HasValue());
    }

    TEST_CASE("Registry rejects duplicate, unavailable, and over-capacity registrations", "[unit][terrain][registry]") {
        auto registry = Registry(Capabilities({TerrainFoliageCapability::TerrainQuery}), {1, 1, 1});
        REQUIRE(registry.RegisterDataset(DatasetRegistration()).HasValue());
        RequireError(registry.RegisterDataset(DatasetRegistration()), TerrainErrors::RegistryDuplicate);
        RequireError(registry.RegisterDataset(DatasetRegistration("terrain/secondary")), TerrainErrors::CapacityExceeded);

        auto unavailable = DatasetRegistration();
        unavailable.requiredCapabilities = Capabilities({TerrainFoliageCapability::RenderExtraction});
        RequireError(registry.ReplaceDataset(std::move(unavailable)), TerrainErrors::CapabilityUnsupported);
    }

    TEST_CASE("Replacement advances publication and fences old handles without invalidating old snapshots", "[unit][terrain][registry]") {
        auto registry = Registry(Capabilities({TerrainFoliageCapability::TerrainQuery}));
        REQUIRE(registry.RegisterDataset(DatasetRegistration()).HasValue());
        const auto oldSnapshot = registry.Snapshot().Value();
        const auto oldHandle = oldSnapshot.FindDataset(Dataset()).Value();

        RequireError(registry.ReplaceDataset(DatasetRegistration("terrain/main", 11, 3)), TerrainErrors::RevisionStale);
        REQUIRE(registry.ReplaceDataset(DatasetRegistration("terrain/main", 12, 4)).HasValue());
        const auto newSnapshot = registry.Snapshot().Value();
        const auto newHandle = newSnapshot.FindDataset(Dataset()).Value();
        CHECK(newSnapshot.Binding().revision > oldSnapshot.Binding().revision);
        CHECK(oldSnapshot.Resolve(oldHandle).HasValue());
        RequireError(newSnapshot.Resolve(oldHandle), TerrainErrors::RegistryHandleStale);
        REQUIRE(newSnapshot.Resolve(newHandle).HasValue());
        CHECK(newSnapshot.Resolve(newHandle).Value()->descriptor.Data().content.Value() == 12);
    }

    TEST_CASE("Queries preserve output on bounded-capacity failure", "[unit][terrain][registry]") {
        auto registry = Registry(Capabilities({TerrainFoliageCapability::TerrainQuery}), {2, 1, 1});
        REQUIRE(registry.RegisterDataset(DatasetRegistration()).HasValue());
        REQUIRE(registry.RegisterDataset(DatasetRegistration("terrain/secondary", 12, 4)).HasValue());
        const auto snapshot = registry.Snapshot().Value();
        std::array<TerrainDatasetRegistryHandle, 1> output{};
        const auto before = output;
        RequireError(snapshot.QueryDatasets({}, output), TerrainErrors::CapacityExceeded);
        CHECK(output == before);
    }

    TEST_CASE("Cancellation and shutdown close admission while retained snapshots remain readable", "[unit][terrain][registry]") {
        auto registry = Registry(Capabilities({TerrainFoliageCapability::TerrainQuery}));
        REQUIRE(registry.RegisterDataset(DatasetRegistration()).HasValue());
        const auto retained = registry.Snapshot().Value();
        registry.BeginCancellation();
        CHECK(registry.Lifecycle() == TerrainFoliageRegistryState::Cancelling);
        RequireError(registry.Snapshot(), TerrainErrors::RegistryClosed);
        RequireError(registry.RegisterDataset(DatasetRegistration("terrain/secondary")), TerrainErrors::RegistryClosed);
        CHECK(retained.FindDataset(Dataset()).HasValue());
        registry.Shutdown();
        CHECK(registry.Lifecycle() == TerrainFoliageRegistryState::Closed);
        registry.Shutdown();
        CHECK(retained.IsValid());
    }
}  // namespace Horo::Terrain
