#include "Horo/WorldStreaming/StreamingCellStability.h"
#include "Horo/WorldStreaming/WorldStreamingErrors.h"
#include "WorldStreamingTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace Horo::WorldStreaming {
    namespace {
        using TestSupport::IdentityFrom;
        using TestSupport::Layer;
        using TestSupport::RequireError;
        using TestSupport::World;

        StreamingCellStabilityPolicy Policy(const std::uint64_t revision = 1, const std::uint32_t maximumTrackedCells = 8,
                                            const std::uint64_t lingerMilliseconds = 5'000, const std::uint64_t cooldownMilliseconds = 0,
                                            const std::uint32_t thrashThreshold = 3) {
            StreamingCellStabilityPolicyRequest request{};
            request.id = IdentityFrom<StreamingCellStabilityPolicyId>(10);
            request.revision = IdentityFrom<StreamingCellStabilityPolicyRevision>(revision);
            request.enterMarginMillimeters = 100;
            request.exitMarginMillimeters = 200;
            request.lingerMilliseconds = lingerMilliseconds;
            request.maximumTrackedCells = maximumTrackedCells;
            request.cooldownMilliseconds = cooldownMilliseconds;
            request.thrashExitThreshold = thrashThreshold;
            return StreamingCellStabilityPolicy::Create(request).Value();
        }

        StreamingCellStabilityContext Context(const std::uint64_t time = 1'000, const std::uint32_t tracked = 0) {
            return {.policy = IdentityFrom<StreamingCellStabilityPolicyId>(10),
                    .policyRevision = IdentityFrom<StreamingCellStabilityPolicyRevision>(1),
                    .partition = World(),
                    .epoch = IdentityFrom<PartitionEpoch>(4),
                    .serviceTimeMilliseconds = time,
                    .trackedCells = tracked,
                    .lifecycle = StreamingCellStabilityLifecycle::Active};
        }

        StreamingCellStabilityObservation Observation(const std::int64_t boundaryDistance,
                                                      const StreamingDesiredResidency residency = StreamingDesiredResidency::Activated) {
            return {.cell = {0, 0, 0, 0, Layer(2)},
                    .effectiveResidency = residency,
                    .pinnedResidencyFloor = std::nullopt,
                    .signedBoundaryDistanceMillimeters = boundaryDistance};
        }

        StreamingCellStabilityDecision Decision(const StreamingCellStabilityPolicy &policy, const StreamingCellStabilityContext &context,
                                                const StreamingCellStabilityObservation &observation,
                                                const std::optional<StreamingCellStabilitySnapshot> &previous = std::nullopt) {
            return EvaluateStreamingCellStability(policy, context, observation, previous).Value();
        }

        TEST_CASE("Cell stability applies inclusive enter and exit hysteresis boundaries",
                  "[unit][world_streaming][stability][hysteresis]") {
            const auto policy = Policy();
            const auto beforeEnter = Decision(policy, Context(), Observation(99));
            REQUIRE(beforeEnter.snapshot.phase == StreamingCellStabilityPhase::Unloaded);

            const auto admitted = Decision(policy, Context(), Observation(100));
            REQUIRE(admitted.snapshot.phase == StreamingCellStabilityPhase::Resident);
            REQUIRE(admitted.snapshot.retainedResidency == StreamingDesiredResidency::Activated);

            const auto held = Decision(policy, Context(1'001, 1), Observation(-200), admitted.snapshot);
            REQUIRE(held.snapshot.phase == StreamingCellStabilityPhase::Resident);
            REQUIRE(held.boundaryHeld);

            const auto exited = Decision(policy, Context(1'002, 1), Observation(-201), held.snapshot);
            REQUIRE(exited.snapshot.phase == StreamingCellStabilityPhase::Lingering);
            REQUIRE(exited.snapshot.lingerStartedAtServiceMilliseconds == 1'002);
        }

        TEST_CASE("Cell stability expires linger exactly and reentry cancels it", "[unit][world_streaming][stability][linger]") {
            const auto policy = Policy();
            const auto admitted = Decision(policy, Context(), Observation(100));
            const auto lingering =
                Decision(policy, Context(2'000, 1), Observation(-500, StreamingDesiredResidency::Unloaded), admitted.snapshot);
            REQUIRE(lingering.snapshot.phase == StreamingCellStabilityPhase::Lingering);

            const auto resumed = Decision(policy, Context(6'999, 1), Observation(-100), lingering.snapshot);
            REQUIRE(resumed.snapshot.phase == StreamingCellStabilityPhase::Resident);
            REQUIRE(resumed.snapshot.lingerStartedAtServiceMilliseconds == 0);

            const auto secondLinger =
                Decision(policy, Context(7'000, 1), Observation(-500, StreamingDesiredResidency::Unloaded), resumed.snapshot);
            const auto expired =
                Decision(policy, Context(12'000, 1), Observation(-500, StreamingDesiredResidency::Unloaded), secondLinger.snapshot);
            REQUIRE(expired.snapshot.phase == StreamingCellStabilityPhase::Unloaded);
            REQUIRE(expired.lingerExpired);
        }

        TEST_CASE("Pinned demand bypasses geometric thresholds without bypassing record capacity",
                  "[unit][world_streaming][stability][pin][capacity]") {
            const auto policy = Policy(1, 1);
            auto pinned = Observation(-10'000, StreamingDesiredResidency::Activated);
            pinned.pinnedResidencyFloor = StreamingDesiredResidency::Loaded;
            REQUIRE(Decision(policy, Context(), pinned).snapshot.phase == StreamingCellStabilityPhase::Resident);
            RequireError(EvaluateStreamingCellStability(policy, Context(1'000, 1), pinned, std::nullopt),
                         WorldStreamingErrors::CellStabilityCapacityExceeded);
            RequireError(EvaluateStreamingCellStability(policy, Context(1'000, 2), Observation(100), std::nullopt),
                         WorldStreamingErrors::CellStabilityCapacityExceeded);

            const auto tracked = Decision(policy, Context(), pinned);
            const auto retiring =
                Decision(policy, Context(2'000, 2), Observation(-10'000, StreamingDesiredResidency::Unloaded), tracked.snapshot);
            REQUIRE(retiring.snapshot.phase == StreamingCellStabilityPhase::Lingering);
        }

        TEST_CASE("Cell stability rejects invalid unsupported stale and closed evaluations",
                  "[unit][world_streaming][stability][failure][lifecycle]") {
            StreamingCellStabilityPolicyRequest invalid{};
            invalid.id = IdentityFrom<StreamingCellStabilityPolicyId>(10);
            invalid.revision = IdentityFrom<StreamingCellStabilityPolicyRevision>(1);
            invalid.enterMarginMillimeters = -1;
            RequireError(StreamingCellStabilityPolicy::Create(invalid), WorldStreamingErrors::CellStabilityInvalid);
            invalid.enterMarginMillimeters = 0;
            invalid.contractVersion = StreamingCellStabilityPolicyRequest::CurrentContractVersion + 1;
            RequireError(StreamingCellStabilityPolicy::Create(invalid), WorldStreamingErrors::CellStabilityUnsupported);

            const auto policy = Policy();
            auto staleContext = Context();
            staleContext.policyRevision = IdentityFrom<StreamingCellStabilityPolicyRevision>(2);
            RequireError(EvaluateStreamingCellStability(policy, staleContext, Observation(100), std::nullopt),
                         WorldStreamingErrors::CellStabilityStale);

            auto closed = Context();
            closed.lifecycle = StreamingCellStabilityLifecycle::Closed;
            RequireError(EvaluateStreamingCellStability(policy, closed, Observation(100), std::nullopt),
                         WorldStreamingErrors::CellStabilityLifecycleUnavailable);

            const auto admitted = Decision(policy, Context(), Observation(100));
            RequireError(EvaluateStreamingCellStability(policy, Context(999, 1), Observation(100), admitted.snapshot),
                         WorldStreamingErrors::CellStabilityStale);

            auto unsupported = Observation(100);
            unsupported.effectiveResidency = static_cast<StreamingDesiredResidency>(std::numeric_limits<std::uint8_t>::max());
            RequireError(EvaluateStreamingCellStability(policy, Context(), unsupported, std::nullopt),
                         WorldStreamingErrors::CellStabilityUnsupported);
        }

        TEST_CASE("Zero linger releases on first loss while no-demand cells consume no record",
                  "[unit][world_streaming][stability][boundary]") {
            const auto policy = Policy(1, 1, 0);
            const auto absent = Decision(policy, Context(1'000, 1), Observation(-500, StreamingDesiredResidency::Unloaded));
            REQUIRE(absent.snapshot.phase == StreamingCellStabilityPhase::Unloaded);

            const auto admitted = Decision(policy, Context(), Observation(100));
            const auto released =
                Decision(policy, Context(1'001, 1), Observation(-500, StreamingDesiredResidency::Unloaded), admitted.snapshot);
            REQUIRE(released.snapshot.phase == StreamingCellStabilityPhase::Unloaded);
            REQUIRE(released.lingerExpired);
        }

        TEST_CASE("Thrashing arms metadata-only cooldown and admits at the exact deadline",
                  "[unit][world_streaming][stability][cooldown]") {
            const auto policy = Policy(1, 1, 100, 500, 2);
            auto state = Decision(policy, Context(0), Observation(100));
            state = Decision(policy, Context(10, 1), Observation(-201), state.snapshot);
            REQUIRE(state.snapshot.boundaryExitCount == 1);
            state = Decision(policy, Context(20, 1), Observation(100), state.snapshot);
            state = Decision(policy, Context(30, 1), Observation(-201), state.snapshot);
            REQUIRE(state.thrashing);
            state = Decision(policy, Context(130, 1), Observation(-201), state.snapshot);
            REQUIRE(state.lingerExpired);
            REQUIRE(state.snapshot.phase == StreamingCellStabilityPhase::Cooldown);
            REQUIRE(state.snapshot.retainedResidency == StreamingDesiredResidency::Unloaded);
            state = Decision(policy, Context(629, 1), Observation(100), state.snapshot);
            REQUIRE(state.cooldownHeld);
            REQUIRE(state.snapshot.cooldownStartedAtServiceMilliseconds == 130);
            state = Decision(policy, Context(630, 1), Observation(100), state.snapshot);
            REQUIRE(state.snapshot.phase == StreamingCellStabilityPhase::Resident);
            REQUIRE_FALSE(state.cooldownHeld);
        }

        TEST_CASE("Pins bypass cooldown while pressure never discards current demand",
                  "[unit][world_streaming][stability][pressure][pin]") {
            const auto policy = Policy(1, 1, 0, 500, 1);
            const auto admitted = Decision(policy, Context(), Observation(100));
            const auto cooling = Decision(policy, Context(1'001, 1), Observation(-201), admitted.snapshot);
            auto context = Context(1'002, 1);
            context.pressure = StreamingCellStabilityPressure::Critical;
            auto pinned = Observation(-1'000);
            pinned.pinnedResidencyFloor = StreamingDesiredResidency::Loaded;
            const auto resumed = Decision(policy, context, pinned, cooling.snapshot);
            REQUIRE(resumed.snapshot.phase == StreamingCellStabilityPhase::Resident);
            REQUIRE_FALSE(resumed.pressureReleased);
            REQUIRE(Decision(policy, context, Observation(100), resumed.snapshot).snapshot.phase == StreamingCellStabilityPhase::Resident);
        }

        TEST_CASE("Pressure caps linger from its original origin and critical pressure releases immediately",
                  "[unit][world_streaming][stability][pressure]") {
            const auto policy = Policy();
            const auto admitted = Decision(policy, Context(), Observation(100));
            const auto lingering = Decision(policy, Context(2'000, 1), Observation(-201), admitted.snapshot);
            auto elevated = Context(2'999, 1);
            elevated.pressure = StreamingCellStabilityPressure::Elevated;
            const auto held = Decision(policy, elevated, Observation(-201), lingering.snapshot);
            REQUIRE(held.snapshot.phase == StreamingCellStabilityPhase::Lingering);
            elevated.serviceTimeMilliseconds = 3'000;
            const auto released = Decision(policy, elevated, Observation(-201), held.snapshot);
            REQUIRE(released.snapshot.phase == StreamingCellStabilityPhase::Unloaded);
            REQUIRE(released.pressureReleased);
            auto critical = Context(2'001, 1);
            critical.pressure = StreamingCellStabilityPressure::Critical;
            REQUIRE(Decision(policy, critical, Observation(-201), lingering.snapshot).pressureReleased);
        }

        TEST_CASE("Thrash windows reset exactly and repeated linger observations do not count exits",
                  "[unit][world_streaming][stability][thrash]") {
            const auto policy = Policy(1, 8, 60'000, 500, 2);
            auto state = Decision(policy, Context(0), Observation(100));
            state = Decision(policy, Context(10, 1), Observation(-201), state.snapshot);
            state = Decision(policy, Context(20, 1), Observation(-201), state.snapshot);
            REQUIRE(state.snapshot.boundaryExitCount == 1);
            state = Decision(policy, Context(30'010, 1), Observation(100), state.snapshot);
            REQUIRE(state.snapshot.boundaryExitCount == 0);
            state = Decision(policy, Context(30'011, 1), Observation(-201), state.snapshot);
            REQUIRE(state.snapshot.boundaryExitCount == 1);
            REQUIRE_FALSE(state.thrashing);
        }

        TEST_CASE("Cooldown expires without demand and does not require overflowing absolute deadlines",
                  "[unit][world_streaming][stability][cooldown][boundary]") {
            const auto policy = Policy(1, 1, 0, 500, 1);
            const auto maximum = std::numeric_limits<std::uint64_t>::max();
            const auto admitted = Decision(policy, Context(maximum - 600), Observation(100));
            const auto cooling = Decision(policy, Context(maximum - 500, 1), Observation(-201), admitted.snapshot);
            REQUIRE(Decision(policy, Context(maximum - 1, 1), Observation(100), cooling.snapshot).cooldownHeld);
            const auto absent =
                Decision(policy, Context(maximum, 1), Observation(-201, StreamingDesiredResidency::Unloaded), cooling.snapshot);
            REQUIRE(absent.snapshot.phase == StreamingCellStabilityPhase::Unloaded);
            REQUIRE_FALSE(absent.lingerExpired);
        }

        TEST_CASE("Pressure and cooldown validate hostile facts without changing prior snapshots",
                  "[unit][world_streaming][stability][failure]") {
            auto request = StreamingCellStabilityPolicyRequest{};
            request.id = IdentityFrom<StreamingCellStabilityPolicyId>(10);
            request.revision = IdentityFrom<StreamingCellStabilityPolicyRevision>(1);
            request.cooldownMilliseconds = StreamingCellStabilityPolicyRequest::MaximumLingerMilliseconds + 1;
            RequireError(StreamingCellStabilityPolicy::Create(request), WorldStreamingErrors::CellStabilityInvalid);
            request.cooldownMilliseconds = 0;
            request.thrashWindowMilliseconds = 0;
            RequireError(StreamingCellStabilityPolicy::Create(request), WorldStreamingErrors::CellStabilityInvalid);
            request.thrashWindowMilliseconds = 1;
            request.thrashExitThreshold = 0;
            RequireError(StreamingCellStabilityPolicy::Create(request), WorldStreamingErrors::CellStabilityInvalid);
            const auto policy = Policy(1, 1, 0, 500, 1);
            const auto admitted = Decision(policy, Context(), Observation(100));
            const auto cooling = Decision(policy, Context(1'001, 1), Observation(-201), admitted.snapshot);
            const auto original = cooling.snapshot;
            auto invalid = Context(1'002, 1);
            invalid.pressure = StreamingCellStabilityPressure::Count;
            RequireError(EvaluateStreamingCellStability(policy, invalid, Observation(100), original),
                         WorldStreamingErrors::CellStabilityUnsupported);
            invalid = Context(1'000, 1);
            RequireError(EvaluateStreamingCellStability(policy, invalid, Observation(100), original),
                         WorldStreamingErrors::CellStabilityStale);
            invalid = Context(1'002, 1);
            invalid.lifecycle = StreamingCellStabilityLifecycle::Cancelling;
            RequireError(EvaluateStreamingCellStability(policy, invalid, Observation(100), original),
                         WorldStreamingErrors::CellStabilityLifecycleUnavailable);
            invalid.lifecycle = StreamingCellStabilityLifecycle::Active;
            invalid.epoch = IdentityFrom<PartitionEpoch>(5);
            RequireError(EvaluateStreamingCellStability(policy, invalid, Observation(100), original),
                         WorldStreamingErrors::CellStabilityStale);
            REQUIRE(cooling.snapshot == original);
        }

        TEST_CASE("Actual releases retain bounded history until cooldown arms or the window expires",
                  "[unit][world_streaming][stability][thrash][lifecycle]") {
            const auto policy = Policy(1, 1, 0, 500, 2);
            auto state = Decision(policy, Context(0), Observation(100));
            state = Decision(policy, Context(10, 1), Observation(-201), state.snapshot);
            REQUIRE(state.snapshot.phase == StreamingCellStabilityPhase::Watching);
            REQUIRE(state.snapshot.retainedResidency == StreamingDesiredResidency::Unloaded);
            REQUIRE(state.snapshot.boundaryExitCount == 1);
            const auto original = state.snapshot;
            auto closed = Context(11, 1);
            closed.lifecycle = StreamingCellStabilityLifecycle::Closed;
            RequireError(EvaluateStreamingCellStability(policy, closed, Observation(100), original),
                         WorldStreamingErrors::CellStabilityLifecycleUnavailable);
            auto malformed = original;
            malformed.retainedResidency = StreamingDesiredResidency::Loaded;
            RequireError(EvaluateStreamingCellStability(policy, Context(11, 1), Observation(100), malformed),
                         WorldStreamingErrors::CellStabilityStale);
            state = Decision(policy, Context(20, 1), Observation(100), state.snapshot);
            REQUIRE(state.snapshot.phase == StreamingCellStabilityPhase::Resident);
            state = Decision(policy, Context(30, 1), Observation(-201), state.snapshot);
            REQUIRE(state.snapshot.phase == StreamingCellStabilityPhase::Cooldown);
            REQUIRE(state.thrashing);
            REQUIRE(Decision(policy, Context(31, 1), Observation(100), state.snapshot).cooldownHeld);

            auto waiting = Decision(policy, Context(30'009, 1), Observation(-201, StreamingDesiredResidency::Unloaded), original);
            REQUIRE(waiting.snapshot.phase == StreamingCellStabilityPhase::Watching);
            waiting = Decision(policy, Context(30'010, 1), Observation(-201, StreamingDesiredResidency::Unloaded), waiting.snapshot);
            REQUIRE(waiting.snapshot.phase == StreamingCellStabilityPhase::Unloaded);
            REQUIRE(waiting.snapshot.boundaryExitCount == 0);
            REQUIRE_FALSE(waiting.thrashing);
        }

    }  // namespace
}  // namespace Horo::WorldStreaming
