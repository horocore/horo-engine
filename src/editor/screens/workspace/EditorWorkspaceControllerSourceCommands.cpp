#include "EditorBuildCommandEndpoint.h"
#include "EditorSourcePresentation.h"
#include "EditorWorkspaceController.h"

namespace Horo::Editor {
    namespace {
        const ErrorCodeDescriptor BuildAwaitingPlay{
            .domain = ErrorDomainId{"editor.gameplay"},
            .code = ErrorCode{"play_waits_for_build"},
            .defaultSeverity = ErrorSeverity::Info,
            .summary = "Play still owns a pending gameplay build result.",
        };
    }

    /** @copydoc EditorWorkspaceController::BindSourcePresentation */
    std::weak_ptr<const EditorSourceCommandEndpoint> EditorWorkspaceController::BindSourcePresentation(
        std::string project, const std::uint64_t authorizationRevision) {
        auto endpoint = std::make_shared<const EditorSourceCommandEndpoint>(
            EditorSourceCommandEndpoint{std::move(project), authorizationRevision, std::this_thread::get_id(),
                                        [this](const SourceDocumentId &path, std::optional<EditorSourceCommandPosition> position,
                                               const std::function<bool()> &stop) {
            return PresentSourceCommand(path, SourceCommandLocation(path, position), stop);
        }});
        m_sourceCommandEndpoint = std::move(endpoint);
        return m_sourceCommandEndpoint;
    }

    /** @copydoc EditorWorkspaceController::BindGameplayBuildCommands */
    std::weak_ptr<const EditorBuildCommandEndpoint> EditorWorkspaceController::BindGameplayBuildCommands(
        std::string project, const std::uint64_t authorizationRevision) {
        m_buildCommandEndpoint.reset();
        if (!m_gameplayBuilds || m_gameplayBuildEnvironment.gameplaySdkPackage.empty())
            return {};
        if (m_commandBuildSession) {
            static_cast<void>(m_gameplayBuilds->RequestCancel(*m_commandBuildSession));
            m_commandBuildSession.reset();
        }
        m_buildCommandEndpoint = std::make_shared<const EditorBuildCommandEndpoint>(
            EditorBuildCommandEndpoint{std::move(project), authorizationRevision, std::this_thread::get_id(),
                                       [this](const std::function<bool()> &stop) {
            return StartApprovedGameplayBuild(stop);
        }, [this](const auto id) {
            return m_commandBuildSession == id ? m_gameplayBuilds->Query(id) : std::nullopt;
        }, [this](const auto id) {
            return m_commandBuildSession == id && m_gameplayBuilds->RequestCancel(id);
        }});
        return m_buildCommandEndpoint;
    }

    /** @copydoc EditorWorkspaceController::StartApprovedGameplayBuild */
    Result<Application::GameplayBuildSessionId> EditorWorkspaceController::StartApprovedGameplayBuild(const std::function<bool()> &stop) {
        if (m_playAfterGameplayBuild)
            return Result<Application::GameplayBuildSessionId>::Failure(MakeError(BuildAwaitingPlay));
        auto started = m_gameplayBuilds->StartIsolated(MakeGameplayBuildRequest(), stop);
        if (started.HasValue()) {
            m_commandBuildSession = started.Value();
            m_gameplayBuildSession = started.Value();
        }
        return started;
    }

    /** @copydoc EditorWorkspaceController::RevokeWorkspaceCommands */
    void EditorWorkspaceController::RevokeWorkspaceCommands() noexcept {
        m_sourceCommandEndpoint.reset();
        m_buildCommandEndpoint.reset();
        if (m_commandBuildSession) {
            static_cast<void>(m_gameplayBuilds->RequestCancel(*m_commandBuildSession));
            m_commandBuildSession.reset();
        }
    }

    /** @copydoc EditorWorkspaceController::PresentSourceCommand */
    Result<void> EditorWorkspaceController::PresentSourceCommand(const SourceDocumentId &path,
                                                                 std::optional<Language::LanguageLocation> target,
                                                                 const std::function<bool()> &stop) {
        auto shown =
            PresentEditorSource(m_sourceOpenService, m_viewModel.workspacePanelHost, path, std::move(target), m_sourceOpenNavigator, stop);
        if (shown.HasError())
            return Result<void>::Failure(shown.ErrorValue());
        m_viewModel.sourceLanguageFocus = std::move(shown).Value();
        RefreshSourceDocumentProjection();
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
