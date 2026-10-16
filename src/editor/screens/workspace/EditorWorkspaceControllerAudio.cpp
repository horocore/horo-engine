#include "Horo/Runtime/Scene/AudioSceneExtraction.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>

namespace Horo::Editor {
    /** @copydoc EditorWorkspaceController::ProcessAudioComponentCommand */
    bool EditorWorkspaceController::ProcessAudioComponentCommand(const EditorWorkspaceViewCommandData &cmd) {
        switch (cmd.command) {
            case EditorWorkspaceViewCommand::StartAudioComponentPreview:
                if (cmd.objectPayload && cmd.audioPreviewRevision)
                    static_cast<void>(StartAudioPreview(*cmd.objectPayload, *cmd.audioPreviewRevision));
                break;
            case EditorWorkspaceViewCommand::StopAudioComponentPreview:
                StopAudioPreview();
                break;
            case EditorWorkspaceViewCommand::UpdateAudioSourceComponent:
                if (cmd.objectPayload.has_value() && cmd.audioSourcePayload.has_value()) {
                    HandleDocumentCommandResult(m_documentCommands.Execute(
                                                    SetSceneObjectAudioSourceCommand{*cmd.objectPayload, *cmd.audioSourcePayload}),
                                                "Update audio source");
                }
                break;
            case EditorWorkspaceViewCommand::UpdateAudioListenerComponent:
                if (cmd.objectPayload.has_value() && cmd.audioListenerPayload.has_value()) {
                    HandleDocumentCommandResult(m_documentCommands.Execute(
                                                    SetSceneObjectAudioListenerCommand{*cmd.objectPayload, *cmd.audioListenerPayload}),
                                                "Update audio listener");
                }
                break;
            default:
                return false;
        }
        return true;
    }

    /** @copydoc EditorWorkspaceController::StartAudioPreview */
    Result<void> EditorWorkspaceController::StartAudioPreview(const SceneObjectId object, const DocumentRevision revision) {
        const auto fail = [this](const Error &error) {
            m_viewModel.audioPreviewFailure = error;
            return Result<void>::Failure(error);
        };
        if (!m_audioPreview)
            return fail(MakeError(AudioComponentPreviewErrors::ProviderUnavailable));
        const auto scene = m_runtimeScene.ActiveScene();
        if (revision != m_document.Revision() || m_viewModel.primarySelection != object || !scene ||
            m_activeRuntimeRevision.value != m_document.State().value)
            return fail(MakeError(AudioComponentPreviewErrors::Stale));
        const auto entity = scene->Find(Runtime::SceneObjectId{object.value});
        if (!entity)
            return fail(MakeError(AudioComponentPreviewErrors::Stale));
        Runtime::AudioSceneExtractor extractor;
        const auto owner = Audio::AudioRuntimeId::Create(1).Value();
        auto frame = extractor.Capture(*scene, {{owner, 1, 1}, 1, 1.0F, Audio::AudioListenerPolicy::Primary, {}, true});
        if (frame.HasError())
            return fail(frame.ErrorValue());
        const auto sources = frame.Value().Sources();
        const auto source = std::ranges::find_if(sources, [&entity](const auto &value) {
            return value.identity.slot == entity->entity.index + 1 && value.identity.generation == entity->entity.generation;
        });
        if (source == sources.end())
            return fail(MakeError(AudioComponentPreviewErrors::Stale));
        const auto listeners = frame.Value().Listeners();
        AudioComponentPreviewRequest request{m_audioPreviewWorkspace,
                                             revision.value,
                                             object.value,
                                             m_sourceOpenService.ProjectRoot(),
                                             m_assetRegistry,
                                             *source,
                                             listeners.empty() ? std::nullopt : std::optional{listeners.front()}};
        auto started = m_audioPreview->Start(request);
        if (started.HasError())
            return fail(started.ErrorValue());
        m_viewModel.audioPreviewFailure.reset();
        ReconcileAudioPreview();
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::StopAudioPreview */
    void EditorWorkspaceController::StopAudioPreview() noexcept {
        if (m_audioPreview)
            m_audioPreview->Stop(m_audioPreviewWorkspace);
    }

    /** @copydoc EditorWorkspaceController::ReconcileAudioPreview */
    void EditorWorkspaceController::ReconcileAudioPreview() {
        if (!m_audioPreview)
            return;
        auto snapshot = m_audioPreview->Snapshot();
        if (snapshot.workspace != m_audioPreviewWorkspace)
            return;
        if (snapshot.revision != m_document.Revision().value || !m_selection.Current().primary ||
            snapshot.object != m_selection.Current().primary->value) {
            StopAudioPreview();
            m_viewModel.audioPreviewFailure.reset();
            m_viewModel.audioPreview.reset();
            return;
        }
        m_viewModel.audioPreview = std::move(snapshot);
        if (m_viewModel.audioPreview->failure)
            m_viewModel.audioPreviewFailure = m_viewModel.audioPreview->failure;
    }
}  // namespace Horo::Editor
