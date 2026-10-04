#include "Horo/WorldStreaming/WorldPackageChunkAssignment.h"
#include "StreamingCellCandidateTestSupport.h"
#include "WorldStreamingTestUtils.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::WorldStreaming {
    namespace {
        using namespace TestSupport;
        using namespace CandidateTestSupport;

        Assets::AssetChunkId Chunk(const char *name) {
            return Assets::AssetChunkId::Parse(name).Value();
        }

        WorldPackageAssignmentBinding Binding(const std::uint64_t revision = 1) {
            return {IdentityFrom<WorldPackageAssignmentId>(10), IdentityFrom<WorldPackageAssignmentRevision>(revision),
                    IdentityFrom<PartitionEpoch>(1)};
        }

        std::vector<Assets::AssetChunkDefinition> Definitions() {
            return {{Chunk("base"), Assets::AssetChunkKind::Base, {Asset(4)}, {}, 0, {}},
                    {Chunk("one"), Assets::AssetChunkKind::Optional, {Asset(5)}, {Chunk("base")}, 1, {}},
                    {Chunk("two"), Assets::AssetChunkKind::Optional, {Asset(6)}, {Chunk("one")}, 2, {}},
                    {Chunk("unused"), Assets::AssetChunkKind::Optional, {Asset(7)}, {Chunk("base")}, 1, {}}};
        }

        constexpr WorldPackageChunkLimits Limits{4, 4, 8, 16};

        WorldPackageChunkAssignment Assignment(const WorldPackageAssignmentBinding binding = Binding()) {
            const auto manifest = Manifest();
            const auto definitions = Definitions();
            const auto plan = Assets::AssetChunkPlan::Create(definitions).Value();
            auto result = WorldPackageChunkAssignment::Create(manifest, plan, binding, Hash(20), Limits);
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        std::vector<WorldPackageChunkAvailability> States() {
            return {{Chunk("unused"), WorldPackageChunkState::Unavailable},
                    {Chunk("two"), WorldPackageChunkState::Downloadable},
                    {Chunk("base"), WorldPackageChunkState::Installed},
                    {Chunk("one"), WorldPackageChunkState::Installed}};
        }

        WorldPackageContentContext ContentContext(const std::uint64_t revision = 1) {
            return {Binding(), IdentityFrom<WorldPackageAvailabilityRevision>(revision), WorldPackageContentLifecycle::Active};
        }
    }  // namespace

    TEST_CASE("World package assignment owns canonical artifact membership and dependency requirements",
              "[unit][world_streaming][package_chunk]") {
        auto assignment = Assignment();
        REQUIRE(assignment.Partition() == World());
        REQUIRE(assignment.Binding() == Binding());
        REQUIRE(assignment.Cells().size() == 3);
        REQUIRE(assignment.Cells()[0].artifact == Asset(4));
        REQUIRE(assignment.Cells()[0].artifactHash == Hash());
        REQUIRE(std::ranges::equal(assignment.Requirements(0), std::array{Chunk("base"), Chunk("one"), Chunk("two")}));
        REQUIRE(std::ranges::equal(assignment.Requirements(1), std::array{Chunk("base"), Chunk("one")}));
        REQUIRE(std::ranges::equal(assignment.Requirements(2), std::array{Chunk("base"), Chunk("one"), Chunk("two")}));
        REQUIRE(assignment.Requirements(3).empty());
        const auto unchanged = Manifest();
        REQUIRE(assignment.ValidateCell(unchanged, Cell()).HasValue());
        const std::array changedDependencies{Cell(1)};
        const auto changed = Manifest(changedDependencies);
        RequireError(assignment.ValidateCell(changed, Cell()), WorldStreamingErrors::PackageChunkStale);

        auto retained = std::move(assignment);
        REQUIRE(retained.Requirements(0).size() == 3);
    }

    TEST_CASE("World package assignment closes transitive cyclic hard-cell components and deduplicates shared chunks",
              "[unit][world_streaming][package_chunk][dependencies]") {
        auto source = Manifest();
        const auto &original = source.Descriptor();
        auto descriptor = WorldPartitionDescriptor::Create(original.Version(), original.Partition(), original.Bounds(), original.Grid(),
                                                           original.Layers(), original.Cells(), {2, 4, 16})
                              .Value();
        const std::array first{Cell(1)};
        const std::array second{Cell(2)};
        const std::array third{Cell(1)};
        const std::array cooked{CookedWorldCellManifestCandidate{Cell(), 48, 128, 99, Hash(), first},
                                CookedWorldCellManifestCandidate{Cell(1), 1, 1, 1, Hash(8), second},
                                CookedWorldCellManifestCandidate{Cell(2), 1, 1, 2, Hash(9), third}};
        const auto manifest = CookedWorldIndexManifest::Create(std::move(descriptor), cooked, {4, 4, 4, 256, 256}).Value();
        auto definitions = Definitions();
        definitions[1].assets.push_back(Asset(6));
        definitions.erase(definitions.begin() + 2);
        const auto plan = Assets::AssetChunkPlan::Create(definitions).Value();
        const auto assignment = WorldPackageChunkAssignment::Create(manifest, plan, Binding(), Hash(20), Limits).Value();
        for (std::size_t index{}; index < 3; ++index)
            REQUIRE(std::ranges::equal(assignment.Requirements(index), std::array{Chunk("base"), Chunk("one")}));
    }

    TEST_CASE("World package assignment rejects missing membership invalid bounds and incompatible releases atomically",
              "[unit][world_streaming][package_chunk][failure]") {
        const auto manifest = Manifest();
        auto definitions = Definitions();
        SECTION("missing cell membership") {
            definitions[2].assets = {Asset(8)};
        }
        SECTION("DLC belongs to another base release") {
            definitions[2].kind = Assets::AssetChunkKind::Dlc;
            definitions[2].requiredBaseManifest = Hash(21);
        }
        const auto plan = Assets::AssetChunkPlan::Create(definitions).Value();
        const auto result = WorldPackageChunkAssignment::Create(manifest, plan, Binding(), Hash(20), Limits);
        REQUIRE(result.HasError());
        REQUIRE(manifest.Cells().size() == 3);
        REQUIRE(plan.Chunks().size() == 4);
    }

    TEST_CASE("World package assignment validates every mandatory capacity before publishing",
              "[unit][world_streaming][package_chunk][capacity]") {
        const auto manifest = Manifest();
        const auto definitions = Definitions();
        const auto plan = Assets::AssetChunkPlan::Create(definitions).Value();
        RequireError(WorldPackageChunkAssignment::Create(manifest, plan, {}, Hash(20), Limits), WorldStreamingErrors::PackageChunkInvalid);
        RequireError(WorldPackageChunkAssignment::Create(manifest, plan, Binding(), Hash(20), {}),
                     WorldStreamingErrors::PackageChunkInvalid);
        for (const auto limits : std::array{WorldPackageChunkLimits{2, 4, 8, 16}, WorldPackageChunkLimits{4, 3, 8, 16},
                                            WorldPackageChunkLimits{4, 4, 3, 16}, WorldPackageChunkLimits{4, 4, 8, 7}})
            RequireError(WorldPackageChunkAssignment::Create(manifest, plan, Binding(), Hash(20), limits),
                         WorldStreamingErrors::PackageChunkCapacityExceeded);
    }

    TEST_CASE("Partial package availability reports only missing required content with stable typed causes",
              "[unit][world_streaming][package_chunk][availability]") {
        const auto assignment = Assignment();
        auto states = States();
        const auto snapshot = WorldPackageAvailabilitySnapshot::Create(assignment, ContentContext().availability, states).Value();
        states[1].state = WorldPackageChunkState::Installed;
        const auto partial = EvaluateWorldCellContent(assignment, snapshot, Cell(), ContentContext());
        REQUIRE(partial.HasValue());
        REQUIRE_FALSE(partial.Value().IsAvailable());
        REQUIRE(partial.Value().missing.size() == 1);
        REQUIRE(partial.Value().missing[0].chunk == Chunk("two"));
        REQUIRE(partial.Value().missing[0].state == WorldPackageChunkState::Downloadable);
        REQUIRE(EvaluateWorldCellContent(assignment, snapshot, Cell(1), ContentContext()).Value().IsAvailable());
        for (const auto state : std::array{WorldPackageChunkState::Installable, WorldPackageChunkState::Installing,
                                           WorldPackageChunkState::Unavailable, WorldPackageChunkState::Failed}) {
            states[1].state = state;
            const auto replacement = WorldPackageAvailabilitySnapshot::Create(assignment, ContentContext(2).availability, states).Value();
            const auto result = EvaluateWorldCellContent(assignment, replacement, Cell(), ContentContext(2));
            REQUIRE(result.Value().missing[0].state == state);
        }
        states[1].state = WorldPackageChunkState::Installed;
        const auto installed = WorldPackageAvailabilitySnapshot::Create(assignment, ContentContext(2).availability, states).Value();
        REQUIRE(EvaluateWorldCellContent(assignment, installed, Cell(), ContentContext(2)).Value().IsAvailable());
        REQUIRE_FALSE(EvaluateWorldCellContent(assignment, snapshot, Cell(), ContentContext()).Value().IsAvailable());
    }

    TEST_CASE("Package availability publication rejects incomplete duplicate foreign and unsupported input",
              "[unit][world_streaming][package_chunk][failure]") {
        const auto assignment = Assignment();
        auto states = States();
        SECTION("missing state") {
            states.pop_back();
        }
        SECTION("extra state") {
            states.push_back({Chunk("extra"), WorldPackageChunkState::Installed});
        }
        SECTION("duplicate state") {
            states[1].chunk = states[0].chunk;
        }
        SECTION("foreign chunk") {
            states[1].chunk = Chunk("foreign");
        }
        SECTION("unsupported state") {
            states[1].state = WorldPackageChunkState::Count;
            RequireError(WorldPackageAvailabilitySnapshot::Create(assignment, ContentContext().availability, states),
                         WorldStreamingErrors::PackageChunkUnsupported);
            return;
        }
        RequireError(WorldPackageAvailabilitySnapshot::Create(assignment, ContentContext().availability, states),
                     WorldStreamingErrors::PackageChunkInvalid);
        RequireError(WorldPackageAvailabilitySnapshot::Create(assignment, {}, States()), WorldStreamingErrors::PackageChunkInvalid);
    }

    TEST_CASE("Package content admission fences replacement cancellation shutdown and undeclared cells",
              "[unit][world_streaming][package_chunk][lifecycle]") {
        const auto assignment = Assignment();
        const auto snapshot = WorldPackageAvailabilitySnapshot::Create(assignment, ContentContext().availability, States()).Value();
        auto context = ContentContext(2);
        RequireError(EvaluateWorldCellContent(assignment, snapshot, Cell(), context), WorldStreamingErrors::PackageChunkStale);
        context = ContentContext();
        context.assignment = Binding(2);
        RequireError(EvaluateWorldCellContent(assignment, snapshot, Cell(), context), WorldStreamingErrors::PackageChunkStale);
        const auto replacement = Assignment(Binding(2));
        RequireError(EvaluateWorldCellContent(replacement, snapshot, Cell(), context), WorldStreamingErrors::PackageChunkStale);
        context = ContentContext();
        context.assignment.owner = IdentityFrom<WorldPackageAssignmentId>(11);
        RequireError(EvaluateWorldCellContent(assignment, snapshot, Cell(), context), WorldStreamingErrors::PackageChunkStale);
        for (const auto lifecycle : std::array{WorldPackageContentLifecycle::Cancelling, WorldPackageContentLifecycle::Closed}) {
            context = ContentContext();
            context.lifecycle = lifecycle;
            RequireError(EvaluateWorldCellContent(assignment, snapshot, Cell(), context),
                         WorldStreamingErrors::PackageChunkLifecycleUnavailable);
        }
        context.lifecycle = WorldPackageContentLifecycle::Count;
        RequireError(EvaluateWorldCellContent(assignment, snapshot, Cell(), context), WorldStreamingErrors::PackageChunkUnsupported);
        RequireError(EvaluateWorldCellContent(assignment, snapshot, Cell(3), ContentContext()),
                     WorldStreamingErrors::PackageChunkUnassigned);
        RequireError(EvaluateWorldCellContent(assignment, snapshot, {}, ContentContext()), WorldStreamingErrors::PackageChunkInvalid);
        REQUIRE(EvaluateWorldCellContent(assignment, snapshot, Cell(1), ContentContext()).Value().IsAvailable());
    }
}  // namespace Horo::WorldStreaming
