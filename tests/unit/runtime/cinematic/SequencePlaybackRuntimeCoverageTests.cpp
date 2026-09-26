#include "Horo/Cinematic/SequencePlaybackRuntime.h"
#include "Horo/Cinematic/SequencePlaybackRuntimeErrors.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <optional>
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

        [[nodiscard]] SequenceFrameEvaluationPlan Plan() {
            auto plan = SequenceFrameEvaluationPlan::Create(10, SequenceLoopMode::Once, 4, {}, {}, {});
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

        Result<SequenceCoordinationLease> RejectCoordination(void *, const SequencePlayerHandle &, SequenceCoordinationLeaseKind) {
            return Result<SequenceCoordinationLease>::Failure(MakeError(SequencePlaybackRuntimeErrors::CapacityExceeded));
        }

        Result<SequenceCoordinationLease> RejectHudCoordination(void *context, const SequencePlayerHandle &player,
                                                                const SequenceCoordinationLeaseKind kind) {
            if (kind == SequenceCoordinationLeaseKind::HudSuppression)
                return Result<SequenceCoordinationLease>::Failure(MakeError(SequencePlaybackRuntimeErrors::CapacityExceeded));
            return AcquireCoordination(context, player, kind);
        }

        Result<SequenceCoordinationLease> InvalidCoordination(void *, const SequencePlayerHandle &, SequenceCoordinationLeaseKind) {
            return Result<SequenceCoordinationLease>::Success({});
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

    TEST_CASE("Runtime service rejects invalid and stale handles", "[unit][cinematic][playback][lifecycle]") {
        auto service = Service();
        const SequencePlayerHandle invalid{};
        const SequencePlayerHandle unknown{{99, 3}, {7, 1}};
        const SequencePlayerHandle staleSession{{41, 2}, {7, 1}};
        const SequencePlayerHandle unknownPlayer = Handle(999);
        const SequenceFrameScratch scratch{std::span<SequenceSampledValue>{}, std::span<SequenceFrameEventOccurrence>{},
                                           std::span<SequenceFrameCameraCutRequest>{}};
        std::array<SequenceRestoreDiagnostic, 0> noDiagnostics{};
        RequireError(service.Snapshot(invalid), SequencePlaybackRuntimeErrors::HandleInvalid);
        RequireError(service.Snapshot(unknown), SequencePlaybackRuntimeErrors::HandleUnknown);
        RequireError(service.Snapshot(staleSession), SequencePlaybackRuntimeErrors::HandleStale);
        RequireError(service.Snapshot(unknownPlayer), SequencePlaybackRuntimeErrors::HandleUnknown);
        RequireError(service.Play(invalid), SequencePlaybackRuntimeErrors::HandleInvalid);
        RequireError(service.Pause(invalid), SequencePlaybackRuntimeErrors::HandleInvalid);
        RequireError(service.Stop(invalid), SequencePlaybackRuntimeErrors::HandleInvalid);
        RequireError(service.FinishStop(invalid), SequencePlaybackRuntimeErrors::HandleInvalid);
        RequireError(service.Seek(invalid, 1), SequencePlaybackRuntimeErrors::HandleInvalid);
        RequireError(service.SetPlaybackSpeed(invalid, {1, 1}), SequencePlaybackRuntimeErrors::HandleInvalid);
        RequireError(service.Cancel(invalid), SequencePlaybackRuntimeErrors::HandleInvalid);
        RequireError(service.Fail(invalid), SequencePlaybackRuntimeErrors::HandleInvalid);
        RequireError(service.Evaluate(invalid, 1, scratch, {}), SequencePlaybackRuntimeErrors::HandleInvalid);
        RequireError(service.ResolveGameplayPause(invalid, {1, false}), SequencePlaybackRuntimeErrors::HandleInvalid);
        RequireError(service.CoordinationSnapshot(invalid), SequencePlaybackRuntimeErrors::HandleInvalid);
        RequireError(service.ResolveGameplayWrite(invalid, SequenceGameplayWriteRequest{}), SequencePlaybackRuntimeErrors::HandleInvalid);
        RequireError(service.Restore(invalid, std::span<const SequenceRestoreTargetSnapshot>{}, noDiagnostics),
                     SequencePlaybackRuntimeErrors::HandleInvalid);
        RequireError(service.Release(invalid), SequencePlaybackRuntimeErrors::HandleInvalid);
    }

    TEST_CASE("Runtime service completes terminal command boundaries", "[unit][cinematic][playback][lifecycle]") {
        auto service = Service();
        std::array<SequenceRestoreDiagnostic, 0> noDiagnostics{};
        const SequenceFrameScratch scratch{std::span<SequenceSampledValue>{}, std::span<SequenceFrameEventOccurrence>{},
                                           std::span<SequenceFrameCameraCutRequest>{}};
        auto handleResult = service.Activate({{Handle(), 10, 0, {1, 1}}, Plan()});
        REQUIRE(handleResult.HasValue());
        const auto handle = handleResult.Value();
        RequireError(service.Snapshot(Handle(7, 2)), SequencePlaybackRuntimeErrors::HandleStale);
        RequireError(service.Restore(handle, std::span<const SequenceRestoreTargetSnapshot>{}, noDiagnostics),
                     SequencePlaybackRuntimeErrors::PlayerNotTerminal);
        RequireError(service.Release(handle), SequencePlaybackRuntimeErrors::PlayerNotTerminal);
        const std::array invalidOrder{SequenceFramePlayerOrder{SequencePlayerHandle{}, 0}};
        std::array<SequenceFramePlayerOrder, 1> ordered{};
        RequireError(service.OrderActivePlayers(invalidOrder, ordered), SequencePlaybackRuntimeErrors::HandleInvalid);
        const std::array order{SequenceFramePlayerOrder{handle, 0}};
        REQUIRE(service.OrderActivePlayers(order, ordered).Value() == 1);
        REQUIRE(service.Play(handle).HasValue());
        REQUIRE(service.Pause(handle).HasValue());
        REQUIRE(service.Play(handle).HasValue());
        REQUIRE(service.Seek(handle, 3).HasValue());
        REQUIRE(service.SetPlaybackSpeed(handle, {2, 1}).HasValue());
        REQUIRE(service.Stop(handle).HasValue());
        REQUIRE(service.Stop(handle).HasValue());
        REQUIRE(service.FinishStop(handle).HasValue());
        RequireError(service.ResolveGameplayWrite(handle, SequenceGameplayWriteRequest{}),
                     SequencePlaybackRuntimeErrors::ActivationInvalid);
        auto accepted = service.ResolveGameplayWrite(handle, {AuthorityTarget(500), CinematicControlChannel::CharacterTranslation, 1, 1});
        REQUIRE(accepted.HasValue());
        CHECK(accepted.Value().outcome == SequenceGameplayWriteOutcome::AcceptedGameplay);
        REQUIRE(service.Restore(handle, std::span<const SequenceRestoreTargetSnapshot>{}, noDiagnostics).HasValue());
        REQUIRE(service.Restore(handle, std::span<const SequenceRestoreTargetSnapshot>{}, noDiagnostics).HasValue());
        CHECK(service.Evaluate(handle, 1, scratch, {}).HasError());
        CHECK(service.Fail(handle).HasError());
        REQUIRE(service.Cancel(handle).HasValue());
        REQUIRE(service.Release(handle).HasValue());
    }

    TEST_CASE("Runtime service enforces restore before release", "[unit][cinematic][playback][restore]") {
        auto service = Service();
        const std::array entries{SequenceRestoreEntry{TrackId{1, 1}, RestoreTarget(901), 1, 4.0F}};
        auto snapshot = SequenceRestoreSnapshot::Create(entries);
        REQUIRE(snapshot.HasValue());
        auto activation = SequencePlaybackActivation{{Handle(8), 10, 0, {1, 1}},
                                                     Plan(),
                                                     {{SequenceBlendMode::Cut, 0},
                                                      {SequenceBlendMode::Cut, 0},
                                                      SequenceRestorePolicy::RestorePrePlayback},
                                                     std::nullopt,
                                                     std::move(snapshot).Value(),
                                                     0};
        auto handleResult = service.Activate(std::move(activation));
        REQUIRE(handleResult.HasValue());
        const auto handle = handleResult.Value();
        REQUIRE(service.Stop(handle).HasValue());
        REQUIRE(service.FinishStop(handle).HasValue());
        RequireError(service.Release(handle), SequencePlaybackRuntimeErrors::RestoreRequired);
        RestoreProbe probe{};
        const std::array targets{SequenceRestoreTargetSnapshot{RestoreTarget(901), 1, &probe, ApplyRestore}};
        std::array<SequenceRestoreDiagnostic, 1> diagnostics{};
        REQUIRE(service.Restore(handle, targets, diagnostics).HasValue());
        CHECK(probe.value == 4.0F);
        REQUIRE(service.Release(handle).HasValue());
    }

    TEST_CASE("Runtime activation rejects malformed policies", "[unit][cinematic][playback][activation]") {
        RequireError(CinematicRuntimeService::Create({{}, SequenceCookTier::Standard}), SequencePlaybackRuntimeErrors::SessionInvalid);
        auto service = Service();
        RequireError(service.Activate({{}, Plan()}), SequencePlaybackRuntimeErrors::SessionInvalid);
        auto invalidBlend = SequencePlaybackActivation{{Handle(60), 10, 0, {1, 1}}, Plan()};
        invalidBlend.blend.blendIn = {SequenceBlendMode::Blend, 0};
        RequireError(service.Activate(std::move(invalidBlend)), SequencePlaybackRuntimeErrors::ActivationInvalid);
        auto invalidCoordination = SequencePlaybackActivation{{Handle(61), 10, 0, {1, 1}}, Plan()};
        invalidCoordination.coordination = {SequenceClockSource::CommittedSimulation, SequencePausePolicy::PlayerOnly,
                                            SequenceDilationPolicy::SourceNative, false, false};
        RequireError(service.Activate(std::move(invalidCoordination)), SequencePlaybackRuntimeErrors::ActivationInvalid);
        auto missingHooks = SequencePlaybackActivation{{Handle(62), 10, 0, {1, 1}}, Plan()};
        missingHooks.coordination = {SequenceClockSource::UnscaledFixedControl, SequencePausePolicy::PlayerOnly,
                                     SequenceDilationPolicy::SourceNative, true, false};
        RequireError(service.Activate(std::move(missingHooks)), SequencePlaybackRuntimeErrors::ActivationInvalid);
        auto missingRestore = SequencePlaybackActivation{{Handle(63), 10, 0, {1, 1}}, Plan()};
        missingRestore.blend.restorePolicy = SequenceRestorePolicy::RestorePrePlayback;
        RequireError(service.Activate(std::move(missingRestore)), SequencePlaybackRuntimeErrors::RestoreInvalid);
    }

    TEST_CASE("Runtime activation enforces capacity and authority ownership", "[unit][cinematic][playback][activation]") {
        auto service = Service();
        auto mismatchedDuration = SequencePlaybackActivation{{Handle(64), 9, 0, {1, 1}}, Plan()};
        RequireError(service.Activate(std::move(mismatchedDuration)), SequencePlaybackRuntimeErrors::ActivationInvalid);
        auto overRetainedBudget = SequencePlaybackActivation{{Handle(65), 10, 0, {1, 1}}, Plan()};
        overRetainedBudget.retainedBytes = 8'388'609;
        RequireError(service.Activate(std::move(overRetainedBudget)), SequencePlaybackRuntimeErrors::CapacityExceeded);
        const std::array wrongOwnerClaim{SequenceAuthorityClaim{Handle(67), AuthorityTarget(700), CinematicControlChannel::GameplayAction,
                                                                CinematicClaimMode::Exclusive, 1, 1, 1, true}};
        auto wrongOwnerAuthority = SequenceAuthorityPlan::Create(1, 1, wrongOwnerClaim);
        REQUIRE(wrongOwnerAuthority.HasValue());
        auto wrongOwner = SequencePlaybackActivation{{Handle(66), 10, 0, {1, 1}}, Plan()};
        wrongOwner.authority = std::move(wrongOwnerAuthority).Value();
        RequireError(service.Activate(std::move(wrongOwner)), SequencePlaybackRuntimeErrors::ActivationInvalid);
        const std::array pauseClaim{SequenceAuthorityClaim{Handle(68), AuthorityTarget(701), CinematicControlChannel::GameplayAction,
                                                           CinematicClaimMode::Exclusive, 1, 1, 1, true}};
        auto pauseAuthority = SequenceAuthorityPlan::Create(1, 1, pauseClaim);
        REQUIRE(pauseAuthority.HasValue());
        auto pauseActivation = SequencePlaybackActivation{{Handle(68), 10, 0, {1, 1}}, Plan()};
        pauseActivation.coordination = {SequenceClockSource::UnscaledFixedControl, SequencePausePolicy::PlayerOnly,
                                        SequenceDilationPolicy::SourceNative, true, false};
        pauseActivation.coordinationHooks = {nullptr, AcquireCoordination, ReleaseCoordination};
        pauseActivation.authority = std::move(pauseAuthority).Value();
        RequireError(service.Activate(std::move(pauseActivation)), SequencePlaybackRuntimeErrors::ActivationInvalid);
    }

    TEST_CASE("Runtime coordination acquisition rolls back failed leases", "[unit][cinematic][playback][coordination]") {
        CoordinationProbe rollbackProbe;
        auto service = Service();
        auto rollback = SequencePlaybackActivation{{Handle(70), 10, 0, {1, 1}}, Plan()};
        rollback.coordination = {SequenceClockSource::UnscaledFixedControl, SequencePausePolicy::PlayerOnly,
                                 SequenceDilationPolicy::SourceNative, true, true};
        rollback.coordinationHooks = {&rollbackProbe, RejectHudCoordination, ReleaseCoordination};
        RequireError(service.Activate(std::move(rollback)), SequencePlaybackRuntimeErrors::CapacityExceeded);
        CHECK(rollbackProbe.acquired == 1);
        CHECK(rollbackProbe.released == 1);
        auto invalidLease = SequencePlaybackActivation{{Handle(71), 10, 0, {1, 1}}, Plan()};
        invalidLease.coordination = {SequenceClockSource::UnscaledFixedControl, SequencePausePolicy::PlayerOnly,
                                     SequenceDilationPolicy::SourceNative, true, false};
        invalidLease.coordinationHooks = {&rollbackProbe, InvalidCoordination, ReleaseCoordination};
        RequireError(service.Activate(std::move(invalidLease)), SequencePlaybackRuntimeErrors::ActivationInvalid);
        CHECK(rollbackProbe.released == 2);
        auto failedFirstLease = SequencePlaybackActivation{{Handle(72), 10, 0, {1, 1}}, Plan()};
        failedFirstLease.coordination = {SequenceClockSource::UnscaledFixedControl, SequencePausePolicy::PlayerOnly,
                                         SequenceDilationPolicy::SourceNative, true, false};
        failedFirstLease.coordinationHooks = {nullptr, RejectCoordination, ReleaseCoordination};
        RequireError(service.Activate(std::move(failedFirstLease)), SequencePlaybackRuntimeErrors::CapacityExceeded);
    }

    TEST_CASE("Runtime coordination move assignment releases old leases", "[unit][cinematic][playback][coordination]") {
        CoordinationProbe destinationProbe;
        CoordinationProbe sourceProbe;
        {
            auto destination = Service();
            auto destinationActivation = SequencePlaybackActivation{{Handle(73), 10, 0, {1, 1}}, Plan()};
            destinationActivation.coordination = {SequenceClockSource::UnscaledFixedControl, SequencePausePolicy::PlayerOnly,
                                                  SequenceDilationPolicy::SourceNative, true, true};
            destinationActivation.coordinationHooks = {&destinationProbe, AcquireCoordination, ReleaseCoordination};
            REQUIRE(destination.Activate(std::move(destinationActivation)).HasValue());
            auto source = Service();
            auto sourceActivation = SequencePlaybackActivation{{Handle(74), 10, 0, {1, 1}}, Plan()};
            sourceActivation.coordination = {SequenceClockSource::UnscaledFixedControl, SequencePausePolicy::PlayerOnly,
                                             SequenceDilationPolicy::SourceNative, true, true};
            sourceActivation.coordinationHooks = {&sourceProbe, AcquireCoordination, ReleaseCoordination};
            REQUIRE(source.Activate(std::move(sourceActivation)).HasValue());
            destination = std::move(source);
            CHECK(destinationProbe.released == 2);
            CHECK(destination.ActivePlayerCount() == 1);
        }
        CHECK(sourceProbe.released == 2);
    }

    TEST_CASE("Runtime shutdown closes active players and admission once", "[unit][cinematic][playback][lifecycle]") {
        auto service = Service();
        const auto handleResult = service.Activate({{Handle(80), 10, 0, {1, 1}}, Plan()});
        REQUIRE(handleResult.HasValue());
        const auto handle = handleResult.Value();
        REQUIRE(service.BeginShutdown().HasValue());
        CHECK(!service.ServiceSnapshot().admissionOpen);
        RequireError(service.Activate({{Handle(81), 10, 0, {1, 1}}, Plan()}), SequencePlaybackRuntimeErrors::AdmissionClosed);
        REQUIRE(service.BeginShutdown().HasValue());
        REQUIRE(service.Release(handle).HasValue());
    }
}  // namespace Horo::Cinematic
