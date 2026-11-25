#pragma once

/** @file EditorMicrophoneSpeechSession.h @brief macOS composition of the editor microphone and speech worker. */

#include "ImmersiveCommandEnvelope.h"
#include "ImmersiveSpeechWorker.h"

#include <vector>

namespace Horo::Editor {
    class EditorMicrophonePermission;

    /** @brief One read of the capture clock tied to a specific listening operation. */
    struct EditorSpeechClockSample final {
        Audio::AudioMonotonicTimestamp timestamp;
        std::uint64_t generation{};
    };

    /** @brief Bridges explicit foreground listening and sealing to an on-device worker.
     * @details Audio control owns this object outside XR and Audio callbacks. The borrowed microphone owner
     * outlives the session, including teardown. Cancel after adoption or when focus/consent is withdrawn.
     */
    class EditorMicrophoneSpeechSession final {
    public:
        explicit EditorMicrophoneSpeechSession(EditorMicrophonePermission &microphone) noexcept;
        ~EditorMicrophoneSpeechSession();

        /** @brief Opens the exact selected input and prepares one on-device transcription. */
        [[nodiscard]] SpeechTranscriptionStatus Start(std::size_t input, bool foreground, const ImmersiveAuthoringContext &owner,
                                                      const ImmersivePlanningContext &context, std::string_view locale,
                                                      float minimumConfidence);
        /** @brief Records one XR pointing observation measured during the current listening generation.
         * @param observation Exact cutoff, geometry and measured Audio interval from the XR owner.
         * @param captureGeneration Listening generation bound to both clock readings.
         * @return Recorded, stale, closed or over-capacity.
         */
        [[nodiscard]] ImmersiveSpatialCaptureStatus RecordPointing(const ImmersiveSpatialObservation &observation,
                                                                   std::uint64_t captureGeneration) noexcept;
        /** @brief Returns clock evidence only while the active speech lease is accepting input. */
        [[nodiscard]] std::optional<EditorSpeechClockSample> ReadCaptureClock() const noexcept;
        /** @brief Detaches the microphone before letting the worker finalize the utterance. */
        [[nodiscard]] SpeechTranscriptionStatus Seal(bool foreground);
        /** @brief Relays a bounded batch of PCM and polls at most one recognition result. */
        [[nodiscard]] SpeechTranscriptionUpdate Advance(bool foreground);
        /** @brief Reconciles native consent and assembles final speech with sealed spatial evidence. */
        [[nodiscard]] ImmersiveCommandAssembly AssembleCommand(bool foreground, const ImmersiveSpeechEvidence &speech,
                                                               const XR::XRInteractionCutoff &cutoff);
        /** @brief Revokes the capture and invalidates all unadopted evidence. */
        void Cancel() noexcept;

    private:
        [[nodiscard]] SpeechTranscriptionStatus TransferBlock(bool foreground);
        EditorMicrophonePermission &microphone_;
        ImmersiveSpeechWorker worker_;
        ImmersiveSpatialCapture spatial_;
        std::vector<Audio::AudioSample> scratch_;
        std::uint64_t generation_{};
        std::size_t channels_{};
        bool sealed_{};
    };
}  // namespace Horo::Editor
