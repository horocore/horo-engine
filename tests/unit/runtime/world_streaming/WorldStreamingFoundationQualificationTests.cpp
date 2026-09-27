#include "Horo/WorldStreaming/FallbackStreamingProvider.h"
#include "Horo/WorldStreaming/WorldCellQuantization.h"
#include "Horo/WorldStreaming/WorldDependencyPlan.h"
#include "Horo/WorldStreaming/WorldPartitionCapabilityProfile.h"
#include "Horo/WorldStreaming/WorldPartitionRegistry.h"
#include "Horo/WorldStreaming/WorldSpatialAssignment.h"
#include "Horo/WorldStreaming/WorldSpatialObjectDescriptor.h"
#include "Horo/WorldStreaming/WorldStreamingErrors.h"
#include "Horo/WorldStreaming/WorldStreamingIdentity.h"
#include "WorldStreamingTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace Horo::WorldStreaming {
    namespace {
        using TestSupport::Asset;
        using TestSupport::IdentityFrom;
        using TestSupport::Layer;
        using TestSupport::RequireError;
        using TestSupport::World;

        [[nodiscard]] WorldCellQuantizationPolicy QualificationGrid() {
            const auto result = WorldCellQuantizationPolicy::Create({}, 1'000, {-2, 1, -1, 1, -1, 0}, 2);
            REQUIRE(result.HasValue());
            return result.Value();
        }

        [[nodiscard]] Result<WorldPartitionDescriptor> QualificationDescriptor(const WorldPartitionId partition,
                                                                               const std::uint8_t assetOffset = 0) {
            const auto base = Layer(2);
            const auto detail = Layer(7);
            const std::array layers{
                WorldLayerDescriptor{base, "base", WorldLayerOwnership::WorldStreaming, WorldLayerFlags::Persistent, 1.0F},
                WorldLayerDescriptor{detail, "detail", WorldLayerOwnership::GameplayScript, WorldLayerFlags::Optional, 0.5F},
            };
            const std::array cells{
                WorldPartitionCellDescriptor{{-1, 0, 0, 0, base}, {Asset(static_cast<std::uint8_t>(assetOffset + 1))}},
                WorldPartitionCellDescriptor{{0, 0, 0, 0, base}, {Asset(static_cast<std::uint8_t>(assetOffset + 2))}},
                WorldPartitionCellDescriptor{{1, 0, 0, 0, base}, {Asset(static_cast<std::uint8_t>(assetOffset + 3))}},
                WorldPartitionCellDescriptor{{0, 0, 0, 1, detail}, {Asset(static_cast<std::uint8_t>(assetOffset + 4))}},
            };
            return WorldPartitionDescriptor::Create({}, partition,
                                                    {Math::WorldCoordinate64::FromMillimeters(-2'000, -1'000, -1'000),
                                                     Math::WorldCoordinate64::FromMillimeters(1'999, 1'999, 999)},
                                                    QualificationGrid(), layers, cells, {4, 4, 128});
        }

        [[nodiscard]] StreamingRuntimeOwnerToken RuntimeOwner(const WorldPartitionId partition) {
            return {.partition = partition, .epoch = IdentityFrom<PartitionEpoch>(1), .owner = IdentityFrom<StreamingRuntimeOwnerId>(7)};
        }

        [[nodiscard]] StreamingSourceOwnerToken SourceOwner(const WorldPartitionId partition) {
            return {.partition = partition, .epoch = IdentityFrom<PartitionEpoch>(1), .slot = 3, .generation = 1};
        }

        [[nodiscard]] WorldPartitionCapabilitySnapshot Capabilities() {
            return {.capability = IdentityFrom<WorldPartitionCapabilityId>(11),
                    .revision = IdentityFrom<WorldPartitionCapabilityRevision>(12),
                    .minimumCellSizeMillimeters = 1'000,
                    .maximumCellSizeMillimeters = 1'024'000,
                    .maximumLodLevels = 8,
                    .maximumLayers = 4,
                    .maximumCells = 4,
                    .maximumLayerNameBytes = 128,
                    .maximumQueryResults = 4,
                    .precision = WorldPartitionPrecision::SignedMillimeter64,
                    .packages = WorldPartitionPackageCapabilities::StandaloneCellFile | WorldPartitionPackageCapabilities::ArchiveChunk};
        }

        [[nodiscard]] WorldPartitionProjectSettingsRequest SettingsRequest() {
            return {.settings = IdentityFrom<WorldPartitionSettingsId>(21),
                    .revision = IdentityFrom<WorldPartitionSettingsRevision>(22),
                    .profile = WorldPartitionProjectProfile::Standalone,
                    .baseCellSizeMillimeters = 1'000,
                    .lodLevels = 2,
                    .maximumLayers = 2,
                    .maximumCells = 4,
                    .maximumLayerNameBytes = 64,
                    .maximumQueryResults = 4,
                    .precision = WorldPartitionPrecision::SignedMillimeter64,
                    .packageMode = WorldPartitionPackageMode::ArchiveChunk};
        }

        TEST_CASE("World streaming foundation composes deterministic identities",
                  "[unit][world_streaming][qualification][headless][identity]") {
            const auto partition = World(42);
            const auto base = Layer(2);
            const auto grid = QualificationGrid();

            const auto atNegativeBoundary = QuantizeWorldToCell(Math::WorldCoordinate64::FromMillimeters(-1'000, 0, 0), grid, 0, base);
            REQUIRE(atNegativeBoundary.Value() == StreamingCellId{-1, 0, 0, 0, base});
            REQUIRE(QuantizeWorldToCell(Math::WorldCoordinate64::FromMillimeters(1'000, 0, 0), grid, 0, base).Value() ==
                    StreamingCellId{1, 0, 0, 0, base});
            REQUIRE(QuantizeWorldToCell(Math::WorldCoordinate64::FromMillimeters(-1'001, 0, 0), grid, 0, base).Value() ==
                    StreamingCellId{-2, 0, 0, 0, base});

            const auto largeCoordinateGrid =
                WorldCellQuantizationPolicy::Create(Math::WorldCoordinate64::FromMillimeters(std::numeric_limits<std::int64_t>::min(), 0,
                                                                                             0),
                                                    1'000, {0, 2, 0, 0, 0, 0}, 1);
            REQUIRE(largeCoordinateGrid.HasValue());
            REQUIRE(QuantizeWorldToCell(Math::WorldCoordinate64::FromMillimeters(std::numeric_limits<std::int64_t>::min() + 2'001, 0, 0),
                                        largeCoordinateGrid.Value(), 0, base)
                        .Value() == StreamingCellId{2, 0, 0, 0, base});
            RequireError(QuantizeWorldToCell(Math::WorldCoordinate64::FromMillimeters(std::numeric_limits<std::int64_t>::max(), 0, 0),
                                             largeCoordinateGrid.Value(), 0, base),
                         WorldStreamingErrors::CoordinateOutOfRange);

            const StreamingCellId identityCell{-1, 0, 0, 1, Layer(7)};
            REQUIRE(DeserializeWorldPartitionId(SerializeWorldPartitionId(partition)).Value() == partition);
            REQUIRE(DeserializeStreamingCellId(SerializeStreamingCellId(identityCell)).Value() == identityCell);
            REQUIRE(DeserializeStreamingSourceId(SerializeStreamingSourceId(IdentityFrom<StreamingSourceId>(31))).Value() ==
                    IdentityFrom<StreamingSourceId>(31));
            RequireError(DeserializeStreamingCellId({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff}),
                         WorldStreamingErrors::SerializedIdentityInvalid);

            const auto generation = IdentityFrom<StreamingGeneration>(3);
            const auto nextGeneration = NextStreamingGeneration(generation);
            REQUIRE(nextGeneration.HasValue());
            REQUIRE(nextGeneration.Value().Value() == 4);
            RequireError(NextStreamingGeneration(IdentityFrom<StreamingGeneration>(std::numeric_limits<std::uint64_t>::max())),
                         WorldStreamingErrors::GenerationExhausted);
        }

        TEST_CASE("World streaming foundation keeps registry snapshots stable across publication",
                  "[unit][world_streaming][qualification][headless][registry]") {
            const auto partition = World(42);
            const auto base = Layer(2);
            auto descriptor = QualificationDescriptor(partition).Value();
            const auto owner = RuntimeOwner(partition);
            auto registry = WorldPartitionRegistry::Create(IdentityFrom<WorldPartitionRegistryId>(51), owner, {4, 4}).Value();
            REQUIRE(registry->Publish(std::move(descriptor), IdentityFrom<WorldPartitionRegistryRevision>(1)).HasValue());

            const auto first = registry->Snapshot().Value();
            const auto handle = first.Find({-1, 0, 0, 0, base}).Value();
            REQUIRE(first.Resolve(handle).Value()->package.chunkAsset == Asset(1));

            std::array<WorldPartitionCellHandle, 4> queryOutput{};
            const auto query = first.Query({{Math::WorldCoordinate64::FromMillimeters(-1'000, 0, 0),
                                             Math::WorldCoordinate64::FromMillimeters(999, 999, 999)},
                                            base,
                                            0},
                                           queryOutput);
            REQUIRE(query.HasValue());
            REQUIRE(query.Value().matches == 2);
            REQUIRE(queryOutput[0].cell == StreamingCellId{-1, 0, 0, 0, base});
            REQUIRE(queryOutput[1].cell == StreamingCellId{0, 0, 0, 0, base});

            REQUIRE(registry->Publish(QualificationDescriptor(partition, 20).Value(), IdentityFrom<WorldPartitionRegistryRevision>(2))
                        .HasValue());
            const auto second = registry->Snapshot().Value();
            REQUIRE(first.Resolve(handle).Value()->package.chunkAsset == Asset(1));
            RequireError(second.Resolve(handle), WorldStreamingErrors::PartitionRegistryStale);
            REQUIRE(second.Resolve(second.Find(handle.cell).Value()).Value()->package.chunkAsset == Asset(21));

            const auto sentinel = WorldPartitionCellHandle{first.Binding(), 99, handle.cell};
            std::array outputWithNoCapacity{sentinel};
            RequireError(second.Query({{Math::WorldCoordinate64::FromMillimeters(-2'000, -1'000, -1'000),
                                        Math::WorldCoordinate64::FromMillimeters(1'999, 1'999, 999)},
                                       base,
                                       0},
                                      outputWithNoCapacity),
                         WorldStreamingErrors::PartitionRegistryCapacityExceeded);
            REQUIRE(outputWithNoCapacity.front() == sentinel);
        }

        TEST_CASE("World streaming foundation preserves authored spatial references",
                  "[unit][world_streaming][qualification][headless][references]") {
            const auto partition = World(43);
            const auto layer = Layer(2);
            const auto descriptor = QualificationDescriptor(partition).Value();
            const auto page = Asset(40);
            const auto firstRevision = IdentityFrom<WorldAuthoringRevision>(1);
            const auto secondRevision = IdentityFrom<WorldAuthoringRevision>(2);
            const std::array candidates{
                WorldSpatialAssignmentCandidate{{page, 1},
                                                firstRevision,
                                                {Math::WorldCoordinate64::FromMillimeters(0, 0, 0),
                                                 Math::WorldCoordinate64::FromMillimeters(999, 999, 999)},
                                                layer,
                                                0},
                WorldSpatialAssignmentCandidate{{page, 2},
                                                secondRevision,
                                                {Math::WorldCoordinate64::FromMillimeters(1'000, 0, 0),
                                                 Math::WorldCoordinate64::FromMillimeters(1'999, 999, 999)},
                                                layer,
                                                0},
            };
            const auto assignments = WorldSpatialAssignment::Create(descriptor, candidates, {2, 2, 4});
            REQUIRE(assignments.HasValue());
            REQUIRE(assignments.Value().Objects().size() == 2);
            REQUIRE(assignments.Value().CellsForObject(0).size() == 1);
            REQUIRE(assignments.Value().CellsForObject(1).size() == 1);

            const WorldSpatialObjectDescriptor authored{
                .version = {},
                .address = {page, 1},
                .revision = firstRevision,
                .sourceAsset = Asset(41),
                .bounds = candidates[0].bounds,
                .placement = WorldSpatialObjectPlacementClass::Spatial,
            };
            REQUIRE(ValidateWorldSpatialObjectDescriptor(authored).HasValue());
            REQUIRE(ValidateWorldSpatialObjectAdmission({authored, std::nullopt}, {.currentDescriptor = std::nullopt,
                                                                                   .objectCount = 0,
                                                                                   .objectCapacity = 2,
                                                                                   .ownerState = WorldSpatialObjectOwnerState::Active})
                        .Value() == WorldSpatialObjectAdmissionKind::Insert);
        }

        TEST_CASE("World streaming foundation enforces dependency revision policy",
                  "[unit][world_streaming][qualification][headless][dependencies]") {
            const auto partition = World(43);
            const auto descriptor = QualificationDescriptor(partition).Value();
            const auto page = Asset(40);
            const auto firstRevision = IdentityFrom<WorldAuthoringRevision>(1);
            const auto secondRevision = IdentityFrom<WorldAuthoringRevision>(2);
            const std::array candidates{
                WorldSpatialAssignmentCandidate{{page, 1},
                                                firstRevision,
                                                {Math::WorldCoordinate64::FromMillimeters(0, 0, 0),
                                                 Math::WorldCoordinate64::FromMillimeters(999, 999, 999)},
                                                Layer(2),
                                                0},
                WorldSpatialAssignmentCandidate{{page, 2},
                                                secondRevision,
                                                {Math::WorldCoordinate64::FromMillimeters(1'000, 0, 0),
                                                 Math::WorldCoordinate64::FromMillimeters(1'999, 999, 999)},
                                                Layer(2),
                                                0},
            };
            const auto assignments = WorldSpatialAssignment::Create(descriptor, candidates, {2, 2, 4}).Value();
            const std::array dependencies{
                WorldDependencyCandidate{{{page, 1}, firstRevision}, {{page, 2}, secondRevision}, WorldDependencyKind::Hard},
                WorldDependencyCandidate{{{page, 2}, secondRevision},
                                         {{Asset(42), 3}, IdentityFrom<WorldAuthoringRevision>(9)},
                                         WorldDependencyKind::Soft},
            };
            const auto plan = WorldDependencyPlan::Create(assignments, dependencies, {4, 2, 2, 2});
            REQUIRE(plan.HasValue());
            REQUIRE(plan.Value().Bundles().size() == 1);
            REQUIRE(plan.Value().MembersForBundle(0).size() == 2);
            REQUIRE(plan.Value().SoftReferences().size() == 1);
            REQUIRE(plan.Value().SoftReferences().front().target.address.page == Asset(42));

            auto stale = dependencies[0];
            stale.source.revision = IdentityFrom<WorldAuthoringRevision>(99);
            RequireError(WorldDependencyPlan::Create(assignments, std::array{stale}, {4, 2, 2, 2}),
                         WorldStreamingErrors::DependencyPlanRevisionStale);
            REQUIRE(assignments.Objects().size() == 2);
        }

        TEST_CASE("World streaming foundation qualifies exact capability profiles",
                  "[unit][world_streaming][qualification][headless][fallback][profiles]") {
            const auto capabilities = Capabilities();
            auto request = SettingsRequest();
            const auto settings = WorldPartitionProjectSettings::Create(request, capabilities);
            REQUIRE(settings.HasValue());
            REQUIRE(settings.Value().PackageMode() == WorldPartitionPackageMode::ArchiveChunk);
            REQUIRE(ValidateWorldPartitionSettingsAdmission(settings.Value(), request.settings, request.revision, capabilities.capability,
                                                            capabilities.revision, WorldPartitionSettingsLifecycle::Active)
                        .HasValue());

            request.contractVersion = WorldPartitionProjectSettingsRequest::CurrentContractVersion + 1;
            RequireError(WorldPartitionProjectSettings::Create(request, capabilities), WorldStreamingErrors::PartitionSettingsInvalid);
            request = SettingsRequest();
            request.packageMode = WorldPartitionPackageMode::StandaloneCellFile;
            RequireError(WorldPartitionProjectSettings::Create(request, capabilities), WorldStreamingErrors::PartitionSettingsUnsupported);
        }

        TEST_CASE("World streaming foundation provides the non-streamed fallback",
                  "[unit][world_streaming][qualification][headless][fallback][provider]") {
            const auto partition = World(44);
            const auto owner = SourceOwner(partition);
            auto nullResult = FallbackStreamingProvider::Create({.owner = owner,
                                                                 .revision = IdentityFrom<StreamingSourceRevision>(1),
                                                                 .mode = FallbackStreamingProviderMode::Null,
                                                                 .singleCell = std::nullopt,
                                                                 .maximumPublishedCells = 0});
            REQUIRE(nullResult.HasValue());
            auto provider = std::move(nullResult).Value();
            REQUIRE(provider.DesiredCells().empty());

            const auto singleCell = StreamingCellId{0, 0, 0, 0, Layer(2)};
            REQUIRE(provider
                        .Replace({.owner = owner,
                                  .revision = IdentityFrom<StreamingSourceRevision>(2),
                                  .mode = FallbackStreamingProviderMode::SingleCell,
                                  .singleCell = singleCell,
                                  .maximumPublishedCells = 1})
                        .HasValue());
            REQUIRE(provider.DesiredCells().size() == 1);
            REQUIRE(provider.DesiredCells().front() == singleCell);

            RequireError(provider.Replace({.owner = owner,
                                           .revision = IdentityFrom<StreamingSourceRevision>(3),
                                           .mode = FallbackStreamingProviderMode::SingleCell,
                                           .singleCell = singleCell,
                                           .maximumPublishedCells = 0}),
                         WorldStreamingErrors::FallbackProviderCapacityExceeded);
            REQUIRE(provider.Revision() == IdentityFrom<StreamingSourceRevision>(2));
            REQUIRE(provider.DesiredCells().front() == singleCell);

            REQUIRE(provider.RequestCancellation(owner, IdentityFrom<StreamingSourceRevision>(2)).HasValue());
            REQUIRE(provider.DesiredCells().empty());
            REQUIRE(provider.Shutdown(owner).HasValue());
            REQUIRE(provider.Shutdown(owner).HasValue());
            REQUIRE(provider.State() == FallbackStreamingProviderState::Closed);
        }

        TEST_CASE("Partition qualification rejects malformed identity and descriptor inputs without borrowing caller state",
                  "[unit][world_streaming][qualification][headless][identity][descriptor][failure]") {
            const auto partition = World(42);
            const auto layer = Layer(2);
            RequireError(DeserializeWorldPartitionId({}), WorldStreamingErrors::SerializedIdentityInvalid);
            RequireError(DeserializeStreamingSourceId({}), WorldStreamingErrors::SerializedIdentityInvalid);
            RequireError(DeserializeStreamingLayerId({0xff, 0xff}), WorldStreamingErrors::SerializedIdentityInvalid);
            RequireError(NextPartitionEpoch({}), WorldStreamingErrors::IdentityInvalid);
            RequireError(NextPartitionEpoch(IdentityFrom<PartitionEpoch>(std::numeric_limits<std::uint64_t>::max())),
                         WorldStreamingErrors::GenerationExhausted);

            const std::array layers{
                WorldLayerDescriptor{layer, "base", WorldLayerOwnership::WorldStreaming, WorldLayerFlags::Persistent, 1.0F}};
            std::array cells{WorldPartitionCellDescriptor{{0, 0, 0, 0, layer}, {Asset(1)}}};
            const WorldPartitionBounds bounds{Math::WorldCoordinate64::FromMillimeters(0, 0, 0),
                                              Math::WorldCoordinate64::FromMillimeters(999, 999, 999)};
            const auto grid = WorldCellQuantizationPolicy::Create({}, 1'000, {0, 0, 0, 0, 0, 0}, 1).Value();
            auto owned = WorldPartitionDescriptor::Create({}, partition, bounds, grid, layers, cells, {1, 1, 32});
            REQUIRE(owned.HasValue());
            cells[0].package.chunkAsset = Asset(2);
            REQUIRE(owned.Value().Cells()[0].package.chunkAsset == Asset(1));

            RequireError(WorldPartitionDescriptor::Create({2, 0}, partition, bounds, grid, layers, cells, {1, 1, 32}),
                         WorldStreamingErrors::PartitionVersionUnsupported);
            RequireError(WorldPartitionDescriptor::Create({}, partition, bounds, grid, layers, cells, {1, 0, 32}),
                         WorldStreamingErrors::PartitionDescriptorInvalid);
            RequireError(WorldPartitionDescriptor::Create({}, partition, bounds, grid, layers, cells, {1, 1, 3}),
                         WorldStreamingErrors::PartitionCapacityExceeded);
            const std::array duplicateCells{cells[0], cells[0]};
            RequireError(WorldPartitionDescriptor::Create({}, partition, bounds, grid, layers, duplicateCells, {1, 2, 32}),
                         WorldStreamingErrors::PartitionIdentityConflict);
            REQUIRE(cells[0].package.chunkAsset == Asset(2));
            REQUIRE(owned.Value().Cells()[0].package.chunkAsset == Asset(1));
        }

        TEST_CASE("Partition qualification fences queries and preserves every output slot on failure",
                  "[unit][world_streaming][qualification][headless][registry][query][failure]") {
            const auto partition = World(42);
            const auto owner = RuntimeOwner(partition);
            auto registry = WorldPartitionRegistry::Create(IdentityFrom<WorldPartitionRegistryId>(51), owner, {4, 4}).Value();
            auto descriptor = QualificationDescriptor(partition).Value();
            REQUIRE(registry->Publish(std::move(descriptor), IdentityFrom<WorldPartitionRegistryRevision>(1)).HasValue());
            const auto first = registry->Snapshot().Value();
            const auto handle = first.Find({0, 0, 0, 0, Layer(2)}).Value();
            const auto sentinel = WorldPartitionCellHandle{first.Binding(), 99, handle.cell};
            std::array output{sentinel, sentinel};
            const WorldPartitionBounds whole{Math::WorldCoordinate64::FromMillimeters(-2'000, -1'000, -1'000),
                                             Math::WorldCoordinate64::FromMillimeters(1'999, 1'999, 999)};
            RequireError(first.Query({whole, std::nullopt, std::nullopt}, output), WorldStreamingErrors::PartitionRegistryCapacityExceeded);
            REQUIRE(output[0] == sentinel);
            REQUIRE(output[1] == sentinel);
            RequireError(first.Query({whole, Layer(99), 0}, output), WorldStreamingErrors::PartitionRegistryUnsupported);
            RequireError(first.Query({whole, Layer(2), 2}, output), WorldStreamingErrors::PartitionRegistryUnsupported);
            RequireError(first.Query({{whole.maximum, whole.minimum}, Layer(2), 0}, output),
                         WorldStreamingErrors::PartitionRegistryInvalid);
            REQUIRE(output[0] == sentinel);
            REQUIRE(output[1] == sentinel);

            const auto exactEdge =
                first.Query({{Math::WorldCoordinate64::FromMillimeters(999, 0, 0), Math::WorldCoordinate64::FromMillimeters(999, 0, 0)},
                             Layer(2),
                             0},
                            output);
            REQUIRE(exactEdge.HasValue());
            REQUIRE(exactEdge.Value().matches == 1);
            REQUIRE(output[0].cell == handle.cell);
            REQUIRE(output[1] == sentinel);
            REQUIRE(exactEdge.Value().binding == first.Binding());

            auto candidate = QualificationDescriptor(partition, 20).Value();
            RequireError(registry->Publish(std::move(candidate), IdentityFrom<WorldPartitionRegistryRevision>(3)),
                         WorldStreamingErrors::PartitionRegistryStale);
            REQUIRE(candidate.Cells().size() == 4);
            REQUIRE(registry->Snapshot().Value().Binding() == first.Binding());
            REQUIRE(registry->Publish(std::move(candidate), IdentityFrom<WorldPartitionRegistryRevision>(2)).HasValue());
            const auto second = registry->Snapshot().Value();
            RequireError(second.Resolve(handle), WorldStreamingErrors::PartitionRegistryStale);
            REQUIRE(first.Resolve(handle).Value()->package.chunkAsset == Asset(2));
            registry->BeginCancellation();
            RequireError(registry->Snapshot(), WorldStreamingErrors::PartitionRegistryLifecycleUnavailable);
            registry->Shutdown();
            REQUIRE(first.Resolve(handle).Value()->package.chunkAsset == Asset(2));
            REQUIRE(second.Find(handle.cell).HasValue());
        }

        TEST_CASE("Partition qualification keeps fallback demand transactional through replacement and teardown",
                  "[unit][world_streaming][qualification][headless][fallback][provider][lifecycle]") {
            const auto owner = SourceOwner(World(44));
            const auto firstCell = StreamingCellId{0, 0, 0, 0, Layer(2)};
            const auto secondCell = StreamingCellId{1, 0, 0, 0, Layer(2)};
            RequireError(FallbackStreamingProvider::Create({owner, IdentityFrom<StreamingSourceRevision>(1),
                                                            static_cast<FallbackStreamingProviderMode>(255), firstCell, 1}),
                         WorldStreamingErrors::FallbackProviderUnsupported);
            RequireError(FallbackStreamingProvider::Create(
                             {owner, IdentityFrom<StreamingSourceRevision>(1), FallbackStreamingProviderMode::SingleCell, std::nullopt, 1}),
                         WorldStreamingErrors::FallbackProviderInvalid);
            auto provider = FallbackStreamingProvider::Create(
                                {owner, IdentityFrom<StreamingSourceRevision>(1), FallbackStreamingProviderMode::SingleCell, firstCell, 1})
                                .Value();
            const auto originalDemand = provider.DesiredCells();
            REQUIRE(originalDemand.size() == 1);
            RequireError(provider.Replace(
                             {owner, IdentityFrom<StreamingSourceRevision>(2), FallbackStreamingProviderMode::SingleCell, secondCell, 0}),
                         WorldStreamingErrors::FallbackProviderCapacityExceeded);
            REQUIRE(provider.Revision().Value() == 1);
            REQUIRE(provider.DesiredCells().front() == firstCell);
            REQUIRE(originalDemand.front() == firstCell);
            RequireError(provider.Replace(
                             {owner, IdentityFrom<StreamingSourceRevision>(1), FallbackStreamingProviderMode::SingleCell, secondCell, 1}),
                         WorldStreamingErrors::FallbackProviderStale);
            RequireError(provider.RequestCancellation(owner, IdentityFrom<StreamingSourceRevision>(2)),
                         WorldStreamingErrors::FallbackProviderStale);
            REQUIRE(provider.DesiredCells().front() == firstCell);
            REQUIRE(
                provider
                    .Replace({owner, IdentityFrom<StreamingSourceRevision>(2), FallbackStreamingProviderMode::SingleCell, secondCell, 1})
                    .HasValue());
            REQUIRE(provider.DesiredCells().front() == secondCell);
            REQUIRE(provider.RequestCancellation(owner, IdentityFrom<StreamingSourceRevision>(2)).HasValue());
            REQUIRE(provider.DesiredCells().empty());
            RequireError(provider.Replace(
                             {owner, IdentityFrom<StreamingSourceRevision>(3), FallbackStreamingProviderMode::Null, std::nullopt, 0}),
                         WorldStreamingErrors::FallbackProviderLifecycleUnavailable);
            REQUIRE(provider.Shutdown(owner).HasValue());
            REQUIRE(provider.Shutdown(owner).HasValue());
            REQUIRE(provider.DesiredCells().empty());
        }
    }  // namespace
}  // namespace Horo::WorldStreaming
