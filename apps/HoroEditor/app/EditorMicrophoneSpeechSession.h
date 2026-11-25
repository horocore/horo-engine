#pragma once

/** @file EditorMicrophoneSpeechSession.h @brief macOS composition of the editor microphone and speech worker. */

#include "ImmersiveCommandEnvelope.h"
#include "ImmersiveSpeechWorker.h"

#include <vector>

namespace Horo::Editor {
    class EditorMicrophonePermission;

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
        /** @brief Records one observed batch from the XR owner while the utterance is still accepting input. */
        [[nodiscard]] ImmersiveSpatialCaptureStatus RecordSpatial(const ImmersivePlanningContext &context,
                                                                  std::span<const ImmersiveSpatialObservation> observations) noexcept;
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
