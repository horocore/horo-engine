#pragma once

/**
 * @file AudioFocusPolicy.h
 * @brief Control-owner audio focus and suspension policy, independent of native backends.
 */

#include "Horo/Audio/AudioDeviceTiming.h"
#include "Horo/Foundation/Result.h"

#include <cstdint>
#include <optional>

namespace Horo::Audio {
    /** @brief Host composition using the same Audio control contract. */
    enum class AudioFocusHostMode : std::uint8_t {
        EditorPreview,
        PlayInEditor,
        PackagedGame,
    };

    /** @brief Complete callback-side output behavior selected by Audio control. */
    enum class AudioFocusBehavior : std::uint8_t {
        Continue,
        PauseAllBuses,
        PauseGameplayKeepMusic,
        MuteOutput,
    };

    /** @brief Cause selected by priority from a complete host fact snapshot. */
    enum class AudioFocusCause : std::uint8_t {
        None,
        FocusLost,
        Minimized,
        HostSuspended,
        DeviceInterrupted,
    };

    /** @brief Explicit behavior for each distinct condition; interruption must pause or mute all output. */
    struct AudioFocusProfile final {
        AudioFocusBehavior onFocusLost{AudioFocusBehavior::Continue};
        AudioFocusBehavior onMinimized{AudioFocusBehavior::MuteOutput};
        AudioFocusBehavior onHostSuspended{AudioFocusBehavior::PauseAllBuses};
        AudioFocusBehavior onDeviceInterrupted{AudioFocusBehavior::PauseAllBuses};
    };

    /** @brief Complete owner-thread facts; focus loss is not host suspension. */
    struct AudioFocusFacts final {
        bool focused{true};
        bool minimized{};
        bool hostSuspended{};
        bool deviceInterrupted{};

        constexpr bool operator==(const AudioFocusFacts &) const noexcept = default;
    };

    /** @brief One ordered callback policy request prepared by Audio control. */
    struct AudioFocusTransition final {
        std::uint64_t revision{};          /**< Exact non-wrapping control revision. */
        AudioFocusBehavior behavior{};     /**< Complete output policy to apply. */
        AudioFocusCause cause{};           /**< Highest-priority active cause. */
        bool closeOrdinaryAdmission{};     /**< Suspend/interruption closes new ordinary start work immediately. */
        bool requiresClockDiscontinuity{}; /**< Resume must establish a fresh clock correlation first. */
    };

    /** @brief Control-owned proof of callback policy application and device readiness. */
    struct AudioFocusAcknowledgement final {
        std::uint64_t revision{};
        AudioDeviceEpoch deviceEpoch;
        std::uint64_t clockDiscontinuityRevision{};
        bool callbackReady{}; /**< Matching device/callback epoch has completed readiness or recovery. */
    };

    /** @brief Immutable control-owner status; pending work is not yet a committed callback policy. */
    struct AudioFocusSnapshot final {
        AudioFocusHostMode mode{AudioFocusHostMode::EditorPreview};
        AudioFocusBehavior behavior{AudioFocusBehavior::Continue};
        AudioFocusCause cause{AudioFocusCause::None};
        AudioFocusFacts facts;
        std::uint64_t revision{};
        std::uint64_t clockDiscontinuityRevision{};
        bool ordinaryAdmissionClosed{};
        std::optional<AudioFocusTransition> pending;
    };

    /**
     * @brief Returns the documented desktop defaults for one host composition.
     * @param mode Editor preview, play-in-editor, or packaged game.
     * @return Explicit default profile, or an identity error for an unknown mode.
     */
    [[nodiscard]] Result<AudioFocusProfile> DefaultAudioFocusProfile(AudioFocusHostMode mode);

    /**
     * @brief Serial control-owner policy and acknowledgement gate; performs no device or callback calls.
     * @details The host resolves mode/profile and marshals facts to the Audio control owner. Control stages each
     * returned transition in FIFO order through reserved lifecycle capacity, then supplies exact callback/device
     * acknowledgement. No focus or interruption edge directly pauses a native device. A pending request blocks
     * competing policy publication; resume requires current callback readiness and a new clock discontinuity.
     */
    class AudioFocusController final {
    public:
        /**
         * @brief Validates policy and initial callback identity without selecting a backend.
         * @param mode Host composition kind.
         * @param profile Explicit per-condition behavior.
         * @param deviceEpoch Currently admitted callback generation.
         * @param clockDiscontinuityRevision Non-zero current control clock revision.
         * @return Prepared control owner or a typed identity error.
         */
        [[nodiscard]] static Result<AudioFocusController> Create(AudioFocusHostMode mode, AudioFocusProfile profile,
                                                                 AudioDeviceEpoch deviceEpoch, std::uint64_t clockDiscontinuityRevision);
        AudioFocusController(const AudioFocusController &) = delete;
        AudioFocusController &operator=(const AudioFocusController &) = delete;
        AudioFocusController(AudioFocusController &&) noexcept = default;
        AudioFocusController &operator=(AudioFocusController &&) noexcept = default;

        /**
         * @brief Prepares one ordered policy change from a complete host fact snapshot.
         * @param facts Focus, minimization, host suspension, and native interruption facts.
         * @return A transition, no transition when behavior/admission is unchanged, or a pending/revision error.
         * @note A suspend/interruption fact closes ordinary admission immediately, even if an earlier transition
         * remains pending and this call returns a pending error. The host retries the latest facts after that
         * transition resolves; a transient critical fact requires a new clock-correlated acknowledgement to clear.
         */
        [[nodiscard]] Result<std::optional<AudioFocusTransition>> Prepare(AudioFocusFacts facts);

        /**
         * @brief Commits only exact callback application after device/clock validation.
         * @param acknowledgement Exact transition, current callback epoch, clock revision, and readiness proof.
         * @return Success or a stale/not-ready error without committing a partial policy.
         */
        [[nodiscard]] Result<void> Commit(const AudioFocusAcknowledgement &acknowledgement);

        /**
         * @brief Adopts an explicitly recovered callback epoch while interruption admission remains closed.
         * @param nextEpoch New generation from the already validated Audio device lifecycle.
         * @param nextClockDiscontinuityRevision New correlation after device recovery.
         * @return Success or stale/invalid-transition error; never resumes audio by itself.
         */
        [[nodiscard]] Result<void> AdoptRecoveredDevice(const AudioDeviceEpoch &nextEpoch, std::uint64_t nextClockDiscontinuityRevision);

        /** @brief Returns committed policy and pending work. @return Owned snapshot. */
        [[nodiscard]] AudioFocusSnapshot Snapshot() const noexcept;

    private:
        AudioFocusController(AudioFocusHostMode mode, AudioFocusProfile profile, AudioDeviceEpoch deviceEpoch,
                             std::uint64_t clockDiscontinuityRevision) noexcept;

        AudioFocusHostMode mode_;
        AudioFocusProfile profile_;
        AudioDeviceEpoch deviceEpoch_;
        AudioFocusFacts facts_;
        AudioFocusFacts pendingFacts_;
        AudioFocusBehavior behavior_{AudioFocusBehavior::Continue};
        AudioFocusCause cause_{AudioFocusCause::None};
        std::optional<AudioFocusTransition> pending_;
        std::uint64_t revision_{};
        std::uint64_t clockDiscontinuityRevision_{};
        bool ordinaryAdmissionClosed_{};
        bool criticalHold_{};
        bool pendingCoversCriticalHold_{};
        bool deviceRecoveryRequired_{};
    };
}  // namespace Horo::Audio
