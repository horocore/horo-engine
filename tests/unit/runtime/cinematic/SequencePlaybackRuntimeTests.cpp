#include "Horo/Cinematic/SequencePlaybackRuntime.h"
#include "Horo/Cinematic/SequencePlaybackRuntimeErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
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

        template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &expected) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().domain.Value() == expected.domain.Value());
            CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        }
    }  // namespace

    TEST_CASE("Cinematic evaluation tiers enforce aggregate typed limits", "[unit][cinematic][playback][budget]") {
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

    TEST_CASE("Follow-gameplay coordination pauses and establishes a resume baseline", "[unit][cinematic][playback][coordination]") {
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
        CHECK(service.Snapshot(heldHandle).Value().position == 0);
        REQUIRE(service.Evaluate(heldHandle, 5, scratch, {}).HasValue());
        CHECK(service.Snapshot(heldHandle).Value().position == 5);
        REQUIRE(service.Cancel(heldHandle).HasValue());
        REQUIRE(service.Release(heldHandle).HasValue());
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
        CHECK(result.Value().restored == 1);
        CHECK(result.Value().missing == 1);
        CHECK(restored.value == 3.0F);
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

    TEST_CASE("Authority plans return typed gameplay conflict outcomes", "[unit][cinematic][playback][authority]") {
        const std::array claims{SequenceAuthorityClaim{Handle(10), AuthorityTarget(500), CinematicControlChannel::CharacterTranslation,
                                                       CinematicClaimMode::Exclusive, 10, 9, 42, true},
                                SequenceAuthorityClaim{Handle(11), AuthorityTarget(500), CinematicControlChannel::CharacterTranslation,
                                                       CinematicClaimMode::Exclusive, 5, 9, 42, false}};
        auto plan = SequenceAuthorityPlan::Create(9, 42, claims);
        REQUIRE(plan.HasValue());
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
