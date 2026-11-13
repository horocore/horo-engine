#pragma once

/** @file CaptionTransport.h
 * @brief Fixed-size caption identities and payloads shared by semantic, Audio and UI owners.
 */
#include <compare>
#include <cstddef>
#include <cstdint>

namespace Horo::Accessibility {
    /** @brief Maximum cues in one bounded producer snapshot or queue. */
    inline constexpr std::size_t MaximumCaptionCues = 32;

    /** @brief Session-qualified identity assigned before independent semantic/audio fan-out. */
    struct CaptionCueId final {
        std::uint64_t session{};
        std::uint64_t sequence{};
        auto operator<=>(const CaptionCueId &) const = default;
    };
    /** @brief Closed cue classification; Dialogue eligibility is independent of audio playback. */
    enum class CaptionCueKind : std::uint8_t {
        Dialogue,
        Sound,
        Music
    };
    /** @brief Explicit bounded producer admission outcomes. */
    enum class CaptionAdmission : std::uint8_t {
        Accepted,
        Full,
        Invalid,
        StaleSession
    };
    /** @brief Producer authority; Audio-only queues cannot recreate semantic dialogue. */
    enum class CaptionProducerKind : std::uint8_t {
        Semantic,
        AudioOnly
    };

    /**
     * @brief Trivially copied producer metadata without text, assets, native state or UI implementation dependencies.
     * @details Text indexes identify entries in a catalog pinned by the producer/consumer
     * composition before callback work. No pointer or borrowed string crosses the transport.
     * Semantic producers assign IDs before fan-out; optional Audio observations preserve
     * them. Simulation tick timing, not mute/device/playback state, determines eligibility.
     */
    struct CaptionCue final {
        CaptionCueId id;
        CaptionCueKind kind{CaptionCueKind::Dialogue};
        std::uint32_t text{};    /**< Zero-based index in the composition-pinned catalog. */
        std::uint32_t speaker{}; /**< Zero means absent; otherwise one-based catalog index. */
        std::uint64_t startTick{};
        std::uint64_t durationTicks{};
    };

    /** @brief Optional Audio observation of an existing semantic cue; cannot create text or restart its simulation lifetime. */
    struct CaptionTimingObservation final {
        CaptionCueId id;
        std::uint64_t observedStartTick{}; /**< Audio/control clock correlation projected onto the host simulation clock. */
    };
}  // namespace Horo::Accessibility
