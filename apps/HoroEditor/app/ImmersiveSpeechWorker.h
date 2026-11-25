#pragma once

/** @file ImmersiveSpeechWorker.h @brief Bounded Audio-control to speech-worker handoff. */

#include "AgentBackgroundTurn.h"
#include "ImmersiveSpeechTranscription.h"

namespace Horo::Editor {
    /** @brief One editor-owned listening operation with fixed PCM and recognition-result storage.
     * @details Call control methods serially outside XR and Audio callbacks. Destruction joins the
     * worker during host teardown. The worker owns the provider and never borrows editor or microphone state.
     */
    class ImmersiveSpeechWorker final {
    public:
        ImmersiveSpeechWorker() = default;
        ~ImmersiveSpeechWorker();
        ImmersiveSpeechWorker(const ImmersiveSpeechWorker &) = delete;
        ImmersiveSpeechWorker &operator=(const ImmersiveSpeechWorker &) = delete;

        /** @brief Starts one worker for the exact admitted capture and editor context.
         * @param owner Current context owner. @param context Exact planning snapshot.
         * @param source Capture description including the queue bounds.
         * @param captureRevoked Audio lease cancellation, including after native detachment.
         * @param provider Retained host-selected adapter. @param locale Recognition language.
         * @param minimumConfidence Inclusive acceptance threshold. @return Accepted or typed failure.
         */
        [[nodiscard]] SpeechTranscriptionStatus Start(const ImmersiveAuthoringContext &owner, const ImmersivePlanningContext &context,
                                                      const Audio::Backend::AudioCaptureBufferDescriptor &source,
                                                      std::stop_token captureRevoked, std::shared_ptr<ISpeechProvider> provider,
                                                      std::string_view locale, float minimumConfidence);

        /** @brief Copies one live block before the host scrubs its scratch. An interrupted or full queue cancels the utterance. */
        [[nodiscard]] SpeechTranscriptionStatus Submit(const Audio::Backend::AudioCapturedBlock &block,
                                                       std::span<const Audio::AudioSample> pcm) noexcept;
        /** @brief Transfers the native tail after detachment. The worker processes it after live PCM and then seals provider input. */
        [[nodiscard]] SpeechTranscriptionStatus Seal(Audio::Backend::AudioCaptureBuffer tail) noexcept;
        /** @brief Reads a partial or terminal update without blocking while the worker is busy. */
        [[nodiscard]] SpeechTranscriptionUpdate Poll();
        /** @brief Invalidates queued and already-delivered evidence and requests worker cancellation. */
        void Cancel() noexcept;
        /** @brief Reports whether one operation still needs to be drained. */
        [[nodiscard]] bool Busy() const noexcept;

    private:
        struct State;
        std::shared_ptr<State> state_;
        Agent::AgentBackgroundTurn<SpeechTranscriptionUpdate> worker_;
    };
}  // namespace Horo::Editor
