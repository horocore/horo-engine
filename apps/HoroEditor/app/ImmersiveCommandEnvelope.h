#pragma once

/** @file ImmersiveCommandEnvelope.h @brief Editor-owned alignment of speech and admitted spatial evidence. */

#include "Horo/XR/XRInteraction.h"
#include "ImmersiveSpeechTranscription.h"

#include <array>
#include <optional>
#include <span>

namespace Horo::Editor {
    /** @brief Bounds the observed transition between released and pressed controller states in Audio time. */
    struct ImmersiveControllerPress final {
        Audio::AudioMonotonicTimestamp earliest;
        Audio::AudioMonotonicTimestamp latest;
    };

    /** @brief Closed interval measured by the host around the exact XR cutoff in the capture clock domain. */
    struct ImmersiveSpatialObservation final {
        XR::XRInteractionCutoff cutoff;
        XR::XRInteractionCandidate candidate;
        Audio::AudioMonotonicTimestamp earliest;
        Audio::AudioMonotonicTimestamp latest;
        std::optional<ImmersiveControllerPress> controllerPress;
    };

    /** @brief Borrowed history from one bounded editor/XR capture, in publication order. */
    struct ImmersiveSpatialHistoryView final {
        const ImmersivePlanningContext &context;
        XR::XRInteractionCutoff cutoff;
        std::span<const ImmersiveSpatialObservation> observations;
    };

    enum class ImmersiveCommandStatus : std::uint8_t {
        Aligned,
        SpeechOnly,
        StaleEvidence,
        PartialSpeech,
        InvalidSpatialEvidence,
        AmbiguousTiming,
        CapacityExceeded,
        SpatialCaptureOpen
    };

    struct ImmersiveCommandAssembly;

    /** @brief One bounded command candidate. The speech lease remains its revocation authority. */
    class ImmersiveCommandEnvelope final {
    public:
        static constexpr std::size_t MaximumObservations = 32;

        /** @brief Aligns final speech with spatial observations inside its phrase window on the editor owner lane.
         * @param current Exact current editor context, including scene revision and selection.
         * @param speech Owned, final evidence issued by the admitted speech stream.
         * @param history Optional history and latest cutoff from the same editor generation.
         * @return An owned envelope, explicit speech-only degradation, or a typed evidence failure.
         * @details The host measures both ends of each observation in the same clock as the microphone.
         * The phrase includes its start and excludes its end. Candidates from a shared cutoff use the same interval.
         * Raw XR timestamps are used only for ordering observations within that XR session. No clock offset is inferred.
         */
        [[nodiscard]] static ImmersiveCommandAssembly Assemble(const ImmersivePlanningContext &current,
                                                               const ImmersiveSpeechEvidence &speech,
                                                               std::optional<ImmersiveSpatialHistoryView> history = {}) noexcept;

        /** @brief Rechecks speech cancellation and the current XR cutoff before adopting the command. */
        [[nodiscard]] bool IsCurrent(const XR::XRInteractionCutoff &cutoff = {}) const noexcept;

        [[nodiscard]] const ImmersiveSpeechEvidence &Speech() const & noexcept {
            return speech_;
        }

        [[nodiscard]] const ImmersiveSpeechEvidence &Speech() const && = delete;

        [[nodiscard]] std::span<const ImmersiveSpatialObservation> Spatial() const & noexcept {
            return std::span{observations_}.first(count_);
        }

        [[nodiscard]] std::span<const ImmersiveSpatialObservation> Spatial() const && = delete;

    private:
        explicit ImmersiveCommandEnvelope(const ImmersiveSpeechEvidence &speech) noexcept : speech_(speech) {}

        /** @brief Validates the ordered batch and copies observations that fall inside the phrase. */
        ImmersiveCommandStatus AppendSpatial(const ImmersiveSpatialHistoryView &history, const SpeechHypothesisView &phrase) noexcept;

        ImmersiveSpeechEvidence speech_;
        XR::XRInteractionCutoff cutoff_;
        std::array<ImmersiveSpatialObservation, MaximumObservations> observations_{};
        std::size_t count_{};
    };

    /** @brief Evidence alignment outcome. Only Aligned and SpeechOnly carry a command. */
    struct ImmersiveCommandAssembly final {
        ImmersiveCommandStatus status;
        std::optional<ImmersiveCommandEnvelope> envelope;
    };

    enum class ImmersiveSpatialCaptureStatus : std::uint8_t {
        Recorded,
        Closed,
        StaleContext,
        CapacityExceeded
    };

    /** @brief Fixed storage for admitted XR evidence during one listening operation.
     * @details All operations belong to the editor owner lane. Source owners submit observations in publication
     * order with measured timestamps. Once sealed, the history remains frozen until the speech worker completes.
     */
    class ImmersiveSpatialCapture final {
    public:
        /** @brief Clears the last operation and opens one capture using the exact current planning generation. */
        void Begin(const ImmersivePlanningContext &context) noexcept;

        /** @brief Copies one complete, ordered batch without allocation or partially admitting an oversized batch. */
        [[nodiscard]] ImmersiveSpatialCaptureStatus Record(const ImmersivePlanningContext &context,
                                                           std::span<const ImmersiveSpatialObservation> observations) noexcept;
        /** @brief Records one pointing observation in the bound listening context; rejects foreign XR sessions. */
        [[nodiscard]] ImmersiveSpatialCaptureStatus RecordPointing(const ImmersiveSpatialObservation &observation) noexcept;

        /** @brief Stops recording before the detached speech worker produces its final result. */
        void Seal() noexcept;

        /** @brief Assembles final evidence using the latest cutoff from the same XR owner. */
        [[nodiscard]] ImmersiveCommandAssembly Assemble(const ImmersiveSpeechEvidence &speech,
                                                        const XR::XRInteractionCutoff &cutoff) const noexcept;

        /** @brief Discards the captured generation and its stored observations. */
        void Reset() noexcept;

    private:
        std::optional<ImmersivePlanningContext> context_;
        std::array<ImmersiveSpatialObservation, ImmersiveCommandEnvelope::MaximumObservations> observations_{};
        std::size_t count_{};
        bool sealed_{};
        bool overflowed_{};
    };
}  // namespace Horo::Editor
