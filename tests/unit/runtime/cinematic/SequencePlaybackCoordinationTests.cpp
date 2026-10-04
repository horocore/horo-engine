#include "support/SequencePlaybackRuntimeTestSupport.h"

namespace Horo::Cinematic {
    using namespace PlaybackTestSupport;

    TEST_CASE("Coordination validates time domains", "[unit][cinematic][playback][coordination]") {
        SequencePlaybackSettings authored{};
        authored.clockSource = SequenceClockSource::UnscaledFixedControl;
        authored.pausePolicy = SequencePausePolicy::PlayerOnly;
        authored.pauseGameplay = true;
        authored.hideHud = true;
        CHECK(MakeSequencePlaybackCoordinationSettings(authored) ==
              SequencePlaybackCoordinationSettings{SequenceClockSource::UnscaledFixedControl, SequencePausePolicy::PlayerOnly,
                                                   SequenceDilationPolicy::SourceNative, true, true});
        RequireError(ValidateSequencePlaybackCoordinationSettings({SequenceClockSource::CommittedSimulation,
                                                                   SequencePausePolicy::FollowGameplay,
                                                                   SequenceDilationPolicy::SourceNative, true, false}),
                     SequencePlaybackRuntimeErrors::ActivationInvalid);
        CHECK(ValidateSequencePlaybackCoordinationSettings({SequenceClockSource::UnscaledFixedControl, SequencePausePolicy::PlayerOnly,
                                                            SequenceDilationPolicy::ApplyGameplayScale, true, true})
                  .HasValue());
        RequireError(ValidateSequencePlaybackCoordinationSettings({SequenceClockSource::External, SequencePausePolicy::PlayerOnly,
                                                                   SequenceDilationPolicy::ApplyGameplayScale, false, false}),
                     SequencePlaybackRuntimeErrors::ActivationInvalid);
    }

    TEST_CASE("Coordination leases release only owned tokens", "[unit][cinematic][playback][coordination]") {
        auto service = Service();
        CoordinationProbe probe;
        SequencePlaybackActivation activation{{Handle(40), 10, 0, {1, 1}}, Plan()};
        activation.coordination = {SequenceClockSource::UnscaledFixedControl, SequencePausePolicy::PlayerOnly,
                                   SequenceDilationPolicy::SourceNative, true, true};
        activation.coordinationHooks = {&probe, AcquireCoordination, ReleaseCoordination};
        auto handleResult = service.Activate(std::move(activation));
        REQUIRE(handleResult.HasValue());
        const auto handle = handleResult.Value();
        REQUIRE(probe.acquired == 2);
        auto coordination = service.CoordinationSnapshot(handle);
        REQUIRE(coordination.HasValue());
        CHECK(coordination.Value().gameplayPauseLeaseOwned);
        CHECK(coordination.Value().hudSuppressionLeaseOwned);
        REQUIRE(service.Play(handle).HasValue());
        auto continued = service.ResolveGameplayPause(handle, {3, true});
        REQUIRE(continued.HasValue());
        CHECK(continued.Value().outcome == SequenceGameplayPauseOutcome::ContinuedDuringGameplayPause);
        const SequenceFrameScratch scratch{std::span<SequenceSampledValue>{}, std::span<SequenceFrameEventOccurrence>{},
                                           std::span<SequenceFrameCameraCutRequest>{}};
        REQUIRE(service.Evaluate(handle, 5, scratch, {}).HasValue());
        CHECK(service.Snapshot(handle).Value().position == 5);
        auto stale = service.ResolveGameplayPause(handle, {2, false});
        REQUIRE(stale.HasValue());
        CHECK(stale.Value().outcome == SequenceGameplayPauseOutcome::StaleAuthority);
        REQUIRE(service.Cancel(handle).HasValue());
        CHECK(probe.released == 2);
        REQUIRE(service.Release(handle).HasValue());

        SequencePlaybackActivation failureActivation{{Handle(41), 10, 0, {1, 1}}, Plan()};
        failureActivation.coordination.hideHud = true;
        failureActivation.coordinationHooks = {&probe, AcquireCoordination, ReleaseCoordination};
        auto failedHandle = service.Activate(std::move(failureActivation));
        REQUIRE(failedHandle.HasValue());
        REQUIRE(service.Fail(failedHandle.Value()).HasValue());
        CHECK(probe.released == 3);
        REQUIRE(service.Release(failedHandle.Value()).HasValue());
    }

    TEST_CASE("Natural completion releases only the finishing player's HUD token", "[unit][cinematic][playback][coordination]") {
        auto service = Service();
        CoordinationProbe probe;
        const auto activate = [&](const SequencePlayerHandle player) {
            SequencePlaybackActivation activation{{player, 10, 0, {1, 1}}, Plan()};
            activation.coordination.hideHud = true;
            activation.coordinationHooks = {&probe, AcquireCoordination, ReleaseCoordination};
            return service.Activate(std::move(activation));
        };
        const auto first = activate(Handle(48));
        const auto second = activate(Handle(49));
        REQUIRE(first.HasValue());
        REQUIRE(second.HasValue());
        REQUIRE(service.Play(first.Value()).HasValue());
        REQUIRE(service.Play(second.Value()).HasValue());
        const SequenceFrameScratch scratch{std::span<SequenceSampledValue>{}, std::span<SequenceFrameEventOccurrence>{},
                                           std::span<SequenceFrameCameraCutRequest>{}};
        REQUIRE(service.Evaluate(first.Value(), 10, scratch, {}).HasValue());
        CHECK(probe.released == 1);
        CHECK(service.CoordinationSnapshot(second.Value()).Value().hudSuppressionLeaseOwned);
        REQUIRE(service.Stop(second.Value()).HasValue());
        REQUIRE(service.FinishStop(second.Value()).HasValue());
        CHECK(probe.released == 2);
        REQUIRE(service.Release(first.Value()).HasValue());
        REQUIRE(service.Release(second.Value()).HasValue());
    }

    TEST_CASE("Committed simulation consumes the first committed tick after gameplay resumes",
              "[unit][cinematic][playback][coordination]") {
        auto service = Service();
        auto heldHandleResult = service.Activate({{Handle(42), 10, 0, {1, 1}}, Plan()});
        REQUIRE(heldHandleResult.HasValue());
        const auto heldHandle = heldHandleResult.Value();
        REQUIRE(service.Play(heldHandle).HasValue());
        auto held = service.ResolveGameplayPause(heldHandle, {1, true});
        REQUIRE(held.HasValue());
        CHECK(held.Value().outcome == SequenceGameplayPauseOutcome::HeldByGameplayPause);
        const SequenceFrameScratch scratch{std::span<SequenceSampledValue>{}, std::span<SequenceFrameEventOccurrence>{},
                                           std::span<SequenceFrameCameraCutRequest>{}};
        REQUIRE(service.Evaluate(heldHandle, 5, scratch, {}).HasValue());
        CHECK(service.Snapshot(heldHandle).Value().position == 0);
        auto resumed = service.ResolveGameplayPause(heldHandle, {2, false});
        REQUIRE(resumed.HasValue());
        REQUIRE(service.Evaluate(heldHandle, 5, scratch, {}).HasValue());
        CHECK(service.Snapshot(heldHandle).Value().position == 5);
        REQUIRE(service.Cancel(heldHandle).HasValue());
        REQUIRE(service.Release(heldHandle).HasValue());
    }

    TEST_CASE("Clock samples scale exactly once without fractional drift", "[unit][cinematic][playback][coordination]") {
        auto service = Service();
        SequencePlaybackActivation activation{{Handle(44), 10, 0, {1, 1}}, Plan(SequenceLoopMode::Loop)};
        activation.coordination = {SequenceClockSource::UnscaledFixedControl, SequencePausePolicy::PlayerOnly,
                                   SequenceDilationPolicy::ApplyGameplayScale, false, false};
        const auto admitted = service.Activate(std::move(activation));
        REQUIRE(admitted.HasValue());
        const auto handle = admitted.Value();
        REQUIRE(service.Play(handle).HasValue());
        const SequenceFrameScratch scratch{std::span<SequenceSampledValue>{}, std::span<SequenceFrameEventOccurrence>{},
                                           std::span<SequenceFrameCameraCutRequest>{}};
        SequenceClockSample sample{SequenceClockSource::UnscaledFixedControl, 0, 1, {1, 2}, false};
        REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
        for (SequenceTime tick = 1; tick <= 5; ++tick) {
            sample.position = tick;
            REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
            CHECK(service.Snapshot(handle).Value().position == tick / 2);
        }
        REQUIRE(service.ResolveGameplayPause(handle, {1, true}).Value().outcome ==
                SequenceGameplayPauseOutcome::ContinuedDuringGameplayPause);
        sample.position = 120;
        RequireError(service.EvaluateClock(handle, sample, scratch, {}), SequenceEvaluationErrors::CapacityExceeded);
        sample.position = 7;
        REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
        CHECK(service.Snapshot(handle).Value().position == 3);
        REQUIRE(service.ResolveGameplayPause(handle, {2, false}).Value().outcome == SequenceGameplayPauseOutcome::Unchanged);
        sample.gameplayScale = {2, 1};
        REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
        sample.position = 8;
        REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
        CHECK(service.Snapshot(handle).Value().position == 5);
        sample.source = SequenceClockSource::CommittedSimulation;
        RequireError(service.EvaluateClock(handle, sample, scratch, {}), SequencePlaybackRuntimeErrors::ClockInvalid);
        CHECK(service.Snapshot(handle).Value().position == 5);
        REQUIRE(service.Cancel(handle).HasValue());
        REQUIRE(service.Release(handle).HasValue());
    }

    TEST_CASE("Clock pause and suspension rebase without catch-up", "[unit][cinematic][playback][coordination]") {
        auto service = Service();
        SequencePlaybackActivation activation{{Handle(45), 10, 0, {1, 1}}, Plan(SequenceLoopMode::Loop)};
        activation.coordination = {SequenceClockSource::MonotonicWall, SequencePausePolicy::FollowGameplay,
                                   SequenceDilationPolicy::ApplyGameplayScale, false, false};
        const auto admitted = service.Activate(std::move(activation));
        REQUIRE(admitted.HasValue());
        const auto handle = admitted.Value();
        REQUIRE(service.Play(handle).HasValue());
        const SequenceFrameScratch scratch{std::span<SequenceSampledValue>{}, std::span<SequenceFrameEventOccurrence>{},
                                           std::span<SequenceFrameCameraCutRequest>{}};
        SequenceClockSample sample{SequenceClockSource::MonotonicWall, 0, 1, {1, 2}};
        REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
        sample.position = 1;
        REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
        CHECK(service.Snapshot(handle).Value().position == 0);
        REQUIRE(service.ResolveGameplayPause(handle, {1, true}).Value().outcome == SequenceGameplayPauseOutcome::HeldByGameplayPause);
        sample.position = 7;
        REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
        REQUIRE(service.ResolveGameplayPause(handle, {2, false}).Value().outcome == SequenceGameplayPauseOutcome::Resumed);
        sample.position = 8;
        REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
        CHECK(service.Snapshot(handle).Value().position == 0);
        sample.position = 9;
        REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
        CHECK(service.Snapshot(handle).Value().position == 1);
        sample.position = 20;
        sample.hostSuspended = true;
        REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
        sample.position = 21;
        sample.hostSuspended = false;
        REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
        CHECK(service.Snapshot(handle).Value().position == 1);
        sample.position = 22;
        REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
        CHECK(service.Snapshot(handle).Value().position == 1);
        sample.position = 23;
        REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
        CHECK(service.Snapshot(handle).Value().position == 2);
        REQUIRE(service.Cancel(handle).HasValue());
        REQUIRE(service.Release(handle).HasValue());
    }

    TEST_CASE("Clock epoch changes rebase and reject regressing positions", "[unit][cinematic][playback][coordination]") {
        auto service = Service();
        SequencePlaybackActivation activation{{Handle(48), 10, 0, {1, 1}}, Plan(SequenceLoopMode::Loop)};
        activation.coordination = {SequenceClockSource::MonotonicWall, SequencePausePolicy::FollowGameplay,
                                   SequenceDilationPolicy::ApplyGameplayScale, false, false};
        const auto admitted = service.Activate(std::move(activation));
        REQUIRE(admitted.HasValue());
        const auto handle = admitted.Value();
        REQUIRE(service.Play(handle).HasValue());
        const SequenceFrameScratch scratch{std::span<SequenceSampledValue>{}, std::span<SequenceFrameEventOccurrence>{},
                                           std::span<SequenceFrameCameraCutRequest>{}};
        SequenceClockSample sample{SequenceClockSource::MonotonicWall, 0, 1, {1, 2}};
        REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
        sample.position = 4;
        REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
        CHECK(service.Snapshot(handle).Value().position == 2);
        sample.epoch = 2;
        sample.position = 0;
        REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
        CHECK(service.Snapshot(handle).Value().position == 2);
        sample.position = 2;
        REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
        CHECK(service.Snapshot(handle).Value().position == 3);
        sample.position = 1;
        RequireError(service.EvaluateClock(handle, sample, scratch, {}), SequencePlaybackRuntimeErrors::ClockInvalid);
        CHECK(service.Snapshot(handle).Value().position == 3);
        REQUIRE(service.Cancel(handle).HasValue());
        REQUIRE(service.Release(handle).HasValue());
    }

    TEST_CASE("Clock scaling accepts a large quotient with an unrepresentable intermediate product",
              "[unit][cinematic][playback][coordination]") {
        constexpr SequenceTime maximum = std::numeric_limits<SequenceTime>::max();
        auto plan = SequenceFrameEvaluationPlan::Create(maximum, SequenceLoopMode::Once, 4, {}, {}, {});
        REQUIRE(plan.HasValue());
        auto service = Service();
        SequencePlaybackActivation activation{{Handle(46), maximum, 0, {1, 1}}, std::move(plan).Value()};
        activation.coordination = {SequenceClockSource::UnscaledFixedControl, SequencePausePolicy::PlayerOnly,
                                   SequenceDilationPolicy::ApplyGameplayScale, false, false};
        const auto admitted = service.Activate(std::move(activation));
        REQUIRE(admitted.HasValue());
        const auto handle = admitted.Value();
        REQUIRE(service.Play(handle).HasValue());
        const SequenceFrameScratch scratch{std::span<SequenceSampledValue>{}, std::span<SequenceFrameEventOccurrence>{},
                                           std::span<SequenceFrameCameraCutRequest>{}};
        SequenceClockSample sample{SequenceClockSource::UnscaledFixedControl, 0, 1, {2, 3}};
        REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
        sample.position = maximum;
        REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
        CHECK(service.Snapshot(handle).Value().position == (maximum / 3) * 2 + ((maximum % 3) * 2) / 3);
        REQUIRE(service.Cancel(handle).HasValue());
        REQUIRE(service.Release(handle).HasValue());
    }

    TEST_CASE("Explicit player pause rebases a continuing committed simulation clock", "[unit][cinematic][playback][coordination]") {
        auto service = Service();
        const auto admitted = service.Activate({{Handle(47), 10, 0, {1, 1}}, Plan(SequenceLoopMode::Loop)});
        REQUIRE(admitted.HasValue());
        const auto handle = admitted.Value();
        REQUIRE(service.Play(handle).HasValue());
        const SequenceFrameScratch scratch{std::span<SequenceSampledValue>{}, std::span<SequenceFrameEventOccurrence>{},
                                           std::span<SequenceFrameCameraCutRequest>{}};
        SequenceClockSample sample{SequenceClockSource::CommittedSimulation, 0};
        REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
        sample.position = 2;
        REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
        REQUIRE(service.Pause(handle).HasValue());
        REQUIRE(service.Play(handle).HasValue());
        sample.position = 9;
        REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
        CHECK(service.Snapshot(handle).Value().position == 2);
        sample.position = 10;
        REQUIRE(service.EvaluateClock(handle, sample, scratch, {}).HasValue());
        CHECK(service.Snapshot(handle).Value().position == 3);
        REQUIRE(service.Cancel(handle).HasValue());
        REQUIRE(service.Release(handle).HasValue());
    }

    TEST_CASE("Runtime service destruction releases live coordination leases", "[unit][cinematic][playback][lifetime]") {
        CoordinationProbe probe;
        {
            auto service = Service();
            SequencePlaybackActivation activation{{Handle(43), 10, 0, {1, 1}}, Plan()};
            activation.coordination = {SequenceClockSource::UnscaledFixedControl, SequencePausePolicy::PlayerOnly,
                                       SequenceDilationPolicy::SourceNative, true, true};
            activation.coordinationHooks = {&probe, AcquireCoordination, ReleaseCoordination};
            REQUIRE(service.Activate(std::move(activation)).HasValue());
        }
        CHECK(probe.released == 2);
    }

}  // namespace Horo::Cinematic
