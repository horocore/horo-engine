#pragma once

/**
 * @file AudioDeviceTiming.h
 * @brief Generation-scoped callback identity and provenance-preserving device timing facts.
 */

#include "Horo/Audio/AudioDeviceDiscovery.h"

#include <cstdint>
#include <optional>

namespace Horo::Audio {
    /** @brief Requested buffering priority; explicit period bounds authorize negotiation. */
    enum class AudioLatencyProfile : std::uint8_t {
        Interactive,
        Balanced,
        PowerSaving
    };

    /** @brief Checks a requested buffering profile. @param profile Candidate. @return True for a defined profile. */
    [[nodiscard]] constexpr bool IsAudioLatencyProfile(const AudioLatencyProfile profile) noexcept {
        return profile <= AudioLatencyProfile::PowerSaving;
    }

    /** @brief A frame amount in its explicit source-rate domain. Zero frames can be a known value. */
    struct AudioTimingFrames final {
        std::uint32_t frames{};
        std::uint32_t sampleRate{};
    };

    /** @brief Complete ADR-062 callback identity; runtime generation is carried by device.owner. */
    struct AudioDeviceEpoch final {
        AudioDeviceId device;
        std::uint64_t formatRevision{};
        std::uint64_t callbackEpoch{};
        bool operator==(const AudioDeviceEpoch &) const noexcept = default;
    };

    /** @brief Nanoseconds in one explicitly named monotonic clock domain, never wall time. */
    struct AudioMonotonicTimestamp final {
        std::uint64_t clockDomain{}; /**< Non-zero Horo-assigned domain; adapters map native clocks before publication. */
        std::uint64_t nanoseconds{}; /**< Zero is a valid clock origin, not an absent timestamp. */
    };

    /** @brief Knowledge quality; missing, unsupported and temporarily unavailable values are distinct. */
    enum class AudioObservationQuality : std::uint8_t {
        Unknown,
        Unsupported,
        Unavailable,
        Estimated,
        Reported,
        Measured
    };
    /** @brief Evidence origin; backend timing estimates cannot masquerade as physical measurements. */
    enum class AudioObservationSource : std::uint8_t {
        None,
        BackendApi,
        Adapter,
        CallbackClock,
        Loopback,
        DeterministicClock
    };

    /**
     * @brief Duration with explicit knowledge quality, source, observation time and measurement window.
     *
     * Unknown/Unsupported/Unavailable carry no numeric value or evidence fields. Estimated
     * and Reported carry a value and timestamp but no measurement count/window. Measured
     * values are arithmetic means over a positive sampleCount and positive window ending
     * at observedAt; window duration cannot precede the clock origin. Numeric zero is valid.
     * Reported comes from BackendApi; Estimated from BackendApi or Adapter; Measured from
     * CallbackClock, Loopback or DeterministicClock. These classifications do not prove accuracy.
     */
    struct AudioDurationObservation final {
        AudioObservationQuality quality{AudioObservationQuality::Unknown};
        AudioObservationSource source{AudioObservationSource::None};
        std::optional<std::uint64_t> nanoseconds;
        std::optional<AudioMonotonicTimestamp> observedAt;
        std::optional<std::uint64_t> windowNanoseconds;
        std::uint64_t sampleCount{};
    };

    /**
     * @brief Owned timing snapshot for exactly one callback epoch and monotonic clock domain.
     *
     * Components may overlap according to backend accounting; never sum them implicitly.
     * API-reported hardware/queue latency is not measured end-to-end latency. Physical
     * hardware/end-to-end measurements require Loopback provenance. Null must report both
     * physical components Unsupported, but may report deterministic or actual CPU callback
     * timing. Callback execution and cadence measurements use CallbackClock or DeterministicClock.
     * Values may be unknown until enough samples exist; no defaults fabricate zero latency.
     */
    struct AudioDeviceTimingReport final {
        AudioDeviceEpoch epoch;
        AudioMonotonicTimestamp capturedAt;
        AudioDurationObservation hardwareLatency;
        AudioDurationObservation adapterLatency;
        AudioDurationObservation queuedLatency;
        AudioDurationObservation endToEndLatency;
        AudioDurationObservation callbackExecution;
        AudioDurationObservation callbackPeriod; /**< Mean start-to-start interval, not callback execution time. */
    };

    /**
     * @brief Matches all non-zero callback identity dimensions without accepting two empty tuples.
     * @param expected Current control-owned epoch.
     * @param observed Epoch attached to a callback acknowledgement, event or timing report.
     * @return True only for the exact current runtime/device/format/callback identity.
     */
    [[nodiscard]] bool MatchesAudioDeviceEpoch(const AudioDeviceEpoch &expected, const AudioDeviceEpoch &observed) noexcept;

    /**
     * @brief Validates duration evidence without allocation or conversion to a different clock domain.
     * @param observation Owned duration facts.
     * @return True for internally consistent quality, source, value, timestamp and measurement window.
     */
    [[nodiscard]] bool ValidateAudioDurationObservation(const AudioDurationObservation &observation) noexcept;

    /**
     * @brief Rejects stale epochs, cross-clock/future evidence and false physical/measurement claims.
     * @param report Immutable timing facts captured by control for one epoch.
     * @param expected Current control-owned callback identity.
     * @param backend Runtime's fixed selected backend; unknown identities fail validation.
     * @return True for a coherent report; neither activates audio nor authorizes callback-memory reclamation.
     */
    [[nodiscard]] bool ValidateAudioDeviceTimingReport(const AudioDeviceTimingReport &report, const AudioDeviceEpoch &expected,
                                                       AudioBackendKind backend) noexcept;
}  // namespace Horo::Audio
