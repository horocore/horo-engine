#include "Horo/Cinematic/SequenceEvaluationErrors.h"
#include "Horo/Cinematic/SequencePlaybackRuntime.h"
#include "Horo/Cinematic/SequencePlaybackRuntimeErrors.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace Horo::Cinematic {
    namespace {
        [[nodiscard]] constexpr CinematicRuntimeSessionId Session() {
            return {41, 3};
        }

        [[nodiscard]] constexpr SequencePlayerHandle Handle(const std::uint64_t player = 7, const std::uint32_t generation = 1) {
            return {Session(), {player, generation}};
        }

        [[nodiscard]] constexpr SequenceRestoreTargetId RestoreTarget(const std::uint64_t value, const std::uint32_t generation = 1) {
            return {value, generation};
        }

        [[nodiscard]] constexpr SequenceAuthorityTargetId AuthorityTarget(const std::uint64_t value, const std::uint32_t generation = 1) {
            return {value, generation};
        }

        [[nodiscard]] SequenceFrameEvaluationPlan Plan(const SequenceLoopMode loop = SequenceLoopMode::Once) {
            auto plan = SequenceFrameEvaluationPlan::Create(10, loop, 4, {}, {}, {});
            REQUIRE(plan.HasValue());
            return std::move(plan).Value();
        }

        [[nodiscard]] CinematicRuntimeService Service() {
            auto service = CinematicRuntimeService::Create({Session(), SequenceCookTier::Standard});
            REQUIRE(service.HasValue());
            return std::move(service).Value();
        }

        struct RestoreProbe final {
            float value{};
        };

        bool ApplyRestore(void *context, const float value) noexcept {
            static_cast<RestoreProbe *>(context)->value = value;
            return true;
        }

        bool RejectRestore(void *, float) noexcept {
            return false;
        }

        struct ValueProbe final {
            float value{4.0F};
            std::size_t writes{};
        };

        Result<float> SampleValue(const void *, const SequenceTime time) {
            return Result<float>::Success(10.0F + static_cast<float>(time));
        }

        void ApplyValue(void *context, const float value) noexcept {
            auto &probe = *static_cast<ValueProbe *>(context);
            probe.value = value;
            ++probe.writes;
        }

        struct CoordinationProbe final {
            std::uint64_t nextRevision{1};
            std::size_t acquired{};
            std::size_t released{};
        };

        Result<SequenceCoordinationLease> AcquireCoordination(void *context, const SequencePlayerHandle &player,
                                                              const SequenceCoordinationLeaseKind kind) {
            auto &probe = *static_cast<CoordinationProbe *>(context);
            ++probe.acquired;
            return Result<SequenceCoordinationLease>::Success({player, kind, probe.nextRevision++});
        }

        void ReleaseCoordination(void *context, const SequenceCoordinationLease &) noexcept {
            ++static_cast<CoordinationProbe *>(context)->released;
        }

        struct PlaybackProbe final {
            CinematicRuntimeService *service{};
            std::array<SequenceFrameEventOccurrence, 16> events{};
            std::size_t eventCount{};
            std::size_t finishedCount{};
            SequencePlaybackState stateAtFinish{SequencePlaybackState::Ready};
        };

        void OnPlaybackEvent(void *context, const SequenceFrameEventOccurrence &event) noexcept {
            auto &probe = *static_cast<PlaybackProbe *>(context);
            probe.events[probe.eventCount++] = event;
        }

        void OnPlaybackFinished(void *context, const SequencePlayerHandle &handle) noexcept {
            auto &probe = *static_cast<PlaybackProbe *>(context);
            ++probe.finishedCount;
            const auto snapshot = probe.service->Snapshot(handle);
            if (snapshot.HasValue())
                probe.stateAtFinish = snapshot.Value().state;
        }

        [[nodiscard]] SequenceFrameHooks PlaybackHooks(PlaybackProbe &probe) {
            return {&probe, OnPlaybackEvent, nullptr, nullptr, &probe, OnPlaybackFinished};
        }

        [[nodiscard]] SequenceFrameEvaluationPlan EventPlan(const SequenceLoopMode mode) {
            const std::array events{SequenceFrameEventKey{TrackId{1, 1}, KeyframeId{1, 1}, 0, true},
                                    SequenceFrameEventKey{TrackId{1, 1}, KeyframeId{2, 1}, 5, true},
                                    SequenceFrameEventKey{TrackId{1, 1}, KeyframeId{3, 1}, 10, true}};
            auto plan = SequenceFrameEvaluationPlan::Create(10, mode, 4, {}, events, {});
            REQUIRE(plan.HasValue());
            return std::move(plan).Value();
        }

        struct EventScratch final {
            std::array<SequenceFrameEventOccurrence, 16> events{};

            [[nodiscard]] SequenceFrameScratch View() {
                return {{}, events, {}};
            }
        };

        template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().domain.Value() == expected.domain.Value());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }
    }  // namespace

    TEST_CASE("Cinematic evaluation tiers enforce aggregate typed limits", "[unit][cinematic][playback][budget]") {
        CHECK(Plan(SequenceLoopMode::Loop).LoopMode() == SequenceLoopMode::Loop);
        const auto compact = GetSequenceEvaluationBudget(SequenceCookTier::Compact);
        REQUIRE(compact.HasValue());
        CHECK(compact.Value().maximumActivePlayers == 2);
        CHECK(compact.Value().maximumAggregateTracks == 64);
        CHECK(compact.Value().maximumBoundaryOccurrences == 64);
        CHECK(compact.Value().maximumLoopCrossings == 4);

        const SequenceEvaluationUsage current{1, 32, 16, 100, 4};
        const SequenceEvaluationUsage admitted{1, 32, 48, 100, 4};
        CHECK(AdmitSequenceEvaluationUsage(current, admitted, compact.Value()).HasValue());
        const SequenceEvaluationUsage overflow{2, 1, 1, 1, 1};
        RequireError(AdmitSequenceEvaluationUsage(current, overflow, compact.Value()), SequencePlaybackRuntimeErrors::CapacityExceeded);
        RequireError(GetSequenceEvaluationBudget(static_cast<SequenceCookTier>(99)), SequencePlaybackRuntimeErrors::BudgetInvalid);

        const std::array tiers{std::pair{SequenceCookTier::Compact, 2U}, std::pair{SequenceCookTier::Standard, 8U},
                               std::pair{SequenceCookTier::Large, 32U}};
        for (const auto [tier, capacity] : tiers) {
            auto serviceResult = CinematicRuntimeService::Create({Session(), tier});
            REQUIRE(serviceResult.HasValue());
            auto service = std::move(serviceResult).Value();
            for (std::uint32_t index = 0; index < capacity; ++index)
                REQUIRE(service.Activate({{Handle(100 + index), 10, 0, {1, 1}}, Plan()}).HasValue());
            RequireError(service.Activate({{Handle(10'000 + capacity), 10, 0, {1, 1}}, Plan()}),
                         SequencePlaybackRuntimeErrors::CapacityExceeded);
            CHECK(service.ActivePlayerCount() == capacity);
        }
    }

    TEST_CASE("Blend helpers are bounded and deterministic", "[unit][cinematic][playback][blend]") {
        CHECK(ValidateSequencePlaybackBlendSettings(
                  {{SequenceBlendMode::Blend, 10}, {SequenceBlendMode::Cut, 0}, SequenceRestorePolicy::KeepFinalState})
                  .HasValue());
        RequireError(ValidateSequencePlaybackBlendSettings(
                         {{SequenceBlendMode::Blend, 0}, {SequenceBlendMode::Cut, 0}, SequenceRestorePolicy::KeepFinalState}),
                     SequencePlaybackRuntimeErrors::ActivationInvalid);
        CHECK(EvaluateSequenceBlendWeight(5, 10).Value() == 0.5F);
        CHECK(EvaluateSequenceBlendWeight(20, 10).Value() == 1.0F);
        CHECK(BlendSequenceScalar(10.0F, 20.0F, 0.25F).Value() == 12.5F);
        RequireError(BlendSequenceScalar(0.0F, 1.0F, 2.0F), SequencePlaybackRuntimeErrors::ActivationInvalid);
    }

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

    TEST_CASE("Restore snapshots never write destroyed or replaced targets", "[unit][cinematic][playback][restore]") {
        const std::array entries{SequenceRestoreEntry{TrackId{1, 1}, RestoreTarget(100), 4, 3.0F},
                                 SequenceRestoreEntry{TrackId{2, 1}, RestoreTarget(200), 7, 8.0F}};
        auto snapshot = SequenceRestoreSnapshot::Create(entries);
        REQUIRE(snapshot.HasValue());

        RestoreProbe restored{};
        const std::array current{SequenceRestoreTargetSnapshot{RestoreTarget(100), 4, &restored, ApplyRestore},
                                 SequenceRestoreTargetSnapshot{}};
        std::array<SequenceRestoreDiagnostic, 2> diagnostics{};
        auto result = ApplySequenceRestoreSnapshot(snapshot.Value(), current, diagnostics);
        REQUIRE(result.HasValue());
        CHECK(result.Value().restored == 0);
        CHECK(result.Value().missing == 1);
        CHECK(result.Value().keptFinal == 1);
        CHECK(restored.value == 0.0F);
        CHECK(diagnostics[0].outcome == SequenceRestoreOutcome::KeptFinalDueToMissingTarget);
        CHECK(diagnostics[1].outcome == SequenceRestoreOutcome::TargetMissing);

        const std::array reordered{SequenceRestoreTargetSnapshot{RestoreTarget(200), 7, &restored, ApplyRestore},
                                   SequenceRestoreTargetSnapshot{RestoreTarget(100), 4, &restored, ApplyRestore}};
        result = ApplySequenceRestoreSnapshot(snapshot.Value(), reordered, diagnostics);
        REQUIRE(result.HasValue());
        CHECK(result.Value().restored == 2);
        CHECK(restored.value == 8.0F);

        const std::array stale{SequenceRestoreTargetSnapshot{RestoreTarget(100), 5, &restored, ApplyRestore},
                               SequenceRestoreTargetSnapshot{RestoreTarget(200), 7, &restored, ApplyRestore}};
        result = ApplySequenceRestoreSnapshot(snapshot.Value(), stale, diagnostics);
        REQUIRE(result.HasValue());
        CHECK(result.Value().stale == 1);
        CHECK(result.Value().keptFinal == 1);
        CHECK(diagnostics[0].outcome == SequenceRestoreOutcome::StaleGeneration);

        const std::array rejected{SequenceRestoreTargetSnapshot{RestoreTarget(100), 4, &restored, RejectRestore},
                                  SequenceRestoreTargetSnapshot{RestoreTarget(200), 7, &restored, ApplyRestore}};
        result = ApplySequenceRestoreSnapshot(snapshot.Value(), rejected, diagnostics);
        REQUIRE(result.HasValue());
        CHECK(result.Value().rejected == 1);
        CHECK(diagnostics[0].outcome == SequenceRestoreOutcome::WriteRejected);

        const std::array duplicate{entries[0], SequenceRestoreEntry{TrackId{3, 1}, RestoreTarget(100), 4, 9.0F}};
        RequireError(SequenceRestoreSnapshot::Create(duplicate), SequencePlaybackRuntimeErrors::RestoreInvalid);
    }

    TEST_CASE("Playback blends both edges from the captured owner value and restores exactly",
              "[unit][cinematic][playback][blend][restore]") {
        ValueProbe probe{};
        const std::array tracks{SequenceFrameTrackDescriptor{TrackId{1, 1}, SequenceApplyStage::Property, &probe, SampleValue, ApplyValue}};
        auto plan = SequenceFrameEvaluationPlan::Create(10, SequenceLoopMode::Once, 4, tracks, {}, {});
        REQUIRE(plan.HasValue());
        const std::array captured{SequenceRestoreEntry{TrackId{1, 1}, RestoreTarget(900), 2, probe.value}};
        auto restore = SequenceRestoreSnapshot::Create(captured);
        REQUIRE(restore.HasValue());
        SequencePlaybackActivation activation{{Handle(90), 10, 0, {1, 1}}, std::move(plan).Value()};
        activation.blend = {{SequenceBlendMode::Blend, 4}, {SequenceBlendMode::Blend, 4}, SequenceRestorePolicy::RestorePrePlayback};
        activation.restore = std::move(restore).Value();
        auto service = Service();
        const auto admitted = service.Activate(std::move(activation));
        REQUIRE(admitted.HasValue());
        const auto handle = admitted.Value();
        std::array<SequenceSampledValue, 1> values{};
        const SequenceFrameScratch scratch{values, {}, {}};
        REQUIRE(service.Play(handle).HasValue());
        const std::size_t allocationsBefore = Horo::Tests::AllocationProbe::Count();
        auto first = service.Evaluate(handle, 2, scratch, {});
        const std::size_t allocationsAfter = Horo::Tests::AllocationProbe::Count();
        REQUIRE(first.HasValue());
        CHECK(allocationsAfter == allocationsBefore);
        CHECK(probe.value == 8.0F);
        REQUIRE(service.Evaluate(handle, 3, scratch, {}).HasValue());
        CHECK(probe.value == 15.0F);
        REQUIRE(service.Evaluate(handle, 3, scratch, {}).HasValue());
        CHECK(probe.value == 11.0F);
        REQUIRE(service.Evaluate(handle, 2, scratch, {}).HasValue());
        CHECK(probe.value == 4.0F);
        RequireError(service.Release(handle), SequencePlaybackRuntimeErrors::RestoreRequired);
        probe.value = 99.0F;
        const std::array targets{SequenceRestoreTargetSnapshot{RestoreTarget(900), 2, &probe, [](void *context, float value) noexcept {
            ApplyValue(context, value);
            return true;
        }}};
        std::array<SequenceRestoreDiagnostic, 1> diagnostics{};
        auto result = service.Restore(handle, targets, diagnostics);
        REQUIRE(result.HasValue());
        CHECK(result.Value().restored == 1);
        CHECK(probe.value == 4.0F);
        CHECK(diagnostics[0].outcome == SequenceRestoreOutcome::Restored);
        REQUIRE(service.Release(handle).HasValue());
    }

    TEST_CASE("Destroyed restore target keeps every final value and emits typed diagnostics", "[unit][cinematic][playback][restore]") {
        const std::array entries{SequenceRestoreEntry{TrackId{1, 1}, RestoreTarget(1), 2, 1.0F},
                                 SequenceRestoreEntry{TrackId{2, 1}, RestoreTarget(2), 3, 2.0F}};
        auto snapshot = SequenceRestoreSnapshot::Create(entries);
        REQUIRE(snapshot.HasValue());
        RestoreProbe surviving{10.0F};
        const std::array targets{SequenceRestoreTargetSnapshot{RestoreTarget(1), 2, &surviving, ApplyRestore}};
        std::array<SequenceRestoreDiagnostic, 2> diagnostics{};
        auto outcome = ApplySequenceRestoreSnapshot(snapshot.Value(), targets, diagnostics);
        REQUIRE(outcome.HasValue());
        CHECK(outcome.Value().restored == 0);
        CHECK(outcome.Value().missing == 1);
        CHECK(outcome.Value().keptFinal == 1);
        CHECK(surviving.value == 10.0F);
        CHECK(diagnostics[0].outcome == SequenceRestoreOutcome::KeptFinalDueToMissingTarget);
        CHECK(diagnostics[1].outcome == SequenceRestoreOutcome::TargetMissing);

        SequencePlaybackActivation activation{{Handle(91), 10, 0, {1, 1}}, Plan()};
        activation.blend.restorePolicy = SequenceRestorePolicy::RestorePrePlayback;
        activation.restore = std::move(snapshot).Value();
        auto service = Service();
        auto admitted = service.Activate(std::move(activation));
        REQUIRE(admitted.HasValue());
        const auto handle = admitted.Value();
        REQUIRE(service.Stop(handle).HasValue());
        REQUIRE(service.FinishStop(handle).HasValue());
        RequireError(service.Release(handle), SequencePlaybackRuntimeErrors::RestoreRequired);
        auto terminal = service.Restore(handle, targets, diagnostics);
        REQUIRE(terminal.HasValue());
        CHECK(terminal.Value().keptFinal == 1);
        CHECK(surviving.value == 10.0F);
        REQUIRE(service.Release(handle).HasValue());
    }

    TEST_CASE("Authority plans return typed gameplay conflict outcomes", "[unit][cinematic][playback][authority]") {
        const std::array claims{SequenceAuthorityClaim{Handle(10), AuthorityTarget(500), CinematicControlChannel::CharacterTranslation,
                                                       CinematicClaimMode::Exclusive, 10, 9, 42, true},
                                SequenceAuthorityClaim{Handle(11), AuthorityTarget(500), CinematicControlChannel::CharacterTranslation,
                                                       CinematicClaimMode::Exclusive, 5, 9, 42, false}};
        auto plan = SequenceAuthorityPlan::Create(9, 42, claims);
        REQUIRE(plan.HasValue());
        CHECK(plan.Value().AuthorityRevision() == 9);
        CHECK(plan.Value().EligibleSimulationTick() == 42);
        auto suppressed = plan.Value().ResolveGameplayWrite({AuthorityTarget(500), CinematicControlChannel::CharacterTranslation, 9, 42});
        REQUIRE(suppressed.HasValue());
        CHECK(suppressed.Value().outcome == SequenceGameplayWriteOutcome::SuppressedByCinematic);
        CHECK(suppressed.Value().owner == Handle(10));

        auto accepted = plan.Value().ResolveGameplayWrite({AuthorityTarget(501), CinematicControlChannel::CharacterTranslation, 9, 42});
        REQUIRE(accepted.HasValue());
        CHECK(accepted.Value().outcome == SequenceGameplayWriteOutcome::AcceptedGameplay);

        auto stale = plan.Value().ResolveGameplayWrite({AuthorityTarget(500), CinematicControlChannel::CharacterTranslation, 9, 43});
        REQUIRE(stale.HasValue());
        CHECK(stale.Value().outcome == SequenceGameplayWriteOutcome::StaleAuthority);

        const std::array equalRequired{claims[0], SequenceAuthorityClaim{Handle(11), AuthorityTarget(500),
                                                                         CinematicControlChannel::CharacterTranslation,
                                                                         CinematicClaimMode::Exclusive, 10, 9, 42, true}};
        RequireError(SequenceAuthorityPlan::Create(9, 42, equalRequired), SequencePlaybackRuntimeErrors::AuthorityConflict);

        const std::array blendClaim{SequenceAuthorityClaim{Handle(12), AuthorityTarget(600), CinematicControlChannel::AnimationParameter,
                                                           CinematicClaimMode::Blend, 1, 9, 42, true}};
        auto blendPlan = SequenceAuthorityPlan::Create(9, 42, blendClaim);
        REQUIRE(blendPlan.HasValue());
        auto blended = blendPlan.Value().ResolveGameplayWrite({AuthorityTarget(600), CinematicControlChannel::AnimationParameter, 9, 42});
        REQUIRE(blended.HasValue());
        CHECK(blended.Value().outcome == SequenceGameplayWriteOutcome::AcceptedForOwnerBlend);

        const std::array observeOnly{SequenceAuthorityClaim{Handle(13), AuthorityTarget(601), CinematicControlChannel::SkeletalPose,
                                                            CinematicClaimMode::ObserveOnly, 1, 9, 42, false}};
        auto observePlan = SequenceAuthorityPlan::Create(9, 42, observeOnly);
        REQUIRE(observePlan.HasValue());
        auto observed = observePlan.Value().ResolveGameplayWrite({AuthorityTarget(601), CinematicControlChannel::SkeletalPose, 9, 42});
        REQUIRE(observed.HasValue());
        CHECK(observed.Value().outcome == SequenceGameplayWriteOutcome::AcceptedGameplay);

        const std::array incompatible{claims[0], SequenceAuthorityClaim{Handle(11), AuthorityTarget(500),
                                                                        CinematicControlChannel::CharacterTranslation,
                                                                        CinematicClaimMode::Blend, 5, 9, 42, true}};
        RequireError(SequenceAuthorityPlan::Create(9, 42, incompatible), SequencePlaybackRuntimeErrors::AuthorityConflict);

        const std::array staleGeneration{claims[0], SequenceAuthorityClaim{Handle(11), AuthorityTarget(500, 2),
                                                                           CinematicControlChannel::CharacterTranslation,
                                                                           CinematicClaimMode::Exclusive, 5, 9, 42, false}};
        RequireError(SequenceAuthorityPlan::Create(9, 42, staleGeneration), SequencePlaybackRuntimeErrors::AuthorityConflict);
    }

    TEST_CASE("Playback activation rejects mismatched plans and simulation claims during host pause",
              "[unit][cinematic][playback][activation]") {
        auto service = Service();
        auto mismatched = SequenceFrameEvaluationPlan::Create(9, SequenceLoopMode::Once, 4, {}, {}, {});
        REQUIRE(mismatched.HasValue());
        RequireError(service.Activate({{Handle(50), 10, 0, {1, 1}}, std::move(mismatched).Value()}),
                     SequencePlaybackRuntimeErrors::ActivationInvalid);

        const std::array claims{SequenceAuthorityClaim{Handle(51), AuthorityTarget(700), CinematicControlChannel::CharacterTranslation,
                                                       CinematicClaimMode::Exclusive, 1, 1, 1, true}};
        auto authority = SequenceAuthorityPlan::Create(1, 1, claims);
        REQUIRE(authority.HasValue());
        SequencePlaybackActivation activation{{Handle(51), 10, 0, {1, 1}}, Plan()};
        activation.coordination = {SequenceClockSource::UnscaledFixedControl, SequencePausePolicy::PlayerOnly,
                                   SequenceDilationPolicy::SourceNative, true, false};
        activation.authority = std::move(authority).Value();
        RequireError(service.Activate(std::move(activation)), SequencePlaybackRuntimeErrors::ActivationInvalid);
    }

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

    TEST_CASE("Runtime service restores at terminal boundary and reports authority through the active player",
              "[unit][cinematic][playback][integration]") {
        auto service = Service();
        const std::array claims{SequenceAuthorityClaim{Handle(30), AuthorityTarget(900), CinematicControlChannel::GameplayAction,
                                                       CinematicClaimMode::Exclusive, 4, 3, 8, true}};
        auto authority = SequenceAuthorityPlan::Create(3, 8, claims);
        REQUIRE(authority.HasValue());
        const std::array entries{SequenceRestoreEntry{TrackId{9, 1}, RestoreTarget(901), 2, 11.0F}};
        auto restore = SequenceRestoreSnapshot::Create(entries);
        REQUIRE(restore.HasValue());
        const SequencePlaybackBlendSettings blend{{SequenceBlendMode::Cut, 0},
                                                  {SequenceBlendMode::Cut, 0},
                                                  SequenceRestorePolicy::RestorePrePlayback};
        auto handleResult =
            service.Activate({{Handle(30), 10, 0, {1, 1}}, Plan(), blend, std::move(authority).Value(), std::move(restore).Value(), 128});
        REQUIRE(handleResult.HasValue());
        const auto handle = handleResult.Value();
        auto decision = service.ResolveGameplayWrite(handle, {AuthorityTarget(900), CinematicControlChannel::GameplayAction, 3, 8});
        REQUIRE(decision.HasValue());
        CHECK(decision.Value().outcome == SequenceGameplayWriteOutcome::SuppressedByCinematic);

        REQUIRE(service.Cancel(handle).HasValue());
        decision = service.ResolveGameplayWrite(handle, {AuthorityTarget(900), CinematicControlChannel::GameplayAction, 3, 8});
        REQUIRE(decision.HasValue());
        CHECK(decision.Value().outcome == SequenceGameplayWriteOutcome::AcceptedGameplay);
        RestoreProbe probe{};
        const std::array targets{SequenceRestoreTargetSnapshot{RestoreTarget(901), 2, &probe, ApplyRestore}};
        std::array<SequenceRestoreDiagnostic, 1> diagnostics{};
        auto restored = service.Restore(handle, targets, diagnostics);
        REQUIRE(restored.HasValue());
        CHECK(restored.Value().restored == 1);
        CHECK(probe.value == 11.0F);
        REQUIRE(service.Release(handle).HasValue());
    }
}  // namespace Horo::Cinematic
