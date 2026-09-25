#include "Horo/Terrain/TerrainFoliageRegistry.h"

#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <thread>
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
                                            const std::uint64_t boundsRevision = 3, const std::int64_t minimumXMillimeters = -1'000) {
            TerrainDatasetDescriptorData data{};
            data.dataset = Dataset(key);
            data.content = RevisionFrom<TerrainContentRevision>(content);
            data.bounds = {.revision = RevisionFrom<TerrainBoundsRevision>(boundsRevision),
                           .minimum = Math::WorldCoordinate64::FromMillimeters(minimumXMillimeters, -250, -2'000),
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
                                                       const std::uint64_t boundsRevision = 3,
                                                       const std::int64_t minimumXMillimeters = -1'000) {
            return {Descriptor(key, content, boundsRevision, minimumXMillimeters),
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

        auto unavailableFoliage = FoliageRegistration();
        unavailableFoliage.requiredCapabilities = Capabilities({TerrainFoliageCapability::RenderExtraction});
        RequireError(registry.RegisterFoliageType(std::move(unavailableFoliage)), TerrainErrors::CapabilityUnsupported);
    }

    TEST_CASE("Replacement advances publication and fences old handles without invalidating old snapshots", "[unit][terrain][registry]") {
        auto registry = Registry(Capabilities({TerrainFoliageCapability::TerrainQuery}));
        REQUIRE(registry.RegisterDataset(DatasetRegistration()).HasValue());
        const auto oldSnapshot = registry.Snapshot().Value();
        const auto oldHandle = oldSnapshot.FindDataset(Dataset()).Value();

        RequireError(registry.ReplaceDataset(DatasetRegistration("terrain/main", 10, 3)), TerrainErrors::RevisionStale);
        RequireError(registry.ReplaceDataset(DatasetRegistration("terrain/main", 13, 3)), TerrainErrors::RevisionStale);
        RequireError(registry.ReplaceDataset(DatasetRegistration("terrain/main", 12, 4)), TerrainErrors::RevisionStale);
        RequireError(registry.ReplaceDataset(DatasetRegistration("terrain/main", 12, 2, -900)), TerrainErrors::RevisionStale);
        RequireError(registry.ReplaceDataset(DatasetRegistration("terrain/main", 12, 5, -900)), TerrainErrors::RevisionStale);
        RequireError(registry.ReplaceDataset(DatasetRegistration("terrain/main", 12, 3, -900)), TerrainErrors::RevisionStale);
        REQUIRE(registry.ReplaceDataset(DatasetRegistration("terrain/main", 12, 3)).HasValue());
        REQUIRE(registry.ReplaceDataset(DatasetRegistration("terrain/main", 13, 4, -900)).HasValue());
        const auto newSnapshot = registry.Snapshot().Value();
        const auto newHandle = newSnapshot.FindDataset(Dataset()).Value();
        CHECK(newSnapshot.Binding().revision > oldSnapshot.Binding().revision);
        CHECK(oldSnapshot.Resolve(oldHandle).HasValue());
        RequireError(newSnapshot.Resolve(oldHandle), TerrainErrors::RegistryHandleStale);
        REQUIRE(newSnapshot.Resolve(newHandle).HasValue());
        CHECK(newSnapshot.Resolve(newHandle).Value()->descriptor.Data().content.Value() == 13);
    }

    TEST_CASE("Dataset removal publishes a new snapshot and preserves retained readers", "[unit][terrain][registry]") {
        auto registry = Registry(Capabilities({TerrainFoliageCapability::TerrainQuery}));
        REQUIRE(registry.RegisterDataset(DatasetRegistration()).HasValue());
        const auto retained = registry.Snapshot().Value();
        const auto oldHandle = retained.FindDataset(Dataset()).Value();
        const auto originalRevision = retained.Binding().revision;

        RequireError(registry.UnregisterDataset({}), TerrainErrors::IdentityInvalid);
        const auto missing = registry.UnregisterDataset(Dataset("terrain/missing"));
        REQUIRE(missing.HasValue());
        CHECK_FALSE(missing.Value());
        CHECK(registry.Snapshot().Value().Binding().revision == originalRevision);

        const auto removed = registry.UnregisterDataset(Dataset());
        REQUIRE(removed.HasValue());
        CHECK(removed.Value());
        const auto empty = registry.Snapshot().Value();
        CHECK(empty.Binding().revision > originalRevision);
        CHECK(empty.Datasets().empty());
        RequireError(empty.FindDataset(Dataset()), TerrainErrors::IdentityUnknown);
        CHECK(retained.Resolve(oldHandle).HasValue());
        CHECK_FALSE(registry.UnregisterDataset(Dataset()).Value());

        REQUIRE(registry.RegisterDataset(DatasetRegistration()).HasValue());
        const auto restored = registry.Snapshot().Value();
        RequireError(restored.Resolve(oldHandle), TerrainErrors::RegistryHandleStale);
        CHECK(restored.FindDataset(Dataset()).HasValue());
    }

    TEST_CASE("Foliage removal fences handles and closes with the registry", "[unit][terrain][registry]") {
        auto registry = Registry(Capabilities({TerrainFoliageCapability::FoliageQuery}));
        REQUIRE(registry.RegisterFoliageType(FoliageRegistration()).HasValue());
        const auto retained = registry.Snapshot().Value();
        const auto oldHandle = retained.FindFoliageType(FoliageType()).Value();

        RequireError(registry.UnregisterFoliageType({}), TerrainErrors::IdentityInvalid);
        const auto missing = registry.UnregisterFoliageType(FoliageType(8));
        REQUIRE(missing.HasValue());
        CHECK_FALSE(missing.Value());

        const auto removed = registry.UnregisterFoliageType(FoliageType());
        REQUIRE(removed.HasValue());
        CHECK(removed.Value());
        const auto empty = registry.Snapshot().Value();
        CHECK(empty.FoliageTypes().empty());
        CHECK(empty.Binding().revision > retained.Binding().revision);
        CHECK(retained.Resolve(oldHandle).HasValue());
        CHECK_FALSE(registry.UnregisterFoliageType(FoliageType()).Value());

        registry.BeginCancellation();
        RequireError(registry.UnregisterFoliageType(FoliageType()), TerrainErrors::RegistryClosed);
        CHECK(retained.Resolve(oldHandle).HasValue());
    }

    TEST_CASE("Foliage type replacement requires its exact non-wrapping successor", "[unit][terrain][registry]") {
        auto registry = Registry(Capabilities({TerrainFoliageCapability::FoliageQuery}));
        REQUIRE(registry.RegisterFoliageType(FoliageRegistration()).HasValue());

        RequireError(registry.ReplaceFoliageType(FoliageRegistration(7, 8)), TerrainErrors::RevisionStale);
        RequireError(registry.ReplaceFoliageType(FoliageRegistration(7, 11)), TerrainErrors::RevisionStale);
        REQUIRE(registry.ReplaceFoliageType(FoliageRegistration(7, 10)).HasValue());

        const auto snapshot = registry.Snapshot();
        REQUIRE(snapshot.HasValue());
        const auto type = snapshot.Value().FindFoliageType(FoliageType());
        REQUIRE(type.HasValue());
        CHECK(type.Value().revision.Value() == 10);
    }

    TEST_CASE("Dataset and foliage semantic revisions do not wrap", "[unit][terrain][registry]") {
        const auto maximum = std::numeric_limits<std::uint64_t>::max();
        auto terrain = Registry(Capabilities({TerrainFoliageCapability::TerrainQuery}));
        REQUIRE(terrain.RegisterDataset(DatasetRegistration("terrain/main", maximum)).HasValue());
        RequireError(terrain.ReplaceDataset(DatasetRegistration("terrain/main", 1)), TerrainErrors::GenerationExhausted);

        auto foliage = Registry(Capabilities({TerrainFoliageCapability::FoliageQuery}));
        REQUIRE(foliage.RegisterFoliageType(FoliageRegistration(7, maximum)).HasValue());
        RequireError(foliage.ReplaceFoliageType(FoliageRegistration(7, 1)), TerrainErrors::GenerationExhausted);
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

    TEST_CASE("Queries enforce the snapshot result ceiling even with larger output spans", "[unit][terrain][registry]") {
        const auto capabilities = Capabilities({TerrainFoliageCapability::TerrainQuery, TerrainFoliageCapability::FoliageQuery});
        auto registry = Registry(capabilities, {2, 2, 1});
        REQUIRE(registry.RegisterDataset(DatasetRegistration()).HasValue());
        REQUIRE(registry.RegisterDataset(DatasetRegistration("terrain/secondary", 12)).HasValue());
        REQUIRE(registry.RegisterFoliageType(FoliageRegistration()).HasValue());
        REQUIRE(registry.RegisterFoliageType(FoliageRegistration(8)).HasValue());

        const auto snapshot = registry.Snapshot().Value();
        std::array<TerrainDatasetRegistryHandle, 2> datasetOutput{};
        const auto datasetsBefore = datasetOutput;
        RequireError(snapshot.QueryDatasets({}, datasetOutput), TerrainErrors::CapacityExceeded);
        CHECK(datasetOutput == datasetsBefore);

        std::array<TerrainFoliageTypeRegistryHandle, 2> foliageOutput{};
        const auto foliageBefore = foliageOutput;
        RequireError(snapshot.QueryFoliageTypes({}, foliageOutput), TerrainErrors::CapacityExceeded);
        CHECK(foliageOutput == foliageBefore);
    }

    TEST_CASE("Snapshot capture remains coherent during owner-thread publication", "[unit][terrain][registry]") {
        auto registry = Registry(Capabilities({TerrainFoliageCapability::TerrainQuery}));
        REQUIRE(registry.RegisterDataset(DatasetRegistration()).HasValue());

        std::atomic<bool> done{};
        std::atomic<bool> coherent{true};
        std::thread reader{[&] {
            do {
                const auto snapshot = registry.Snapshot();
                if (snapshot.HasError() || snapshot.Value().Datasets().size() != 1) {
                    coherent.store(false, std::memory_order_relaxed);
                    return;
                }
                const auto publication = snapshot.Value().Binding().revision.Value();
                const auto content = snapshot.Value().Datasets().front().descriptor.Data().content.Value();
                if (publication + 9U != content) {
                    coherent.store(false, std::memory_order_relaxed);
                    return;
                }
            } while (!done.load(std::memory_order_acquire));
        }};

        for (std::uint64_t content = 12; content < 268; ++content) {
            if (registry.ReplaceDataset(DatasetRegistration("terrain/main", content)).HasError()) {
                coherent.store(false, std::memory_order_relaxed);
                break;
            }
            std::this_thread::yield();
        }
        done.store(true, std::memory_order_release);
        reader.join();

        CHECK(coherent.load(std::memory_order_relaxed));
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
