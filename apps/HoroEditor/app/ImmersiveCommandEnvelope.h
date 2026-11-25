#pragma once

/** @file ImmersiveCommandEnvelope.h @brief Editor-owned alignment of speech and admitted spatial evidence. */

#include "Horo/XR/XRInteraction.h"
#include "Horo/XR/XRPlaneTracking.h"
#include "ImmersiveSpeechTranscription.h"

#include <array>
#include <optional>
#include <span>

namespace Horo::Editor {
    /** @brief Tracks the observed raw button edge while leaving gesture ownership with the routed input owner. */
    class ImmersivePointingPressEdge final {
    public:
        /** @brief Accepts a new scene-owned press in the same capture generation, after a release was observed. */
        [[nodiscard]] bool Observe(std::uint64_t captureGeneration, bool down, bool uiOwned) noexcept;
        /** @brief Disarms an outgoing pointer source before its replacement can report a neutral sample. */
        void Reset() noexcept;

    private:
        std::uint64_t generation_{};
        bool down_{};
    };

    /** @brief Bounds the observed transition between released and pressed controller states in Audio time. */
    struct ImmersiveControllerPress final {
        Audio::AudioMonotonicTimestamp earliest;
        Audio::AudioMonotonicTimestamp latest;
    };

    /** @brief Copied authored geometry evidence associated with one measured press. */
    struct ImmersivePointingHits final {
        static constexpr std::size_t MaximumObjects = 16;

        struct Hit final {
            SceneSource::SceneObjectId object;
            float distanceMeters{};
            Math::Vec3 worldPosition;
        };

        std::uint64_t worldGeneration{};
        std::uint64_t structuralRevision{};
        bool mounted{};
        std::array<Hit, MaximumObjects> hits{};
        std::size_t count{};
    };

    /** @brief Short-lived surface coordinates tied to the exact press and plane grant. */
    struct ImmersiveSurfaceHits final {
        static constexpr std::size_t MaximumHits = 8;

        struct Hit final {
            XR::XRPlaneId plane;
            XR::XRPlaneClassification classification{};
            Math::Vec3 worldPosition;
        };

        XR::XRPlaneScope scope;
        std::uint64_t worldGeneration{};
        std::array<Hit, MaximumHits> hits{};
        std::size_t count{};
    };

    /** @brief Closed interval measured by the host around the exact XR cutoff in the capture clock domain. */
    struct ImmersiveSpatialObservation final {
        XR::XRInteractionCutoff cutoff;
        XR::XRInteractionCandidate candidate;
        Audio::AudioMonotonicTimestamp earliest;
        Audio::AudioMonotonicTimestamp latest;
        std::optional<ImmersiveControllerPress> controllerPress;
        std::optional<ImmersivePointingHits> pointing;
        std::optional<ImmersiveSurfaceHits> surfaces;
    };

    /** @brief Borrowed history from one bounded editor/XR capture, in publication order. */
    struct ImmersiveSpatialHistoryView final {
        const ImmersivePlanningContext &context;
        XR::XRInteractionCutoff cutoff;
        std::span<const ImmersiveSpatialObservation> observations;
    };

    enum class ImmersivePointingReviewKind : std::uint8_t {
        Objects,
        Locations
    };

    /** @brief Collects complete same-world pointing evidence from spatial observations already aligned to final speech.
     * @param aligned Borrowed observations admitted by the command envelope.
     * @param worldGeneration Exact Play occurrence in the command context.
     * @param kind Selects whether distinct points on the same object remain separate.
     * @return Complete evidence, including empty for a verified miss, or no value for incomplete or invalid evidence. */
    [[nodiscard]] std::optional<ImmersivePointingHits> CollectImmersivePointingHits(std::span<const ImmersiveSpatialObservation> aligned,
                                                                                    std::uint64_t worldGeneration,
                                                                                    ImmersivePointingReviewKind kind) noexcept;

    /** @brief Reviews valid press-time geometry against the exact current grant and all referenced live planes. */
    [[nodiscard]] std::optional<ImmersiveSurfaceHits> CollectImmersiveSurfaceHits(std::span<const ImmersiveSpatialObservation> aligned,
                                                                                  std::uint64_t worldGeneration,
                                                                                  const XR::XRPlaneSnapshot &current) noexcept;

    enum class ImmersiveCommandStatus : std::uint8_t {
        Aligned,
        SpeechOnly,
        StaleEvidence,
        PartialSpeech,
        InvalidSpatialEvidence,
        AmbiguousTiming,
        CapacityExceeded,
        SpatialCaptureOpen,
        SpeechOnlySpatialRejected
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

    /** @brief Alignment outcome. SpeechOnlySpatialRejected carries only a local transcript preview. */
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

        /** @brief Retains eligible final speech for local display when optional spatial evidence cannot be used. */
        [[nodiscard]] ImmersiveCommandAssembly AssemblePreview(const ImmersiveSpeechEvidence &speech,
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
