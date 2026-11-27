#pragma once

/** @file EditorImmersiveListening.h @brief Local Play listening controls for the editor Agent panel. */

#include "EditorMicrophoneSpeechSession.h"
#include "Horo/Editor/AgentSceneSelectionSnapshot.h"
#include "OpenXRPlayHost.h"

#include <functional>

namespace Horo::Editor {
    /** @brief Retains and revalidates one explicitly selected local capture while the editor panel is attached. */
    class EditorImmersiveListening final {
    public:
        enum class State : std::uint8_t {
            Idle,
            Listening,
            Finishing,
            SpeechOnly,
            SpeechOnlySpatialRejected,
            Spatial,
            Interrupted
        };

        EditorImmersiveListening(EditorMicrophonePermission &microphone, EditorMicrophoneSpeechSession &speech,
                                 const Application::Internal::OpenXRPlayHost &xr, Input::PlayerId player);
        ~EditorImmersiveListening();

        /** @brief Returns the current UI state. */
        [[nodiscard]] State Status() const noexcept;
        /** @brief Borrows the final transcript only while its XR capture is current. */
        [[nodiscard]] std::string_view Transcript() const &;
        /** @brief Borrows the latest live draft for display while final recognition is pending. */
        [[nodiscard]] std::string_view PartialTranscript() const &;

        /** @brief Reads progress only for the current XR listening scope, including before the first draft. */
        [[nodiscard]] std::optional<ImmersiveSpeechProgress> ReadProgress() const;

        /** @brief Returns the terminal capture reason while Status is Interrupted. */
        [[nodiscard]] SpeechTranscriptionStatus FailureReason() const noexcept {
            return failureReason_;
        }

        /** @brief Workspace-validated candidates with the press evidence from this listening capture. */
        struct ReferenceReview final {
            AgentSceneReferenceReview scene;
            bool hasMissedPress{};
            CancellationToken spatialLifetime{}; /**< Empty for editor selection; withdrawn when the XR source retires. */
        };

        /** @brief Reads the captured selection or aligned pointing candidates on the workspace owner lane. */
        [[nodiscard]] std::optional<ReferenceReview> ReviewReferences(
            const std::function<std::optional<AgentSceneReferenceReview>(const AgentSceneReferenceRequest &)> &review,
            AgentSceneReferenceSource source) const;
        /** @brief Borrows a fresh plane publication before returning any captured surface coordinates. */
        [[nodiscard]] std::optional<ImmersiveSurfaceHits> ReviewSurfaces() const;
        /** @brief Current consent and host readiness for the explicit listening controls. */
        struct StartAvailability final {
            enum class Blocker : std::uint8_t {
                None,
                PermissionRequired,
                PermissionPending,
                PermissionDenied,
                PermissionUnavailable,
                InputBusy,
                PlayUnavailable
            };

            Blocker blocker;
            bool requestMicrophone{};
            bool requestSpeech{};
        };

        /** @brief Reads the current gate without prompting or beginning microphone capture. */
        [[nodiscard]] StartAvailability ReadStartAvailability();
        [[nodiscard]] std::span<const std::string> Inputs() const noexcept;
        [[nodiscard]] std::optional<Audio::AudioCaptureDeviceId> InputAt(std::size_t index) const noexcept;

        /** @brief Explicit permission and device actions. These do not start listening. */
        void RequestMicrophone();
        void RequestSpeech() noexcept;
        void RefreshInputs();

        /** @brief Starts an exclusive on-device utterance scoped to the selected scene and focused XR player. */
        void Start(Audio::AudioCaptureDeviceId input, std::string_view locale, std::uint64_t projectOpening,
                   const AgentSceneSelectionSnapshot &selection);
        /** @brief Detaches the exact microphone before accepting its final transcript. */
        void Finish();
        /** @brief Advances capture and revalidates the same context before retaining any transcript. */
        void Poll();
        /** @brief Clears the transient preview and revokes the listening operation. */
        void Cancel() noexcept;

    private:
        using Scope = Application::Internal::OpenXRPlayListeningScope;
        /** @brief Checks the listening lineage and any final envelope's Input cutoff. */
        [[nodiscard]] std::optional<Scope> Current() const;
        /** @brief Rechecks the live XR scope and current microphone and speech consent. */
        [[nodiscard]] std::optional<Scope> CurrentPresentation() const;
        /** @brief Rechecks presentation and the exact preview generation after querying an external owner. */
        [[nodiscard]] bool CanPresentPreview(std::uint64_t generation) const;
        /** @brief Assembles the final local preview after recognition finishes. */
        void AdoptFinalSpeech(const ImmersiveSpeechEvidence &evidence, const XR::XRInteractionCutoff &cutoff);
        /** @brief Releases the capture and preserves its user-visible terminal reason. */
        void Fail(SpeechTranscriptionStatus reason = SpeechTranscriptionStatus::CaptureInterrupted) noexcept;

        EditorMicrophonePermission &microphone_;
        EditorMicrophoneSpeechSession &speech_;
        const Application::Internal::OpenXRPlayHost &xr_;
        Input::PlayerId player_;
        ImmersiveAuthoringContext owner_;
        std::optional<Scope> scope_;
        std::optional<ImmersivePlanningContext> context_;
        std::optional<ImmersiveSpeechEvidence> partial_;
        std::optional<ImmersiveCommandEnvelope> preview_;
        State state_{State::Idle};
        SpeechTranscriptionStatus failureReason_{SpeechTranscriptionStatus::CaptureInterrupted};
    };
}  // namespace Horo::Editor
