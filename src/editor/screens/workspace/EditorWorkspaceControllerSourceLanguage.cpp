#include "EditorWorkspaceController.h"
#include "Horo/Foundation/Logging/Logger.h"

namespace Horo::Editor {
    namespace {
        constexpr auto SourceLanguageDeadline = std::chrono::seconds{10};
    }

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
                m_viewModel.sourceLanguageConfigured = true;
                m_viewModel.sourceLanguageUnavailable = false;
                return;
            }
            LOG_WARN("editor.source.language", "%s", workspace.ErrorValue().message.c_str());
        } else {
            LOG_INFO("editor.source.language", "%s", provider.ErrorValue().message.c_str());
        }
        m_viewModel.sourceLanguageUnavailable = true;
    }

    /** @copydoc EditorWorkspaceController::RequestSourceLanguage */
    void EditorWorkspaceController::RequestSourceLanguage(const SourceLanguageIntent &intent) {
        using enum SourceLanguageRequestKind;
        if (!m_sourceLanguage || !intent.source.IsCurrent() ||
            intent.source.Identity().instance != m_viewModel.workspacePanelHost.ActiveDocument() ||
            (intent.kind != Definition && intent.kind != Completion))
            return;
        if (m_pendingSourceLanguage)
            static_cast<void>(m_sourceLanguage->Cancel(m_pendingSourceLanguage->request));
        m_pendingSourceLanguage.reset();
        m_viewModel.sourceCompletions.reset();
        const auto request = m_sourceLanguage->Request(intent.source, intent.kind, {.offset = intent.view.caret});
        if (request.HasError()) {
            m_viewModel.sourceLanguageStatus = {intent.source, SourceLanguageStatusKind::Refused};
            return;
        }
        m_pendingSourceLanguage =
            PendingSourceLanguage{intent, request.Value(), Language::ProjectLanguageWorkspace::Clock::now() + SourceLanguageDeadline};
        m_viewModel.sourceLanguageStatus = {intent.source, SourceLanguageStatusKind::Waiting};
    }

    /** @copydoc EditorWorkspaceController::ExpireSourceLanguageRequest */
    void EditorWorkspaceController::ExpireSourceLanguageRequest(const Language::ProjectLanguageWorkspace::Clock::time_point now) {
        if (!m_pendingSourceLanguage)
            return;
        const auto &pending = *m_pendingSourceLanguage;
        const auto &source = pending.intent.source;
        const bool stillActive = source.IsCurrent() && source.Identity().instance == m_viewModel.workspacePanelHost.ActiveDocument();
        if (stillActive && now < pending.deadline && !m_viewModel.sourceLanguageUnavailable)
            return;
        static_cast<void>(m_sourceLanguage->Cancel(pending.request));
        if (stillActive)
            m_viewModel.sourceLanguageStatus = {source, SourceLanguageStatusKind::Refused};
        else
            m_viewModel.sourceLanguageStatus.reset();
        m_pendingSourceLanguage.reset();
    }

    /** @copydoc EditorWorkspaceController::FocusSourceDefinition */
    bool EditorWorkspaceController::FocusSourceDefinition(const Language::LanguageLocation &target) {
        const auto path = std::filesystem::u8path(target.source.Value());
        if (m_sourceOpenService.Classify(path).kind != SourceFileKind::NativeSource)
            return false;
        OpenSourceFile({.path = path, .origin = SourceOpenOrigin::Command, .mode = SourceOpenMode::EmbeddedOnly});
        if (!m_viewModel.contentBrowserOperationError.empty())
            return false;
        const auto active = m_viewModel.workspacePanelHost.ActiveDocument();
        if (!active)
            return false;
        const auto source = m_sourceOpenService.Documents().Snapshot(*active);
        if (source.HasError())
            return false;
        const auto selection = Language::ResolveLanguageLocation(source.Value(), target);
        if (!selection)
            return false;
        m_viewModel.sourceLanguageFocus =
            std::make_shared<const Language::LanguageSourceFocus>(Language::LanguageSourceFocus{source.Value(), *selection});
        RefreshSourceDocumentProjection();
        return true;
    }

    /** @copydoc EditorWorkspaceController::AcceptSourceLanguage */
    void EditorWorkspaceController::AcceptSourceLanguage(Language::LanguageResult result) {
        if (!m_pendingSourceLanguage || result.context.kind != m_pendingSourceLanguage->intent.kind ||
            !result.context.source.SharesStateWith(m_pendingSourceLanguage->intent.source))
            return;
        const auto view = m_pendingSourceLanguage->intent.view;
        m_pendingSourceLanguage.reset();
        m_viewModel.sourceLanguageStatus.reset();
        if (std::holds_alternative<Language::LanguageServerFailure>(result.payload)) {
            m_viewModel.sourceLanguageStatus = {result.context.source, SourceLanguageStatusKind::Refused};
            return;
        }
        if (result.context.kind == SourceLanguageRequestKind::Completion) {
            AcceptSourceCompletions(result, view);
            return;
        }
        const auto *navigation = std::get_if<Language::LanguageNavigation>(&result.payload);
        if (!navigation || navigation->targets.empty() || !FocusSourceDefinition(navigation->targets.front()))
            m_viewModel.sourceLanguageStatus = {result.context.source, SourceLanguageStatusKind::NoDefinition};
    }

    /** @copydoc EditorWorkspaceController::AcceptSourceCompletions */
    void EditorWorkspaceController::AcceptSourceCompletions(Language::LanguageResult &result, const SourceEditingView &view) {
        auto *completions = std::get_if<Language::LanguageCompletions>(&result.payload);
        if (!completions || completions->items.empty()) {
            m_viewModel.sourceLanguageStatus = {result.context.source, SourceLanguageStatusKind::NoCompletion};
            return;
        }
        m_viewModel.sourceCompletions = std::make_shared<const SourceLanguageCompletions>(
            SourceLanguageCompletions{std::move(result.context.source), view, std::move(*completions)});
    }

    /** @copydoc EditorWorkspaceController::ApplySourceCompletion */
    void EditorWorkspaceController::ApplySourceCompletion(const SourceCompletionIntent &intent) {
        const auto &result = intent.result;
        if (!result || result != m_viewModel.sourceCompletions || !result->source.IsCurrent() ||
            result->source.Identity().instance != m_viewModel.workspacePanelHost.ActiveDocument() ||
            intent.index >= result->completions.items.size())
            return;
        m_viewModel.sourceCompletions.reset();
        auto completion = Language::PlanLanguageCompletion(result->source, result->view, result->completions.items[intent.index]);
        if (completion.HasError()) {
            m_viewModel.sourceEditingResult = Result<SourceEditingPublication>::Failure(std::move(completion).ErrorValue());
            return;
        }
        m_viewModel.sourceEditingResult =
            SubmitSourceEditingCommand(m_sourceOpenService.Documents(),
                                       SourceEditSubmission{result->source.Identity().instance, std::move(completion).Value()});
    }

    /** @copydoc EditorWorkspaceController::RetireStaleSourceLanguageProjections */
    void EditorWorkspaceController::RetireStaleSourceLanguageProjections() {
        if (m_viewModel.sourceLanguageStatus && !m_viewModel.sourceLanguageStatus->source.IsCurrent())
            m_viewModel.sourceLanguageStatus.reset();
        if (m_viewModel.sourceLanguageFocus && !m_viewModel.sourceLanguageFocus->source.IsCurrent())
            m_viewModel.sourceLanguageFocus.reset();
        if (m_viewModel.sourceCompletions && !m_viewModel.sourceCompletions->source.IsCurrent())
            m_viewModel.sourceCompletions.reset();
        std::erase_if(m_viewModel.sourceLanguageDiagnostics, [](const auto &entry) {
            return !entry.second->context.source.IsCurrent();
        });
    }

    /** @copydoc EditorWorkspaceController::UpdateSourceLanguage */
    void EditorWorkspaceController::UpdateSourceLanguage() {
        if (!m_sourceLanguage)
            return;
        const auto now = Language::ProjectLanguageWorkspace::Clock::now();
        ExpireSourceLanguageRequest(now);
        RetireStaleSourceLanguageProjections();
        auto &diagnostics = m_viewModel.sourceLanguageDiagnostics;
        for (auto &result : m_sourceLanguage->Poll(m_viewModel.sourceDocuments, now)) {
            if (std::holds_alternative<Language::LanguageDiagnostics>(result.payload)) {
                const auto instance = result.context.source.Identity().instance;
                diagnostics.insert_or_assign(instance, std::make_shared<const Language::LanguageResult>(std::move(result)));
            } else {
                AcceptSourceLanguage(std::move(result));
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
