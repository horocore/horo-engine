#include "EditorWorkspaceController.h"
#include "Horo/Foundation/Logging/Logger.h"

namespace Horo::Editor {
    /** @copydoc EditorWorkspaceController::BindSourceLanguage */
    void EditorWorkspaceController::BindSourceLanguage(IExternalProcessRunner &runner, Language::ClangdSettings settings) {
        m_sourceLanguageRunner = &runner;
        m_clangdSettings = std::move(settings);
        PrepareSourceLanguage();
    }

    /** @copydoc EditorWorkspaceController::PrepareSourceLanguage */
    void EditorWorkspaceController::PrepareSourceLanguage() {
        if (!m_sourceLanguageRunner || m_sourceLanguage)
            return;
        auto provider = Language::PrepareClangdProvider(m_sourceOpenService, m_clangdSettings);
        if (!provider.HasError()) {
            auto workspace =
                Language::ProjectLanguageWorkspace::Create(*m_sourceLanguageRunner, m_sourceOpenService, std::span{&provider.Value(), 1});
            if (workspace.HasValue()) {
                m_sourceLanguage = std::move(workspace).Value();
                m_viewModel.sourceLanguageUnavailable = false;
                return;
            }
            LOG_WARN("editor.source.language", "%s", workspace.ErrorValue().message.c_str());
        } else {
            LOG_INFO("editor.source.language", "%s", provider.ErrorValue().message.c_str());
        }
        m_viewModel.sourceLanguageUnavailable = true;
    }

    /** @copydoc EditorWorkspaceController::UpdateSourceLanguage */
    void EditorWorkspaceController::UpdateSourceLanguage() {
        if (!m_sourceLanguage)
            return;
        auto &diagnostics = m_viewModel.sourceLanguageDiagnostics;
        std::erase_if(diagnostics, [](const auto &entry) {
            return !entry.second->context.source.IsCurrent();
        });
        for (auto &result : m_sourceLanguage->Poll(m_viewModel.sourceDocuments, Language::ProjectLanguageWorkspace::Clock::now())) {
            if (std::holds_alternative<Language::LanguageDiagnostics>(result.payload)) {
                const auto instance = result.context.source.Identity().instance;
                diagnostics.insert_or_assign(instance, std::make_shared<const Language::LanguageResult>(std::move(result)));
            }
        }
        if (const auto *error = m_sourceLanguage->ProviderError(Language::ClangdProviderId);
            error && !m_viewModel.sourceLanguageUnavailable) {
            m_viewModel.sourceLanguageUnavailable = true;
            diagnostics.clear();
            LOG_WARN("editor.source.language", "%s", error->message.c_str());
        }
    }
}  // namespace Horo::Editor
