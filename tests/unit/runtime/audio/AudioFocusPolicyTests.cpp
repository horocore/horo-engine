#include "Horo/Audio/AudioErrors.h"
#include "Horo/Audio/AudioFocusPolicy.h"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <initializer_list>
#include <utility>

namespace Horo::Audio {
    namespace {
        [[nodiscard]] AudioDeviceEpoch Epoch(const std::uint64_t callbackEpoch = 3) {
            return {.device = {AudioRuntimeId::Create(41).Value(), 2, 1}, .formatRevision = 5, .callbackEpoch = callbackEpoch};
        }

        [[nodiscard]] AudioFocusController Controller(const AudioFocusHostMode mode = AudioFocusHostMode::PlayInEditor) {
            auto profile = DefaultAudioFocusProfile(mode);
            REQUIRE(profile.HasValue());
            auto created = AudioFocusController::Create(mode, profile.Value(), Epoch(), 1);
            REQUIRE(created.HasValue());
            return std::move(created).Value();
        }

        template <typename T> void ExpectError(const Result<T> &result, const ErrorCodeDescriptor &error) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == error.code.Value());
        }

        [[nodiscard]] AudioFocusTransition RequireTransition(AudioFocusController &controller, const AudioFocusFacts facts) {
            auto prepared = controller.Prepare(facts);
            REQUIRE(prepared.HasValue());
            REQUIRE(prepared.Value().has_value());
            return *prepared.Value();
        }

        [[nodiscard]] AudioFocusAcknowledgement Ack(const AudioFocusTransition &transition, const std::uint64_t clockRevision,
                                                    const AudioDeviceEpoch epoch = Epoch(), const bool callbackReady = true) {
            return {.revision = transition.revision,
                    .deviceEpoch = epoch,
                    .clockDiscontinuityRevision = clockRevision,
                    .callbackReady = callbackReady};
        }

        TEST_CASE("Audio focus profiles distinguish preview, PIE and packaged hosts", "[unit][audio][focus]") {
            const auto preview = DefaultAudioFocusProfile(AudioFocusHostMode::EditorPreview);
            const auto pie = DefaultAudioFocusProfile(AudioFocusHostMode::PlayInEditor);
            const auto game = DefaultAudioFocusProfile(AudioFocusHostMode::PackagedGame);
            REQUIRE(preview.HasValue());
            REQUIRE(pie.HasValue());
            REQUIRE(game.HasValue());
            CHECK(preview.Value().onFocusLost == AudioFocusBehavior::Continue);
            CHECK(pie.Value().onFocusLost == AudioFocusBehavior::PauseGameplayKeepMusic);
            CHECK(game.Value().onFocusLost == AudioFocusBehavior::Continue);
            CHECK(preview.Value().onMinimized == AudioFocusBehavior::MuteOutput);
            CHECK(pie.Value().onHostSuspended == AudioFocusBehavior::PauseAllBuses);
            CHECK(game.Value().onDeviceInterrupted == AudioFocusBehavior::PauseAllBuses);
            ExpectError(DefaultAudioFocusProfile(static_cast<AudioFocusHostMode>(99)), AudioErrors::IdentityInvalid);
        }

        TEST_CASE("Audio preview and packaged focus can continue while minimization mutes", "[unit][audio][focus]") {
            for (const auto mode : {AudioFocusHostMode::EditorPreview, AudioFocusHostMode::PackagedGame}) {
                auto controller = Controller(mode);
                const auto lost = controller.Prepare({.focused = false});
                REQUIRE(lost.HasValue());
                CHECK_FALSE(lost.Value().has_value());
                CHECK(controller.Snapshot().cause == AudioFocusCause::FocusLost);
                CHECK(controller.Snapshot().behavior == AudioFocusBehavior::Continue);
                const auto minimized = RequireTransition(controller, {.focused = false, .minimized = true});
                CHECK(minimized.behavior == AudioFocusBehavior::MuteOutput);
                REQUIRE(controller.Commit(Ack(minimized, 1)).HasValue());
            }
        }

        TEST_CASE("Audio host may choose an explicit focus override without platform types", "[unit][audio][focus]") {
            auto profile = DefaultAudioFocusProfile(AudioFocusHostMode::PackagedGame).Value();
            profile.onFocusLost = AudioFocusBehavior::PauseAllBuses;
            auto created = AudioFocusController::Create(AudioFocusHostMode::PackagedGame, profile, Epoch(), 1);
            REQUIRE(created.HasValue());
            auto controller = std::move(created).Value();
            const auto lost = RequireTransition(controller, {.focused = false});
            CHECK(lost.behavior == AudioFocusBehavior::PauseAllBuses);
            CHECK_FALSE(lost.closeOrdinaryAdmission);
        }

        TEST_CASE("Audio focus priorities and policy overrides are typed", "[unit][audio][focus]") {
            auto controller = Controller();
            const auto lost = RequireTransition(controller, {.focused = false});
            CHECK(lost.cause == AudioFocusCause::FocusLost);
            CHECK(lost.behavior == AudioFocusBehavior::PauseGameplayKeepMusic);
            CHECK_FALSE(lost.closeOrdinaryAdmission);
            REQUIRE(controller.Commit(Ack(lost, 1)).HasValue());

            const auto minimized = RequireTransition(controller, {.focused = false, .minimized = true});
            CHECK(minimized.cause == AudioFocusCause::Minimized);
            CHECK(minimized.behavior == AudioFocusBehavior::MuteOutput);
            REQUIRE(controller.Commit(Ack(minimized, 2)).HasValue());

            const auto suspended = RequireTransition(controller, {.focused = false, .minimized = true, .hostSuspended = true});
            CHECK(suspended.cause == AudioFocusCause::HostSuspended);
            CHECK(suspended.behavior == AudioFocusBehavior::PauseAllBuses);
            CHECK(suspended.closeOrdinaryAdmission);
            CHECK(controller.Snapshot().ordinaryAdmissionClosed);
            REQUIRE(controller.Commit(Ack(suspended, 3)).HasValue());

            const auto interrupted =
                controller.Prepare({.focused = false, .minimized = true, .hostSuspended = true, .deviceInterrupted = true});
            REQUIRE(interrupted.HasValue());
            CHECK_FALSE(interrupted.Value().has_value());
            CHECK(controller.Snapshot().cause == AudioFocusCause::DeviceInterrupted);
            CHECK(controller.Snapshot().ordinaryAdmissionClosed);
        }

        TEST_CASE("Audio resume waits for exact callback readiness and fresh clock correlation", "[unit][audio][focus]") {
            auto controller = Controller();
            const auto lost = RequireTransition(controller, {.focused = false});
            REQUIRE(controller.Commit(Ack(lost, 1)).HasValue());

            const auto resumed = RequireTransition(controller, {});
            CHECK(resumed.behavior == AudioFocusBehavior::Continue);
            CHECK(resumed.requiresClockDiscontinuity);
            ExpectError(controller.Commit(Ack(resumed, 1)), AudioErrors::RuntimeInactive);
            ExpectError(controller.Commit(Ack(resumed, 2, Epoch(), false)), AudioErrors::RuntimeInactive);
            ExpectError(controller.Commit(Ack(resumed, 2, Epoch(99))), AudioErrors::HandleStale);
            CHECK(controller.Snapshot().behavior == AudioFocusBehavior::PauseGameplayKeepMusic);
            REQUIRE(controller.Commit(Ack(resumed, 2)).HasValue());
            CHECK(controller.Snapshot().behavior == AudioFocusBehavior::Continue);
            CHECK(controller.Snapshot().clockDiscontinuityRevision == 2);
            ExpectError(controller.Commit(Ack(resumed, 2)), AudioErrors::HandleStale);
        }

        TEST_CASE("Audio host suspension closes admission before callback acknowledgement", "[unit][audio][focus]") {
            auto controller = Controller(AudioFocusHostMode::PackagedGame);
            const auto suspended = RequireTransition(controller, {.hostSuspended = true});
            CHECK(controller.Snapshot().ordinaryAdmissionClosed);
            CHECK(controller.Snapshot().behavior == AudioFocusBehavior::Continue);
            ExpectError(controller.Prepare({}), AudioErrors::RuntimeInactive);
            REQUIRE(controller.Commit(Ack(suspended, 1)).HasValue());
            CHECK(controller.Snapshot().behavior == AudioFocusBehavior::PauseAllBuses);

            const auto resumed = RequireTransition(controller, {});
            CHECK(resumed.requiresClockDiscontinuity);
            CHECK(controller.Snapshot().ordinaryAdmissionClosed);
            REQUIRE(controller.Commit(Ack(resumed, 2)).HasValue());
            CHECK_FALSE(controller.Snapshot().ordinaryAdmissionClosed);
            CHECK(controller.Snapshot().behavior == AudioFocusBehavior::Continue);
        }

        TEST_CASE("Audio critical facts arriving during pending work retain an admission hold", "[unit][audio][focus]") {
            auto controller = Controller();
            const auto focus = RequireTransition(controller, {.focused = false});
            ExpectError(controller.Prepare({.focused = false, .hostSuspended = true}), AudioErrors::RuntimeInactive);
            CHECK(controller.Snapshot().ordinaryAdmissionClosed);
            REQUIRE(controller.Commit(Ack(focus, 1)).HasValue());
            CHECK(controller.Snapshot().ordinaryAdmissionClosed);
            const auto suspended = RequireTransition(controller, {.focused = false, .hostSuspended = true});
            REQUIRE(controller.Commit(Ack(suspended, 2)).HasValue());
            CHECK(controller.Snapshot().ordinaryAdmissionClosed);
            const auto resumed = RequireTransition(controller, {.focused = false});
            ExpectError(controller.Commit(Ack(resumed, 2)), AudioErrors::RuntimeInactive);
            REQUIRE(controller.Commit(Ack(resumed, 3)).HasValue());
            CHECK_FALSE(controller.Snapshot().ordinaryAdmissionClosed);
        }

        TEST_CASE("Audio critical fact during pending resume cannot reopen admission", "[unit][audio][focus]") {
            auto controller = Controller();
            const auto focus = RequireTransition(controller, {.focused = false});
            ExpectError(controller.Prepare({.focused = false, .hostSuspended = true}), AudioErrors::RuntimeInactive);
            REQUIRE(controller.Commit(Ack(focus, 1)).HasValue());

            const auto resume = RequireTransition(controller, {.focused = false});
            CHECK(controller.Snapshot().ordinaryAdmissionClosed);
            ExpectError(controller.Prepare({.focused = false, .hostSuspended = true}), AudioErrors::RuntimeInactive);
            REQUIRE(controller.Commit(Ack(resume, 2)).HasValue());
            CHECK(controller.Snapshot().ordinaryAdmissionClosed);

            const auto suspended = RequireTransition(controller, {.focused = false, .hostSuspended = true});
            CHECK(suspended.closeOrdinaryAdmission);
            REQUIRE(controller.Commit(Ack(suspended, 3)).HasValue());
            CHECK(controller.Snapshot().ordinaryAdmissionClosed);
        }

        TEST_CASE("Audio interruption cannot resume merely because the native edge ended", "[unit][audio][focus]") {
            auto controller = Controller();
            const auto interrupted = RequireTransition(controller, {.deviceInterrupted = true});
            REQUIRE(controller.Commit(Ack(interrupted, 1)).HasValue());
            CHECK(controller.Snapshot().ordinaryAdmissionClosed);
            ExpectError(controller.Prepare({}), AudioErrors::RuntimeInactive);
            CHECK(controller.Snapshot().ordinaryAdmissionClosed);
            ExpectError(controller.AdoptRecoveredDevice(Epoch(), 2), AudioErrors::HandleStale);
            ExpectError(controller.AdoptRecoveredDevice(Epoch(4), 1), AudioErrors::HandleStale);
            REQUIRE(controller.AdoptRecoveredDevice(Epoch(4), 2).HasValue());
            CHECK(controller.Snapshot().behavior == AudioFocusBehavior::PauseAllBuses);

            const auto resumed = RequireTransition(controller, {});
            ExpectError(controller.Commit(Ack(resumed, 2, Epoch(4))), AudioErrors::RuntimeInactive);
            ExpectError(controller.Commit(Ack(resumed, 3)), AudioErrors::HandleStale);
            REQUIRE(controller.Commit(Ack(resumed, 3, Epoch(4))).HasValue());
            CHECK_FALSE(controller.Snapshot().ordinaryAdmissionClosed);
        }

        TEST_CASE("Audio focus rejects malformed profiles and preserves pending policy", "[unit][audio][focus]") {
            auto invalid = DefaultAudioFocusProfile(AudioFocusHostMode::EditorPreview).Value();
            invalid.onMinimized = static_cast<AudioFocusBehavior>(99);
            ExpectError(AudioFocusController::Create(AudioFocusHostMode::EditorPreview, invalid, Epoch(), 1), AudioErrors::IdentityInvalid);
            invalid = DefaultAudioFocusProfile(AudioFocusHostMode::EditorPreview).Value();
            invalid.onDeviceInterrupted = AudioFocusBehavior::Continue;
            ExpectError(AudioFocusController::Create(AudioFocusHostMode::EditorPreview, invalid, Epoch(), 1), AudioErrors::IdentityInvalid);
            ExpectError(AudioFocusController::Create(AudioFocusHostMode::EditorPreview, {}, Epoch(), 0), AudioErrors::IdentityInvalid);
            auto controller = Controller();
            const auto pending = RequireTransition(controller, {.hostSuspended = true});
            ExpectError(controller.Commit(Ack({.revision = pending.revision + 1}, 1)), AudioErrors::HandleStale);
            CHECK(controller.Snapshot().pending.has_value());
            CHECK(controller.Snapshot().ordinaryAdmissionClosed);
            REQUIRE(controller.Commit(Ack(pending, 1)).HasValue());
        }
    }  // namespace
}  // namespace Horo::Audio
