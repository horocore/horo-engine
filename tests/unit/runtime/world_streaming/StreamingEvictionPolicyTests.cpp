#include "Horo/WorldStreaming/StreamingEvictionPolicy.h"
#include "WorldStreamingTestUtils.h"

#include <algorithm>
#include <array>
#include <limits>

namespace Horo::WorldStreaming {
    namespace {
        using TestSupport::IdentityFrom;
        using TestSupport::RequireError;

        StreamingEvictionPolicy Policy() {
            return {.id = IdentityFrom<StreamingEvictionPolicyId>(1),
                    .revision = IdentityFrom<StreamingEvictionPolicyRevision>(2),
                    .maximumCandidates = 16};
        }

        StreamingEvictionContext Context() {
            return {.owner = TestSupport::WorldOwner(),
                    .policy = Policy().id,
                    .policyRevision = Policy().revision,
                    .snapshotRevision = IdentityFrom<StreamingEvictionSnapshotRevision>(3),
                    .serviceTimeMilliseconds = 100,
                    .lifecycle = StreamingEvictionLifecycle::Active};
        }

        StreamingEvictionCandidate Candidate(const int x) {
            return {.owner = Context().owner,
                    .snapshotRevision = Context().snapshotRevision,
                    .fence = {.partition = Context().owner.partition,
                              .epoch = Context().owner.epoch,
                              .cell = {.x = x, .layer = TestSupport::Layer()},
                              .generation = IdentityFrom<StreamingGeneration>(1)},
                    .state = StreamingCellState::Active,
                    .retentionPriority = 1,
                    .lastUsedServiceMilliseconds = 10};
        }
    }  // namespace

    TEST_CASE("Eviction ranking respects each pin and preserves charged lease evidence", "[unit][world_streaming][eviction]") {
        std::array candidates{Candidate(4), Candidate(3), Candidate(2), Candidate(1), Candidate(5), Candidate(6), Candidate(7)};
        candidates[0].sourcePins = 1;
        candidates[1].gameplayPins = 1;
        candidates[2].providerPins = 1;
        candidates[3].outstandingLeases = 9;
        candidates[4].retentionPriority = 0;
        candidates[5].lastUsedServiceMilliseconds = 5;
        candidates[6].state = StreamingCellState::Resident;
        std::array<StreamingEvictionVictim, 7> output{};
        const auto selected = SelectStreamingEvictionVictims(Policy(), Context(), candidates, output);
        REQUIRE(selected.HasValue());
        REQUIRE(selected.Value() == 4);
        CHECK(output[0].candidate.fence.cell.x == 5);
        CHECK(output[1].candidate.fence.cell.x == 6);
        CHECK(output[2].candidate == candidates[3]);
        CHECK(output[2].RequiresLeaseDrain());
        CHECK_FALSE(output[0].RequiresLeaseDrain());
        CHECK(output[3].candidate.fence.cell.x == 7);
        const auto expected = output;
        std::ranges::reverse(candidates);
        REQUIRE(SelectStreamingEvictionVictims(Policy(), Context(), candidates, output).HasValue());
        for (std::size_t index = 0; index < selected.Value(); ++index)
            CHECK(output[index].candidate == expected[index].candidate);
    }

    TEST_CASE("Eviction uses manifest tuple order and explicit pin release", "[unit][world_streaming][eviction]") {
        std::array candidates{Candidate(-50), Candidate(90), Candidate(10)};
        candidates[0].fence.cell.layer = TestSupport::Layer(3);
        candidates[1].fence.cell.lod = 1;
        candidates[2].sourcePins = 1;
        std::array<StreamingEvictionVictim, 3> output{};
        auto result = SelectStreamingEvictionVictims(Policy(), Context(), candidates, output);
        REQUIRE(result.HasValue());
        REQUIRE(result.Value() == 2);
        CHECK(output[0].candidate == candidates[1]);
        CHECK(output[1].candidate == candidates[0]);
        auto context = Context();
        context.snapshotRevision = IdentityFrom<StreamingEvictionSnapshotRevision>(4);
        for (auto &candidate : candidates)
            candidate.snapshotRevision = context.snapshotRevision;
        candidates[2].sourcePins = 0;
        result = SelectStreamingEvictionVictims(Policy(), context, candidates, output);
        REQUIRE(result.HasValue());
        REQUIRE(result.Value() == 3);
        CHECK(output[0].candidate == candidates[2]);
    }

    TEST_CASE("Eviction excludes nonresident and already retiring attempts", "[unit][world_streaming][eviction]") {
        std::array candidates{Candidate(1), Candidate(2), Candidate(3), Candidate(4)};
        candidates[0].state = StreamingCellState::Unloaded;
        candidates[1].state = StreamingCellState::Loading;
        candidates[2].state = StreamingCellState::Evicting;
        candidates[3].state = StreamingCellState::Failed;
        std::array<StreamingEvictionVictim, 4> output{};
        output[0].candidate = Candidate(99);
        auto result = SelectStreamingEvictionVictims(Policy(), Context(), candidates, output);
        REQUIRE(result.HasValue());
        CHECK(result.Value() == 0);
        CHECK(output[0].candidate == Candidate(99));
        result = SelectStreamingEvictionVictims(Policy(), Context(), {}, output);
        REQUIRE(result.HasValue());
        CHECK(result.Value() == 0);
    }

    TEST_CASE("Eviction failures preserve the whole caller output", "[unit][world_streaming][eviction]") {
        std::array candidates{Candidate(1), Candidate(2)};
        std::array<StreamingEvictionVictim, 2> output{{{Candidate(90)}, {Candidate(91)}}};
        const auto preserved = output;
        auto policy = Policy();
        auto context = Context();
        SECTION("unsupported version") {
            policy.contractVersion = 2;
            RequireError(SelectStreamingEvictionVictims(policy, context, candidates, output),
                         WorldStreamingErrors::EvictionPolicyUnsupported);
        }
        SECTION("invalid policy ceiling") {
            policy.maximumCandidates = 0;
            RequireError(SelectStreamingEvictionVictims(policy, context, candidates, output), WorldStreamingErrors::EvictionPolicyInvalid);
        }
        SECTION("implementation ceiling") {
            policy.maximumCandidates = StreamingEvictionPolicy::MaximumCandidateCount + 1;
            RequireError(SelectStreamingEvictionVictims(policy, context, candidates, output), WorldStreamingErrors::EvictionPolicyInvalid);
        }
        SECTION("invalid authority") {
            context.owner.owner = {};
            RequireError(SelectStreamingEvictionVictims(policy, context, candidates, output), WorldStreamingErrors::EvictionPolicyInvalid);
        }
        SECTION("invalid identity") {
            candidates[1].fence.generation = {};
            RequireError(SelectStreamingEvictionVictims(policy, context, candidates, output), WorldStreamingErrors::EvictionPolicyInvalid);
        }
        SECTION("invalid numerical priority") {
            candidates[1].retentionPriority = std::numeric_limits<double>::infinity();
            RequireError(SelectStreamingEvictionVictims(policy, context, candidates, output), WorldStreamingErrors::EvictionPolicyInvalid);
        }
        SECTION("unknown residency") {
            candidates[1].state = static_cast<StreamingCellState>(255);
            RequireError(SelectStreamingEvictionVictims(policy, context, candidates, output),
                         WorldStreamingErrors::EvictionPolicyUnsupported);
        }
        SECTION("candidate ceiling") {
            policy.maximumCandidates = 1;
            RequireError(SelectStreamingEvictionVictims(policy, context, candidates, output),
                         WorldStreamingErrors::EvictionPolicyCapacityExceeded);
        }
        SECTION("output ceiling") {
            RequireError(SelectStreamingEvictionVictims(policy, context, candidates, std::span{output}.first(1)),
                         WorldStreamingErrors::EvictionPolicyCapacityExceeded);
        }
        SECTION("duplicate cell successor") {
            candidates[1].fence.cell = candidates[0].fence.cell;
            candidates[1].fence.generation = IdentityFrom<StreamingGeneration>(2);
            RequireError(SelectStreamingEvictionVictims(policy, context, candidates, output),
                         WorldStreamingErrors::EvictionPolicyIdentityConflict);
        }
        for (std::size_t index = 0; index < output.size(); ++index)
            CHECK(output[index].candidate == preserved[index].candidate);
    }

    TEST_CASE("Eviction rejects replacements, stale pin facts and lifecycle closure", "[unit][world_streaming][eviction][lifecycle]") {
        std::array candidates{Candidate(1)};
        std::array<StreamingEvictionVictim, 1> output{{{Candidate(99)}}};
        auto context = Context();
        SECTION("policy replacement") {
            context.policyRevision = IdentityFrom<StreamingEvictionPolicyRevision>(3);
        }
        SECTION("authority replacement") {
            context.owner = TestSupport::WorldOwner(6);
        }
        SECTION("partition replacement") {
            context.owner = TestSupport::WorldOwner(5, 2);
        }
        SECTION("changed pin or lease snapshot") {
            context.snapshotRevision = IdentityFrom<StreamingEvictionSnapshotRevision>(4);
        }
        SECTION("backward service clock") {
            context.serviceTimeMilliseconds = 9;
        }
        SECTION("cancellation") {
            context.lifecycle = StreamingEvictionLifecycle::Cancelling;
        }
        SECTION("shutdown") {
            context.lifecycle = StreamingEvictionLifecycle::Closed;
        }
        const auto expected = context.lifecycle == StreamingEvictionLifecycle::Active
                                  ? &WorldStreamingErrors::EvictionPolicyStale
                                  : &WorldStreamingErrors::EvictionPolicyLifecycleUnavailable;
        RequireError(SelectStreamingEvictionVictims(Policy(), context, candidates, output), *expected);
        CHECK(output[0].candidate == Candidate(99));
    }
}  // namespace Horo::WorldStreaming
