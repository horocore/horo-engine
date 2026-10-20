#include "Horo/WorldStreaming/WorldSpatialAssignment.h"
#include "Horo/WorldStreaming/WorldStreamingErrors.h"
#include "WorldStreamingTestUtils.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <type_traits>
#include <vector>

namespace Horo::WorldStreaming {
    namespace {
        using TestSupport::Asset;
        using TestSupport::IdentityFrom;
        using TestSupport::Layer;
        using TestSupport::RequireError;

        [[nodiscard]] bool ShouldOmitCell(const bool omitOrigin, const std::int32_t x, const std::int32_t y, const std::int32_t z,
                                          const std::uint8_t lod, const StreamingLayerId layer) {
            return omitOrigin && x == 0 && y == 0 && z == 0 && lod == 0 && layer == Layer();
        }

        void AppendCells(std::vector<WorldPartitionCellDescriptor> &cells, const StreamingLayerId layer, const std::uint8_t lod,
                         const bool omitOrigin, std::uint8_t &asset) {
            for (std::int32_t z = -2; z <= 2; ++z) {
                for (std::int32_t y = -2; y <= 2; ++y) {
                    for (std::int32_t x = -2; x <= 2; ++x) {
                        if (ShouldOmitCell(omitOrigin, x, y, z, lod, layer))
                            continue;
                        cells.push_back({{x, y, z, lod, layer}, {Asset(asset++)}});
                        if (asset == 0)
                            asset = 1;
                    }
                }
            }
        }

        WorldPartitionDescriptor Descriptor(const bool omitOrigin = false) {
            const auto grid = WorldCellQuantizationPolicy::Create({}, 100, {-2, 2, -2, 2, -2, 2}, 2).Value();
            const std::vector<WorldLayerDescriptor> layers{
                {Layer(), "base", WorldLayerOwnership::WorldStreaming, WorldLayerFlags::Persistent, 1.0F},
                {Layer(7), "optional", WorldLayerOwnership::GameplayScript, WorldLayerFlags::Optional, 0.5F},
            };
            std::vector<WorldPartitionCellDescriptor> cells;
            std::uint8_t asset = 1;
            for (const auto layer : {Layer(), Layer(7)}) {
                for (std::uint8_t lod = 0; lod < 2; ++lod) {
                    AppendCells(cells, layer, lod, omitOrigin, asset);
                }
            }
            auto result = WorldPartitionDescriptor::Create({}, TestSupport::World(),
                                                           {Math::WorldCoordinate64::FromMillimeters(-200, -200, -200),
                                                            Math::WorldCoordinate64::FromMillimeters(299, 299, 299)},
                                                           grid, layers, cells, {2, 500, 32});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        WorldSpatialAssignmentCandidate Candidate(const std::uint8_t page, const std::uint64_t object,
                                                  const WorldPartitionBounds bounds = {Math::WorldCoordinate64::FromMillimeters(-1, 0, 0),
                                                                                       Math::WorldCoordinate64::FromMillimeters(99, 0, 0)},
                                                  const StreamingLayerId layer = Layer(), const std::uint8_t lod = 0,
                                                  const WorldAuthoringRevision revision = IdentityFrom<WorldAuthoringRevision>(1)) {
            return {{Asset(page), object}, revision, bounds, layer, lod};
        }

        constexpr WorldSpatialAssignmentLimits Limits{8, 16, 32};

        TEST_CASE("Authoring coverage shares cooked assignment semantics without manufacturing published cells",
                  "[unit][world_streaming][spatial_assignment]") {
            const auto descriptor = Descriptor();
            const WorldSpatialAssignmentAuthoringDomain source{descriptor.Partition(), descriptor.Bounds(), descriptor.Grid(),
                                                               descriptor.Layers()};
            const std::array candidates{Candidate(2, 9), Candidate(1, 4)};
            const auto authored = WorldSpatialAssignment::CreateForAuthoring(source, candidates, Limits);
            const auto cooked = WorldSpatialAssignment::Create(descriptor, candidates, Limits);
            REQUIRE(authored.HasValue());
            REQUIRE(cooked.HasValue());
            REQUIRE(authored.Value().Partition() == descriptor.Partition());
            REQUIRE(std::ranges::equal(authored.Value().Objects(), cooked.Value().Objects()));
            for (std::size_t index = 0; index < candidates.size(); ++index)
                REQUIRE(std::ranges::equal(authored.Value().CellsForObject(index), cooked.Value().CellsForObject(index)));

            const auto missing = Descriptor(true);
            const WorldSpatialAssignmentAuthoringDomain uncooked{missing.Partition(), missing.Bounds(), missing.Grid(), missing.Layers()};
            REQUIRE(WorldSpatialAssignment::CreateForAuthoring(uncooked, candidates, Limits).HasValue());
            RequireError(WorldSpatialAssignment::Create(missing, candidates, Limits),
                         WorldStreamingErrors::SpatialAssignmentCellUnavailable);
        }

        TEST_CASE("Authoring coverage rejects malformed source domains and budgets without touching source",
                  "[unit][world_streaming][spatial_assignment]") {
            const auto descriptor = Descriptor();
            auto layers = std::vector<WorldLayerDescriptor>{descriptor.Layers().begin(), descriptor.Layers().end()};
            WorldSpatialAssignmentAuthoringDomain source{descriptor.Partition(), descriptor.Bounds(), descriptor.Grid(), layers};
            const auto candidates = std::array{Candidate(1, 1)};
            const ErrorCodeDescriptor *expected = &WorldStreamingErrors::SpatialAssignmentInvalid;
            auto limits = Limits;
            SECTION("invalid partition") {
                source.partition = {};
            }
            SECTION("reversed world bounds") {
                std::swap(source.bounds.minimum, source.bounds.maximum);
            }
            SECTION("world outside grid") {
                source.bounds.maximum = Math::WorldCoordinate64::FromMillimeters(9000, 0, 0);
            }
            SECTION("no layers") {
                source.layers = {};
            }
            SECTION("invalid layer identity") {
                layers[0].id = {};
            }
            SECTION("duplicate layers") {
                layers[1].id = layers[0].id;
                expected = &WorldStreamingErrors::SpatialAssignmentIdentityConflict;
            }
            SECTION("coverage capacity") {
                limits.maximumCellsPerObject = 1;
                expected = &WorldStreamingErrors::SpatialAssignmentCapacityExceeded;
            }
            const auto result = WorldSpatialAssignment::CreateForAuthoring(source, candidates, limits);
            REQUIRE(result.HasError());
            // Invalid grid extents preserve the canonical quantization failure rather than translating it.
            if (source.bounds.maximum.Millimeters()[0] == 9000)
                REQUIRE(result.ErrorValue().code.Value() == WorldStreamingErrors::CellOutOfBounds.code.Value());
            else
                REQUIRE(result.ErrorValue().code.Value() == expected->code.Value());
            REQUIRE(candidates.front().address == Candidate(1, 1).address);
        }

        TEST_CASE("Spatial assignment is canonical and owns deterministic multi-cell results",
                  "[unit][world_streaming][spatial_assignment]") {
            const auto descriptor = Descriptor();
            std::vector candidates{Candidate(2, 9), Candidate(1, 4)};
            const auto original = candidates;

            auto result = WorldSpatialAssignment::Create(descriptor, candidates, Limits);
            REQUIRE(result.HasValue());
            auto assignment = std::move(result).Value();

            REQUIRE(assignment.Partition() == descriptor.Partition());
            REQUIRE(assignment.Objects().size() == 2);
            REQUIRE(assignment.Objects()[0].address == Candidate(1, 4).address);
            REQUIRE(assignment.Objects()[1].address == Candidate(2, 9).address);
            REQUIRE(assignment.CellsForObject(0).size() == 2);
            REQUIRE(assignment.CellsForObject(0)[0] == StreamingCellId{-1, 0, 0, 0, Layer()});
            REQUIRE(assignment.CellsForObject(0)[1] == StreamingCellId{0, 0, 0, 0, Layer()});
            REQUIRE(std::ranges::equal(assignment.CellsForObject(1), assignment.CellsForObject(0)));
            REQUIRE(assignment.CellsForObject(2).empty());
            REQUIRE(candidates == original);
            static_assert(!std::is_copy_constructible_v<WorldSpatialAssignment>);
            static_assert(std::is_move_constructible_v<WorldSpatialAssignment>);
        }

        TEST_CASE("Spatial assignment uses exact negative floor quantization and canonical three-axis ordering",
                  "[unit][world_streaming][spatial_assignment]") {
            const auto descriptor = Descriptor();
            const std::vector candidates{
                Candidate(1, 1,
                          {Math::WorldCoordinate64::FromMillimeters(-101, -1, -1), Math::WorldCoordinate64::FromMillimeters(-1, 99, 99)})};

            auto result = WorldSpatialAssignment::Create(descriptor, candidates, Limits);
            REQUIRE(result.HasValue());
            const auto cells = result.Value().CellsForObject(0);
            REQUIRE(cells.size() == 8);
            REQUIRE(cells.front() == StreamingCellId{-2, -1, -1, 0, Layer()});
            REQUIRE(cells[1] == StreamingCellId{-1, -1, -1, 0, Layer()});
            REQUIRE(cells.back() == StreamingCellId{-1, 0, 0, 0, Layer()});
        }

        TEST_CASE("Spatial assignment preserves layer LOD and exact authoring revision", "[unit][world_streaming][spatial_assignment]") {
            const auto descriptor = Descriptor();
            const auto revision = IdentityFrom<WorldAuthoringRevision>(7);
            const std::vector candidates{Candidate(1, 1,
                                                   {Math::WorldCoordinate64::FromMillimeters(100, 100, 100),
                                                    Math::WorldCoordinate64::FromMillimeters(199, 199, 199)},
                                                   Layer(7), 1, revision)};

            auto result = WorldSpatialAssignment::Create(descriptor, candidates, Limits);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().Objects()[0].revision == revision);
            REQUIRE(result.Value().CellsForObject(0).size() == 1);
            REQUIRE(result.Value().CellsForObject(0).front() == StreamingCellId{0, 0, 0, 1, Layer(7)});
        }

        TEST_CASE("Spatial assignment rejects malformed identities bounds and limits with typed errors",
                  "[unit][world_streaming][spatial_assignment]") {
            const auto descriptor = Descriptor();

            RequireError(WorldSpatialAssignment::Create(descriptor, {}, Limits), WorldStreamingErrors::SpatialAssignmentInvalid);
            RequireError(WorldSpatialAssignment::Create(descriptor, std::array{Candidate(1, 1)}, {0, 1, 1}),
                         WorldStreamingErrors::SpatialAssignmentInvalid);

            auto candidate = Candidate(1, 1);
            candidate.address = {};
            RequireError(WorldSpatialAssignment::Create(descriptor, std::array{candidate}, Limits),
                         WorldStreamingErrors::SpatialAssignmentInvalid);
            candidate = Candidate(1, 1);
            candidate.revision = {};
            RequireError(WorldSpatialAssignment::Create(descriptor, std::array{candidate}, Limits),
                         WorldStreamingErrors::SpatialAssignmentInvalid);
            candidate = Candidate(1, 1);
            candidate.bounds = {Math::WorldCoordinate64::FromMillimeters(1, 0, 0), Math::WorldCoordinate64::FromMillimeters(0, 0, 0)};
            RequireError(WorldSpatialAssignment::Create(descriptor, std::array{candidate}, Limits),
                         WorldStreamingErrors::SpatialAssignmentInvalid);
            candidate = Candidate(1, 1);
            candidate.bounds.maximum = Math::WorldCoordinate64::FromMillimeters(300, 0, 0);
            RequireError(WorldSpatialAssignment::Create(descriptor, std::array{candidate}, Limits),
                         WorldStreamingErrors::SpatialAssignmentInvalid);
        }

        TEST_CASE("Spatial assignment rejects duplicate object snapshots independent of revision",
                  "[unit][world_streaming][spatial_assignment]") {
            const auto descriptor = Descriptor();
            const std::array candidates{Candidate(1, 1), Candidate(1, 1, {}, Layer(), 0, IdentityFrom<WorldAuthoringRevision>(2))};

            RequireError(WorldSpatialAssignment::Create(descriptor, candidates, Limits),
                         WorldStreamingErrors::SpatialAssignmentIdentityConflict);
        }

        TEST_CASE("Spatial assignment distinguishes unsupported layer and LOD", "[unit][world_streaming][spatial_assignment]") {
            const auto descriptor = Descriptor();
            const std::array missingLayer{Candidate(1, 1, {}, Layer(9))};
            RequireError(WorldSpatialAssignment::Create(descriptor, missingLayer, Limits),
                         WorldStreamingErrors::SpatialAssignmentUnsupported);

            const std::array unsupportedLod{Candidate(1, 1, {}, Layer(), 2)};
            RequireError(WorldSpatialAssignment::Create(descriptor, unsupportedLod, Limits), WorldStreamingErrors::LodUnsupported);
        }

        TEST_CASE("Spatial assignment applies object and aggregate capacities before publication",
                  "[unit][world_streaming][spatial_assignment]") {
            const auto descriptor = Descriptor();
            const std::array objects{Candidate(1, 1), Candidate(1, 2)};
            RequireError(WorldSpatialAssignment::Create(descriptor, objects, {1, 16, 32}),
                         WorldStreamingErrors::SpatialAssignmentCapacityExceeded);
            RequireError(WorldSpatialAssignment::Create(descriptor, std::span{objects}.first(1), {8, 1, 32}),
                         WorldStreamingErrors::SpatialAssignmentCapacityExceeded);
            RequireError(WorldSpatialAssignment::Create(descriptor, objects, {8, 16, 3}),
                         WorldStreamingErrors::SpatialAssignmentCapacityExceeded);
        }

        TEST_CASE("Spatial assignment fails when deterministic coverage includes an undeclared descriptor cell",
                  "[unit][world_streaming][spatial_assignment]") {
            const auto descriptor = Descriptor(true);
            const std::array candidates{Candidate(1, 1)};

            RequireError(WorldSpatialAssignment::Create(descriptor, candidates, Limits),
                         WorldStreamingErrors::SpatialAssignmentCellUnavailable);
        }
    }  // namespace
}  // namespace Horo::WorldStreaming
