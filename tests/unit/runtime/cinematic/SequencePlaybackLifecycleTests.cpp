#include "support/SequencePlaybackRuntimeTestSupport.h"

namespace Horo::Cinematic {
    using namespace PlaybackTestSupport;

    TEST_CASE("Runtime service commits positions and terminalizes once playback reaches its end",
              "[unit][cinematic][playback][evaluation]") {
        auto service = Service();
        const auto handleResult = service.Activate({{Handle(), 10, 0, {1, 1}}, Plan()});
        REQUIRE(handleResult.HasValue());
        const auto handle = handleResult.Value();
        REQUIRE(service.Play(handle).HasValue());
        const SequenceFrameScratch scratch{std::span<SequenceSampledValue>{}, std::span<SequenceFrameEventOccurrence>{},
                                           std::span<SequenceFrameCameraCutRequest>{}};
        const SequenceFrameHooks hooks{};
        auto first = service.Evaluate(handle, 5, scratch, hooks);
        REQUIRE(first.HasValue());
        CHECK(first.Value().position == 5);
        CHECK(service.Snapshot(handle).Value().state == SequencePlaybackState::Playing);

        auto completed = service.Evaluate(handle, 5, scratch, hooks);
        REQUIRE(completed.HasValue());
        CHECK(completed.Value().reachedEnd);
        CHECK(service.Snapshot(handle).Value().state == SequencePlaybackState::Stopped);
        CHECK(service.Snapshot(handle).Value().position == 10);
        REQUIRE(service.Release(handle).HasValue());
        CHECK(service.ActivePlayerCount() == 0);
    }

    TEST_CASE("Stop end mode dispatches each crossed event and finishes once after terminal publication",
              "[unit][cinematic][playback][end]") {
        auto service = Service();
        const auto handleResult = service.Activate({{Handle(60), 10, 0, {1, 1}}, EventPlan(SequenceLoopMode::Once)});
        REQUIRE(handleResult.HasValue());
        const auto handle = handleResult.Value();
        PlaybackProbe probe{&service};
        EventScratch scratch;
        REQUIRE(service.Play(handle).HasValue());

        auto first = service.Evaluate(handle, 5, scratch.View(), PlaybackHooks(probe));
        REQUIRE(first.HasValue());
        CHECK(first.Value().firedEvents == 2);
        CHECK(probe.eventCount == 2);
        CHECK(probe.finishedCount == 0);

        auto last = service.Evaluate(handle, 5, scratch.View(), PlaybackHooks(probe));
        REQUIRE(last.HasValue());
        CHECK(last.Value().firedEvents == 1);
        CHECK(last.Value().reachedEnd);
        CHECK(probe.eventCount == 3);
        CHECK(probe.finishedCount == 1);
        CHECK(probe.stateAtFinish == SequencePlaybackState::Stopped);
        CHECK(probe.events[0].key == KeyframeId{1, 1});
        CHECK(probe.events[1].key == KeyframeId{2, 1});
        CHECK(probe.events[2].key == KeyframeId{3, 1});
        RequireError(service.Evaluate(handle, 1, scratch.View(), PlaybackHooks(probe)), SequenceEvaluationErrors::Stale);
        CHECK(probe.finishedCount == 1);
        REQUIRE(service.Cancel(handle).HasValue());
        CHECK(probe.finishedCount == 1);
        REQUIRE(service.Release(handle).HasValue());
    }

    TEST_CASE("Loop end mode retriggers start and interior events on each traversal without finishing",
              "[unit][cinematic][playback][end]") {
        auto service = Service();
        const auto handleResult = service.Activate({{Handle(61), 10, 0, {1, 1}}, EventPlan(SequenceLoopMode::Loop)});
        REQUIRE(handleResult.HasValue());
        const auto handle = handleResult.Value();
        PlaybackProbe probe{&service};
        EventScratch scratch;
        REQUIRE(service.Play(handle).HasValue());
        RequireError(service.Evaluate(handle, 60, scratch.View(), PlaybackHooks(probe)), SequenceEvaluationErrors::CapacityExceeded);
        CHECK(service.Snapshot(handle).Value().position == 0);
        CHECK(probe.eventCount == 0);
        CHECK(probe.finishedCount == 0);
        auto crossed = service.Evaluate(handle, 25, scratch.View(), PlaybackHooks(probe));
        REQUIRE(crossed.HasValue());
        CHECK(crossed.Value().position == 5);
        CHECK(crossed.Value().firedEvents == 8);
        CHECK_FALSE(crossed.Value().reachedEnd);
        CHECK(probe.eventCount == 8);
        CHECK(probe.events[0].traversal == 1);
        CHECK(probe.events[3].traversal == 2);
        CHECK(probe.events[6].traversal == 3);
        CHECK(probe.events[3].key == KeyframeId{1, 1});
        CHECK(probe.events[6].key == KeyframeId{1, 1});
        CHECK(probe.finishedCount == 0);
        REQUIRE(service.Cancel(handle).HasValue());
        CHECK(probe.finishedCount == 0);
        REQUIRE(service.Release(handle).HasValue());
    }

    TEST_CASE("Reverse stop completes once at zero after reverse-eligible events", "[unit][cinematic][playback][end]") {
        auto service = Service();
        const auto handleResult = service.Activate({{Handle(66), 10, 10, {-1, 1}}, EventPlan(SequenceLoopMode::Once)});
        REQUIRE(handleResult.HasValue());
        const auto handle = handleResult.Value();
        PlaybackProbe probe{&service};
        EventScratch scratch;
        REQUIRE(service.Play(handle).HasValue());
        auto completed = service.Evaluate(handle, 10, scratch.View(), PlaybackHooks(probe));
        REQUIRE(completed.HasValue());
        CHECK(completed.Value().position == 0);
        CHECK(completed.Value().firedEvents == 3);
        CHECK(completed.Value().reachedEnd);
        CHECK(probe.events[0].key == KeyframeId{3, 1});
        CHECK(probe.events[1].key == KeyframeId{2, 1});
        CHECK(probe.events[2].key == KeyframeId{1, 1});
        CHECK(probe.events[2].direction == SequenceTraversalDirection::Reverse);
        CHECK(probe.finishedCount == 1);
        REQUIRE(service.Release(handle).HasValue());
    }

    TEST_CASE("Ping-pong end mode reverses events without duplicating turn keys or finishing", "[unit][cinematic][playback][end]") {
        auto service = Service();
        const auto handleResult = service.Activate({{Handle(62), 10, 0, {1, 1}}, EventPlan(SequenceLoopMode::PingPong)});
        REQUIRE(handleResult.HasValue());
        const auto handle = handleResult.Value();
        PlaybackProbe probe{&service};
        EventScratch scratch;
        REQUIRE(service.Play(handle).HasValue());
        auto crossed = service.Evaluate(handle, 25, scratch.View(), PlaybackHooks(probe));
        REQUIRE(crossed.HasValue());
        CHECK(crossed.Value().position == 5);
        CHECK(crossed.Value().firedEvents == 6);
        CHECK_FALSE(crossed.Value().reachedEnd);
        CHECK(probe.eventCount == 6);
        CHECK(probe.events[0].key == KeyframeId{1, 1});
        CHECK(probe.events[2].key == KeyframeId{3, 1});
        CHECK(probe.events[3].direction == SequenceTraversalDirection::Reverse);
        CHECK(probe.events[4].key == KeyframeId{1, 1});
        CHECK(probe.events[5].direction == SequenceTraversalDirection::Forward);
        CHECK(probe.events[2].traversal == 1);
        CHECK(probe.events[3].traversal == 2);
        CHECK(probe.events[5].traversal == 3);
        CHECK(probe.finishedCount == 0);
        REQUIRE(service.BeginShutdown().HasValue());
        CHECK(service.Snapshot(handle).Value().state == SequencePlaybackState::Stopped);
        CHECK(probe.finishedCount == 0);
        REQUIRE(service.Release(handle).HasValue());
    }

    TEST_CASE("Ping-pong skips reverse-disabled keys and retriggers them on the next forward pass", "[unit][cinematic][playback][end]") {
        const std::array events{SequenceFrameEventKey{TrackId{1, 1}, KeyframeId{4, 1}, 5, false}};
        auto plan = SequenceFrameEvaluationPlan::Create(10, SequenceLoopMode::PingPong, 4, {}, events, {});
        REQUIRE(plan.HasValue());
        auto service = Service();
        const auto handleResult = service.Activate({{Handle(67), 10, 0, {1, 1}}, std::move(plan).Value()});
        REQUIRE(handleResult.HasValue());
        const auto handle = handleResult.Value();
        PlaybackProbe probe{&service};
        EventScratch scratch;
        REQUIRE(service.Play(handle).HasValue());
        auto crossed = service.Evaluate(handle, 25, scratch.View(), PlaybackHooks(probe));
        REQUIRE(crossed.HasValue());
        CHECK(crossed.Value().firedEvents == 2);
        CHECK(probe.eventCount == 2);
        CHECK(probe.events[0].traversal == 1);
        CHECK(probe.events[1].traversal == 3);
        CHECK(probe.events[1].direction == SequenceTraversalDirection::Forward);
        CHECK(probe.finishedCount == 0);
    }

    TEST_CASE("Explicit stop and owner destruction bound infinite playback without a finished notification",
              "[unit][cinematic][playback][end][lifetime]") {
        PlaybackProbe probe;
        CoordinationProbe leases;
        {
            auto service = Service();
            probe.service = &service;
            SequencePlaybackActivation activation{{Handle(63), 10, 0, {1, 1}}, EventPlan(SequenceLoopMode::Loop)};
            activation.coordination.hideHud = true;
            activation.coordinationHooks = {&leases, AcquireCoordination, ReleaseCoordination};
            const auto handleResult = service.Activate(std::move(activation));
            REQUIRE(handleResult.HasValue());
            const auto handle = handleResult.Value();
            EventScratch scratch;
            REQUIRE(service.Play(handle).HasValue());
            REQUIRE(service.Evaluate(handle, 12, scratch.View(), PlaybackHooks(probe)).HasValue());
            REQUIRE(service.Stop(handle).HasValue());
            REQUIRE(service.FinishStop(handle).HasValue());
            CHECK(probe.finishedCount == 0);
            CHECK(leases.released == 1);
            REQUIRE(service.Release(handle).HasValue());

            SequencePlaybackActivation liveActivation{{Handle(64), 10, 0, {1, 1}}, EventPlan(SequenceLoopMode::PingPong)};
            liveActivation.coordination.hideHud = true;
            liveActivation.coordinationHooks = {&leases, AcquireCoordination, ReleaseCoordination};
            const auto live = service.Activate(std::move(liveActivation));
            REQUIRE(live.HasValue());
            REQUIRE(service.Play(live.Value()).HasValue());
            REQUIRE(service.Evaluate(live.Value(), 21, scratch.View(), PlaybackHooks(probe)).HasValue());
            CHECK(service.ActivePlayerCount() == 1);
        }
        CHECK(probe.finishedCount == 0);
        CHECK(leases.released == 2);
    }

    TEST_CASE("Scene replacement closes infinite players and rejects old session handles", "[unit][cinematic][playback][end][lifetime]") {
        auto oldService = Service();
        const auto oldHandle = oldService.Activate({{Handle(65), 10, 0, {1, 1}}, EventPlan(SequenceLoopMode::Loop)});
        REQUIRE(oldHandle.HasValue());
        PlaybackProbe probe{&oldService};
        EventScratch scratch;
        REQUIRE(oldService.Play(oldHandle.Value()).HasValue());
        REQUIRE(oldService.Evaluate(oldHandle.Value(), 15, scratch.View(), PlaybackHooks(probe)).HasValue());
        REQUIRE(oldService.BeginShutdown().HasValue());
        CHECK(oldService.Snapshot(oldHandle.Value()).Value().state == SequencePlaybackState::Stopped);
        CHECK(probe.finishedCount == 0);
        REQUIRE(oldService.Release(oldHandle.Value()).HasValue());
        CHECK(oldService.ActivePlayerCount() == 0);

        const CinematicRuntimeSessionId replacementSession{41, 4};
        auto replacement = CinematicRuntimeService::Create({replacementSession, SequenceCookTier::Standard});
        REQUIRE(replacement.HasValue());
        auto replacementService = std::move(replacement).Value();
        const SequencePlayerHandle replacementHandle{replacementSession, oldHandle.Value().player};
        RequireError(replacementService.Snapshot(oldHandle.Value()), SequencePlaybackRuntimeErrors::HandleStale);
        REQUIRE(replacementService.Activate({{replacementHandle, 10, 0, {1, 1}}, EventPlan(SequenceLoopMode::Once)}).HasValue());
        CHECK(probe.finishedCount == 0);
    }

    TEST_CASE("Runtime service keeps loop players alive and qualifies repeated cancel release generations",
              "[unit][cinematic][playback][lifecycle]") {
        auto service = Service();
        const auto loopHandleResult = service.Activate({{Handle(20), 10, 0, {1, 1}}, Plan(SequenceLoopMode::Loop)});
        REQUIRE(loopHandleResult.HasValue());
        const auto loopHandle = loopHandleResult.Value();
        REQUIRE(service.Play(loopHandle).HasValue());
        const SequenceFrameScratch scratch{std::span<SequenceSampledValue>{}, std::span<SequenceFrameEventOccurrence>{},
                                           std::span<SequenceFrameCameraCutRequest>{}};
        REQUIRE(service.Evaluate(loopHandle, 15, scratch, {}).HasValue());
        CHECK(service.Snapshot(loopHandle).Value().state == SequencePlaybackState::Playing);
        CHECK(service.Snapshot(loopHandle).Value().position == 5);
        REQUIRE(service.Cancel(loopHandle).HasValue());
        REQUIRE(service.Release(loopHandle).HasValue());
        RequireError(service.Activate({{Handle(20), 10, 0, {1, 1}}, Plan()}), SequencePlaybackRuntimeErrors::HandleStale);

        for (std::uint32_t generation = 2; generation <= 101; ++generation) {
            const auto handleResult = service.Activate({{Handle(20, generation), 10, 0, {1, 1}}, Plan()});
            REQUIRE(handleResult.HasValue());
            const auto handle = handleResult.Value();
            REQUIRE(service.Play(handle).HasValue());
            REQUIRE(service.Cancel(handle).HasValue());
            REQUIRE(service.Release(handle).HasValue());
        }
        CHECK(service.ActivePlayerCount() == 0);
        RequireError(service.Snapshot(loopHandle), SequencePlaybackRuntimeErrors::HandleStale);
    }

}  // namespace Horo::Cinematic
