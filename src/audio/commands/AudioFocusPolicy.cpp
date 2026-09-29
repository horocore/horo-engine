#include "Horo/Audio/AudioFocusPolicy.h"

#include "Horo/Audio/AudioErrors.h"

#include <limits>
#include <utility>

namespace Horo::Audio {
    namespace {
        [[nodiscard]] bool Known(const AudioFocusHostMode mode) noexcept {
            return mode >= AudioFocusHostMode::EditorPreview && mode <= AudioFocusHostMode::PackagedGame;
        }

        [[nodiscard]] bool Known(const AudioFocusBehavior behavior) noexcept {
            return behavior >= AudioFocusBehavior::Continue && behavior <= AudioFocusBehavior::MuteOutput;
        }

        [[nodiscard]] bool Valid(const AudioFocusProfile &profile) noexcept {
            return Known(profile.onFocusLost) && Known(profile.onMinimized) && Known(profile.onHostSuspended) &&
                   (profile.onDeviceInterrupted == AudioFocusBehavior::PauseAllBuses ||
                    profile.onDeviceInterrupted == AudioFocusBehavior::MuteOutput);
        }

        [[nodiscard]] AudioFocusCause Cause(const AudioFocusFacts &facts) noexcept {
            if (facts.deviceInterrupted)
                return AudioFocusCause::DeviceInterrupted;
            if (facts.hostSuspended)
                return AudioFocusCause::HostSuspended;
            if (facts.minimized)
                return AudioFocusCause::Minimized;
            return facts.focused ? AudioFocusCause::None : AudioFocusCause::FocusLost;
        }

        [[nodiscard]] AudioFocusBehavior Behavior(const AudioFocusProfile &profile, const AudioFocusCause cause) noexcept {
            switch (cause) {
                case AudioFocusCause::None:
                    return AudioFocusBehavior::Continue;
                case AudioFocusCause::FocusLost:
                    return profile.onFocusLost;
                case AudioFocusCause::Minimized:
                    return profile.onMinimized;
                case AudioFocusCause::HostSuspended:
                    return profile.onHostSuspended;
                case AudioFocusCause::DeviceInterrupted:
                    return profile.onDeviceInterrupted;
            }
            return AudioFocusBehavior::Continue;
        }
    }  // namespace

    /** @copydoc DefaultAudioFocusProfile */
    Result<AudioFocusProfile> DefaultAudioFocusProfile(const AudioFocusHostMode mode) {
        if (!Known(mode))
            return Result<AudioFocusProfile>::Failure(MakeError(AudioErrors::IdentityInvalid));
        AudioFocusProfile profile;
        if (mode == AudioFocusHostMode::PlayInEditor)
            profile.onFocusLost = AudioFocusBehavior::PauseGameplayKeepMusic;
        return Result<AudioFocusProfile>::Success(profile);
    }

    /** @copydoc AudioFocusController::Create */
    Result<AudioFocusController> AudioFocusController::Create(const AudioFocusHostMode mode, AudioFocusProfile profile,
                                                              AudioDeviceEpoch deviceEpoch,
                                                              const std::uint64_t clockDiscontinuityRevision) {
        if (!Known(mode) || !Valid(profile) || !MatchesAudioDeviceEpoch(deviceEpoch, deviceEpoch) || clockDiscontinuityRevision == 0)
            return Result<AudioFocusController>::Failure(MakeError(AudioErrors::IdentityInvalid));
        return Result<AudioFocusController>::Success(
            AudioFocusController{mode, std::move(profile), std::move(deviceEpoch), clockDiscontinuityRevision});
    }

    AudioFocusController::AudioFocusController(const AudioFocusHostMode mode, AudioFocusProfile profile, AudioDeviceEpoch deviceEpoch,
                                               const std::uint64_t clockDiscontinuityRevision) noexcept
        : mode_(mode), profile_(std::move(profile)), deviceEpoch_(std::move(deviceEpoch)), revision_(1),
          clockDiscontinuityRevision_(clockDiscontinuityRevision) {}

    /** @copydoc AudioFocusController::Prepare */
    Result<std::optional<AudioFocusTransition>> AudioFocusController::Prepare(const AudioFocusFacts facts) {
        if (facts.hostSuspended || facts.deviceInterrupted)
            criticalHold_ = true;
        if (facts.deviceInterrupted && !facts_.deviceInterrupted)
            deviceRecoveryRequired_ = true;
        if (pending_.has_value())
            return Result<std::optional<AudioFocusTransition>>::Failure(MakeError(AudioErrors::RuntimeInactive));
        if (!facts.deviceInterrupted && deviceRecoveryRequired_)
            return Result<std::optional<AudioFocusTransition>>::Failure(MakeError(AudioErrors::RuntimeInactive));
        const auto cause = Cause(facts);
        const auto behavior = Behavior(profile_, cause);
        const bool closeAdmission = facts.hostSuspended || facts.deviceInterrupted;
        if (behavior == behavior_ && closeAdmission == ordinaryAdmissionClosed_ && (!criticalHold_ || closeAdmission)) {
            facts_ = facts;
            cause_ = cause;
            if (closeAdmission)
                criticalHold_ = false;
            return Result<std::optional<AudioFocusTransition>>::Success(std::nullopt);
        }
        if (revision_ == std::numeric_limits<std::uint64_t>::max())
            return Result<std::optional<AudioFocusTransition>>::Failure(MakeError(AudioErrors::HandleGenerationExhausted));
        pendingFacts_ = facts;
        pendingCoversCriticalHold_ = criticalHold_;
        pending_ = AudioFocusTransition{.revision = revision_ + 1,
                                        .behavior = behavior,
                                        .cause = cause,
                                        .closeOrdinaryAdmission = closeAdmission,
                                        .requiresClockDiscontinuity =
                                            (behavior_ != AudioFocusBehavior::Continue && behavior != behavior_) ||
                                            (facts_.hostSuspended && !facts.hostSuspended) ||
                                            (facts_.deviceInterrupted && !facts.deviceInterrupted) || (criticalHold_ && !closeAdmission)};
        return Result<std::optional<AudioFocusTransition>>::Success(pending_);
    }

    /** @copydoc AudioFocusController::Commit */
    Result<void> AudioFocusController::Commit(const AudioFocusAcknowledgement &acknowledgement) {
        if (!pending_.has_value() || acknowledgement.revision != pending_->revision ||
            !MatchesAudioDeviceEpoch(deviceEpoch_, acknowledgement.deviceEpoch))
            return Result<void>::Failure(MakeError(AudioErrors::HandleStale));
        if (!acknowledgement.callbackReady ||
            (pending_->requiresClockDiscontinuity ? acknowledgement.clockDiscontinuityRevision <= clockDiscontinuityRevision_
                                                  : acknowledgement.clockDiscontinuityRevision < clockDiscontinuityRevision_))
            return Result<void>::Failure(MakeError(AudioErrors::RuntimeInactive));
        facts_ = pendingFacts_;
        behavior_ = pending_->behavior;
        cause_ = pending_->cause;
        ordinaryAdmissionClosed_ = pending_->closeOrdinaryAdmission;
        if (pendingCoversCriticalHold_ || ordinaryAdmissionClosed_)
            criticalHold_ = false;
        pendingCoversCriticalHold_ = false;
        revision_ = pending_->revision;
        clockDiscontinuityRevision_ = acknowledgement.clockDiscontinuityRevision;
        pending_.reset();
        return Result<void>::Success();
    }

    /** @copydoc AudioFocusController::AdoptRecoveredDevice */
    Result<void> AudioFocusController::AdoptRecoveredDevice(const AudioDeviceEpoch &nextEpoch,
                                                            const std::uint64_t nextClockDiscontinuityRevision) {
        if (!deviceRecoveryRequired_ || pending_.has_value())
            return Result<void>::Failure(MakeError(AudioErrors::RuntimeInactive));
        if (!MatchesAudioDeviceEpoch(nextEpoch, nextEpoch) || nextEpoch.device.owner != deviceEpoch_.device.owner ||
            nextEpoch.callbackEpoch <= deviceEpoch_.callbackEpoch || nextClockDiscontinuityRevision <= clockDiscontinuityRevision_)
            return Result<void>::Failure(MakeError(AudioErrors::HandleStale));
        deviceEpoch_ = nextEpoch;
        clockDiscontinuityRevision_ = nextClockDiscontinuityRevision;
        deviceRecoveryRequired_ = false;
        return Result<void>::Success();
    }

    /** @copydoc AudioFocusController::Snapshot */
    AudioFocusSnapshot AudioFocusController::Snapshot() const noexcept {
        return {.mode = mode_,
                .behavior = behavior_,
                .cause = cause_,
                .facts = facts_,
                .revision = revision_,
                .clockDiscontinuityRevision = clockDiscontinuityRevision_,
                .ordinaryAdmissionClosed =
                    ordinaryAdmissionClosed_ || criticalHold_ || (pending_.has_value() && pending_->closeOrdinaryAdmission),
                .pending = pending_};
    }
}  // namespace Horo::Audio
