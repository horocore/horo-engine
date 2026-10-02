#pragma once

/** @file AudioConcurrencyGroup.h
 * @brief Scoped sound-instance limits and deterministic retrigger eligibility.
 */

#include "Horo/Audio/AudioVoiceStateMachine.h"

#include <cstdint>
#include <optional>
#include <span>

namespace Horo::Audio {
    struct AudioEmitterHandleTag;
    struct AudioPlaybackOwnerHandleTag;
    /** @brief Generation-safe emitter identity assigned by the Audio control owner. */
    using AudioEmitterHandle = AudioHandle<AudioEmitterHandleTag>;
    /** @brief Generation-safe gameplay playback-owner identity assigned by the Audio control owner. */
    using AudioPlaybackOwnerHandle = AudioHandle<AudioPlaybackOwnerHandleTag>;

    /** @brief Dimension used to partition one authored concurrency group. */
    enum class AudioConcurrencyScope : std::uint8_t {
        Global,
        Emitter,
        Owner
    };

    /** @brief Authored group policy; zero maximumInstances imposes no instance ceiling. */
    struct AudioConcurrencyGroup final {
        AudioConcurrencyGroupId group;
        AudioConcurrencyScope scope{AudioConcurrencyScope::Global};
        std::uint32_t maximumInstances{};
        std::uint64_t retriggerFrames{}; /**< Minimum sample-frame distance between successful admissions. */
        bool countPaused{true};
        bool countVirtual{true};
    };

    /** @brief Request identities; only the identity required by the group's scope must be present. */
    struct AudioConcurrencyRequest final {
        AudioRuntimeId runtime;
        AudioEmitterHandle emitter;
        AudioPlaybackOwnerHandle owner;
    };

    /** @brief Canonical bucket identity; unused scope handles are always empty. */
    struct AudioConcurrencyKey final {
        AudioConcurrencyGroupId group;
        AudioRuntimeId runtime;
        AudioConcurrencyScope scope{AudioConcurrencyScope::Global};
        AudioEmitterHandle emitter;
        AudioPlaybackOwnerHandle owner;
        constexpr auto operator<=>(const AudioConcurrencyKey &) const noexcept = default;
    };

    /** @brief Bounded immutable state for exactly one scoped bucket. */
    struct AudioConcurrencySnapshot final {
        AudioConcurrencyKey key;
        std::uint64_t timelineGeneration{}; /**< Non-zero token replaced on sample-clock discontinuity or reset. */
        std::optional<std::uint64_t> lastAdmissionFrame;
        std::span<const AudioVoiceSnapshot> voices; /**< Unique, strictly handle-ordered registry snapshots for this bucket. */
    };

    /** @brief Control-owned sample time in one non-reused timeline generation. */
    struct AudioConcurrencyTime final {
        std::uint64_t timelineGeneration{};
        std::uint64_t sampleFrame{};
    };

    /** @brief Normal policy outcomes, distinct from malformed or stale input errors. */
    enum class AudioConcurrencyEligibility : std::uint8_t {
        Eligible,
        InstanceLimit,
        RetriggerWindow
    };

    /** @brief Deterministic eligibility evidence; limit wins when both restrictions apply. */
    struct AudioConcurrencyDecision final {
        AudioConcurrencyEligibility eligibility{AudioConcurrencyEligibility::Eligible};
        std::uint32_t countedInstances{};
        std::uint64_t remainingRetriggerFrames{};
        constexpr auto operator<=>(const AudioConcurrencyDecision &) const noexcept = default;
    };

    /**
     * @brief Validate and canonicalize an authored group and request into one scoped bucket key.
     * @param group Non-zero identity, known scope and bounded instance ceiling.
     * @param request Runtime and the generation-safe handle required by the selected scope.
     * @return Canonical key or ConcurrencyInvalid; foreign scope handles return HandleOwnerMismatch.
     */
    [[nodiscard]] Result<AudioConcurrencyKey> MakeAudioConcurrencyKey(const AudioConcurrencyGroup &group,
                                                                      const AudioConcurrencyRequest &request);

    /**
     * @brief Evaluate instance limits and cooldown without mutating admission state.
     * @param group Authored group policy matching snapshot.key.
     * @param request Request being evaluated against its canonical bucket.
     * @param snapshot Complete bucket projection, bounded by MaximumAudioVoiceSlots.
     * @param time Current sample time in the snapshot's timeline generation.
     * @return Policy decision or typed invalid, stale-timeline or foreign-runtime failure.
     * @note The single Audio control owner serializes evaluation and admission. It projects registry
     * states into the exact bucket and records each successful admission before evaluating the next
     * request. Successful evaluations allocate nothing. Rejected requests never advance cooldown. Created, Ready, Scheduled, Playing and
     * Stopping count; Paused/Virtual follow policy; terminal states never count. Terminal records must
     * retain their matching terminal reason. No voice is created, stolen, stopped or released here.
     * @note A stopped voice does not erase cooldown. On reset, scene teardown or shutdown the owner
     * retires scoped state; cooldown history cannot cross timeline or handle generations. Borrowed
     * snapshots must remain immutable for the call; the function retains no references.
     */
    [[nodiscard]] Result<AudioConcurrencyDecision> EvaluateAudioConcurrency(const AudioConcurrencyGroup &group,
                                                                            const AudioConcurrencyRequest &request,
                                                                            const AudioConcurrencySnapshot &snapshot,
                                                                            AudioConcurrencyTime time);
}  // namespace Horo::Audio
