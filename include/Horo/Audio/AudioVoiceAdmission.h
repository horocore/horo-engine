#pragma once

/** @file AudioVoiceAdmission.h
 * @brief Bounded deterministic physical voice admission across shared concurrency limits.
 */

#include "Horo/Audio/AudioConcurrencyGroup.h"
#include "Horo/Audio/AudioSoundReference.h"

#include <cstdint>
#include <optional>
#include <span>

namespace Horo::Audio {
    /** @brief Maximum number of simultaneous authored/source-local restrictions per request. */
    inline constexpr std::uint32_t MaximumAudioVoiceAdmissionConstraints = 8;

    /** @brief Control-owned immutable ranking and physical reservation for a canonical voice. */
    struct AudioVoiceAdmissionCandidate final {
        AudioVoiceSnapshot snapshot;
        std::uint64_t admissionOrder{}; /**< Monotonic request order; smaller values are older. */
        float audibleGain{};            /**< Finite non-negative effective gain, after attenuation. */
        AudioPriority priority{};       /**< Larger values have greater importance. */
        double listenerDistance{};      /**< Finite non-negative distance in one common coordinate frame. */
        bool physical{};                /**< Holds a physical reservation, including queued/stopping/terminal retirement. */
        bool stealable{true};           /**< False protects retained/native/event proxy ownership. */
    };

    /** @brief One complete authored or source-local bucket; its states must match the global projection. */
    struct AudioVoiceAdmissionConstraint final {
        AudioConcurrencyGroup group;
        AudioConcurrencyRequest request;
        AudioConcurrencySnapshot snapshot;
    };

    /** @brief One serialized admission request using complete bounded control-owner projections. */
    struct AudioVoiceAdmissionRequest final {
        AudioRuntimeId runtime;
        std::uint32_t maximumPhysicalVoices{}; /**< Zero permits no physical reservation. */
        AudioConcurrencyMode mode{AudioConcurrencyMode::Reject};
        AudioConcurrencyTime time;
        std::span<const AudioVoiceAdmissionCandidate> voices;       /**< Strictly handle-ordered complete runtime projection. */
        std::span<const AudioVoiceAdmissionConstraint> constraints; /**< Includes every applicable authored/source-local limit. */
    };

    /** @brief Mutually exclusive next action; replacement retires exactly the selected victim. */
    enum class AudioVoiceAdmissionAction : std::uint8_t {
        AdmitPhysical,
        Replace,
        Reject,
        AdmitVirtual
    };

    /** @brief Exactly one observable reason per successful evaluation, including ordinary rejection. */
    enum class AudioVoiceAdmissionReason : std::uint8_t {
        CapacityAvailable,
        RejectNewest,
        PhysicalCapacity,
        InstanceCapacity,
        RetriggerWindow,
        NoEligibleVictim,
        OverCapacity,
        StopOldest,
        StopQuietest,
        StopLowestPriority,
        StopFurthest,
        ReplaceOldest,
        Virtualized
    };

    /** @brief Immutable decision; only Replace carries an exact generation-safe victim. */
    struct AudioVoiceAdmissionDecision final {
        AudioVoiceAdmissionAction action{AudioVoiceAdmissionAction::Reject};
        AudioVoiceAdmissionReason reason{AudioVoiceAdmissionReason::RejectNewest};
        std::optional<AudioVoiceHandle> victim;
        constexpr auto operator<=>(const AudioVoiceAdmissionDecision &) const noexcept = default;
    };

    /**
     * @brief Choose one physical/virtual admission, replacement or rejection without mutating any owner.
     * @param request Complete registry/bucket projections serialized by the exclusive Audio control owner.
     * @return One decision or typed malformed, foreign-runtime or stale-timeline failure.
     * @note Valid evaluations allocate nothing. Work is bounded by voice slots times constraint count times log(bucket size).
     * Oldest/Replace minimize admissionOrder; Quietest minimizes gain; LowestPriority minimizes priority;
     * Furthest maximizes distance. Equal ranks minimize admissionOrder, then the complete voice handle.
     * Replacement must free every saturated bucket and, when full, a physical reservation. Terminal,
     * Stopping and protected candidates cannot be stolen. A cooldown always rejects, including when a
     * bucket is also full. An already over-budget projection rejects until the owner reconciles it.
     * Virtualize only bypasses a physical ceiling; it cannot bypass an instance ceiling or cooldown.
     * Allow never bypasses a configured ceiling. The caller commits the decision before reevaluating,
     * schedules the victim's stop, and waits for retained render work to retire before reusing storage.
     * No slot is freed, no cursor advances, and no backend or middleware is invoked by evaluation.
     * Native and event proxy reservations share this projection; Virtual admission is eligibility only,
     * with cursor/realization execution owned by the virtualization layer. Borrowed spans are not retained.
     */
    [[nodiscard]] Result<AudioVoiceAdmissionDecision> EvaluateAudioVoiceAdmission(const AudioVoiceAdmissionRequest &request);
}  // namespace Horo::Audio
