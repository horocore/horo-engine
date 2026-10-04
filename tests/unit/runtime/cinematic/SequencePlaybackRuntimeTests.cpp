#include "support/SequencePlaybackRuntimeTestSupport.h"

namespace Horo::Cinematic {
    using namespace PlaybackTestSupport;

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
