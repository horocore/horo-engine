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
            Spatial,
            Interrupted
        };

        EditorImmersiveListening(EditorMicrophonePermission &microphone, EditorMicrophoneSpeechSession &speech,
                                 const Application::Internal::OpenXRPlayHost &xr, Input::PlayerId player);
        ~EditorImmersiveListening();

        /** @brief Returns the current UI state and a transient preview, if the captured context remains valid. */
        [[nodiscard]] State Status() const noexcept;
        [[nodiscard]] std::string_view Transcript() const & noexcept;
        /** @brief Reads the captured selection or aligned pointing candidates on the workspace owner lane. */
        [[nodiscard]] std::optional<AgentSceneReferenceReview> ReviewReferences(
            const std::function<std::optional<AgentSceneReferenceReview>(const AgentSceneReferenceRequest &)> &review,
            AgentSceneReferenceSource source) const;
        [[nodiscard]] bool CanListen();
        [[nodiscard]] bool NeedsMicrophonePermission();
        [[nodiscard]] bool NeedsSpeechPermission() const noexcept;
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
        [[nodiscard]] std::optional<Scope> Current() const;
        void Fail() noexcept;

        EditorMicrophonePermission &microphone_;
        EditorMicrophoneSpeechSession &speech_;
        const Application::Internal::OpenXRPlayHost &xr_;
        Input::PlayerId player_;
        ImmersiveAuthoringContext owner_;
        std::optional<Scope> scope_;
        CancellationToken selectionLifetime_;
        std::optional<ImmersiveCommandEnvelope> preview_;
        State state_{State::Idle};
    };
}  // namespace Horo::Editor
