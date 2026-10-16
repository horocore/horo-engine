#include "editor/screens/workspace/panels/inspector/InspectorEditSession.h"

namespace Horo::Editor {
    /** @copydoc InspectorEditSession::IsAudioSourceValid */
    bool InspectorEditSession::IsAudioSourceValid() const noexcept {
        return m_draft.audioSource.has_value() && IsValidAudioSourceComponent(*m_draft.audioSource);
    }

    /** @copydoc InspectorEditSession::IsAudioListenerValid */
    bool InspectorEditSession::IsAudioListenerValid() const noexcept {
        return m_draft.audioListener.has_value() && IsValidAudioListenerComponent(*m_draft.audioListener);
    }

    /** @copydoc InspectorEditSession::ApplyAudioSourceEdit */
    EditorWorkspaceViewCommandData InspectorEditSession::ApplyAudioSourceEdit(const InspectorAudioSourceEdit &edit,
                                                                              const SceneObject &object, const bool allowCommands) {
        if (m_baselines.size() != 1 || m_draft.object != object.id || !object.components.audioSource.has_value() ||
            !m_draft.audioSource.has_value()) {
            return {};
        }
        if (edit.cancelRequested) {
            ResetAudioSourceDraft(object);
            return {};
        }
        if (!allowCommands || !edit.committed || !IsValidAudioSourceComponent(*m_draft.audioSource) ||
            *m_draft.audioSource == *object.components.audioSource)
            return {};

        auto command =
            EditorWorkspaceViewCommandData{.command = EditorWorkspaceViewCommand::UpdateAudioSourceComponent, .objectPayload = object.id};
        command.audioSourcePayload = *m_draft.audioSource;
        return command;
    }

    /** @copydoc InspectorEditSession::ApplyAudioListenerEdit */
    EditorWorkspaceViewCommandData InspectorEditSession::ApplyAudioListenerEdit(const InspectorAudioListenerEdit &edit,
                                                                                const SceneObject &object, const bool allowCommands) {
        if (m_baselines.size() != 1 || m_draft.object != object.id || !object.components.audioListener.has_value() ||
            !m_draft.audioListener.has_value()) {
            return {};
        }
        if (edit.cancelRequested) {
            ResetAudioListenerDraft(object);
            return {};
        }
        if (!allowCommands || !edit.committed || !IsValidAudioListenerComponent(*m_draft.audioListener) ||
            *m_draft.audioListener == *object.components.audioListener)
            return {};

        auto command =
            EditorWorkspaceViewCommandData{.command = EditorWorkspaceViewCommand::UpdateAudioListenerComponent, .objectPayload = object.id};
        command.audioListenerPayload = *m_draft.audioListener;
        return command;
    }
}  // namespace Horo::Editor
