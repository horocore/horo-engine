#include "EditorWorkspaceController.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "ScriptModuleHostServices.h"
#include "WorkspacePathText.h"
#include "editor/source_editor/HoroScriptModuleLanguage.h"

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

    /** @copydoc EditorWorkspaceController::ConfigureScriptLanguage */
    void EditorWorkspaceController::ConfigureScriptLanguage(std::shared_ptr<Extensions::ScriptModuleHostServices> host) {
        m_scriptLanguageHost = std::move(host);
        if (!m_scriptLanguageHost || m_sourceOpenService.ProjectRoot().empty())
            return;
        auto bindings = Language::LoadScriptModuleSourceBindings(m_sourceOpenService.ProjectRoot());
        if (bindings.HasError()) {
            LOG_WARN("editor.source.language", "%s", bindings.ErrorValue().message.c_str());
            return;
        }
        m_viewModel.sourceScriptBindings = std::move(bindings).Value();
    }

    /** @copydoc EditorWorkspaceController::QueryScriptLanguage */
    Result<Language::LanguageResult> EditorWorkspaceController::QueryScriptLanguage(const SourceLanguageIntent &intent) {
        using Output = Result<Language::LanguageResult>;
        const auto identity = intent.source.Identity();
        const auto *module = Language::FindScriptModuleSourceBinding(m_viewModel.sourceScriptBindings, identity.key.source);
        if (!module || !m_scriptLanguageHost)
            return Output::Failure(MakeError(SourceDocumentErrors::Invalid));
        const auto current = m_sourceOpenService.Documents().Snapshot(identity.instance);
        if (current.HasError() || !current.Value().SharesStateWith(intent.source))
            return Output::Failure(MakeError(SourceDocumentErrors::Stale));
        const auto imports =
            m_scriptLanguageHost->DescribeActiveModuleApis(module->package, module->module, m_scriptLanguageProject.Lease());
        if (imports.HasError())
            return Output::Failure(imports.ErrorValue());
        return Language::QueryHoroScriptImports(intent.source, intent.kind, intent.view.caret, imports.Value());
    }

    /** @copydoc EditorWorkspaceController::RequestScriptLanguage */
    void EditorWorkspaceController::RequestScriptLanguage(const SourceLanguageIntent &intent) {
        m_viewModel.sourceLanguageStatus.reset();
        auto queried = QueryScriptLanguage(intent);
        if (queried.HasError()) {
            m_viewModel.sourceLanguageStatus = {intent.source, SourceLanguageStatusKind::ScriptModuleUnavailable};
            return;
        }
        auto result = std::move(queried).Value();
        if (intent.kind == SourceLanguageRequestKind::Completion) {
            AcceptSourceCompletions(result, intent.view, true);
        } else if (auto *hover = std::get_if<Language::LanguageHover>(&result.payload)) {
            m_viewModel.sourceScriptHover = {intent.source, intent.view, std::move(hover->text)};
        } else {
            m_viewModel.sourceLanguageStatus = {intent.source, SourceLanguageStatusKind::NoSymbol};
        }
    }

    /** @copydoc EditorWorkspaceController::ScriptCompletionCurrent */
    bool EditorWorkspaceController::ScriptCompletionCurrent(const SourceLanguageCompletions &result, const std::size_t index) {
        const auto queried = QueryScriptLanguage({result.source, SourceLanguageRequestKind::Completion, result.view});
        if (queried.HasError())
            return false;
        const auto *current = std::get_if<Language::LanguageCompletions>(&queried.Value().payload);
        return current && std::ranges::find(current->items, result.completions.items[index]) != current->items.end();
    }

    /** @copydoc EditorWorkspaceController::RequestSourceLanguage */
    void EditorWorkspaceController::RequestSourceLanguage(const SourceLanguageIntent &intent) {
        using enum SourceLanguageRequestKind;
        if (!intent.source.IsCurrent() || intent.source.Identity().instance != m_viewModel.workspacePanelHost.ActiveDocument())
            return;
        if (m_sourceLanguage && m_pendingSourceLanguage)
            static_cast<void>(m_sourceLanguage->Cancel(m_pendingSourceLanguage->request));
        m_pendingSourceLanguage.reset();
        m_viewModel.sourceCompletions.reset();
        m_viewModel.sourceScriptHover.reset();
        if (Language::FindScriptModuleSourceBinding(m_viewModel.sourceScriptBindings, intent.source.Identity().key.source)) {
            RequestScriptLanguage(intent);
            return;
        }
        if (!m_sourceLanguage || (intent.kind != Definition && intent.kind != Completion))
            return;
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
    void EditorWorkspaceController::AcceptSourceCompletions(Language::LanguageResult &result, const SourceEditingView &view,
                                                            const bool fromScriptModule) {
        auto *completions = std::get_if<Language::LanguageCompletions>(&result.payload);
        if (!completions || completions->items.empty()) {
            m_viewModel.sourceLanguageStatus = {result.context.source, SourceLanguageStatusKind::NoCompletion};
            return;
        }
        m_viewModel.sourceCompletions = std::make_shared<const SourceLanguageCompletions>(
            SourceLanguageCompletions{std::move(result.context.source), view, std::move(*completions), fromScriptModule});
    }

    /** @copydoc EditorWorkspaceController::ApplySourceCompletion */
    void EditorWorkspaceController::ApplySourceCompletion(const SourceCompletionIntent &intent) {
        const auto &result = intent.result;
        if (!result || result != m_viewModel.sourceCompletions || !result->source.IsCurrent() ||
            result->source.Identity().instance != m_viewModel.workspacePanelHost.ActiveDocument() ||
            intent.index >= result->completions.items.size())
            return;
        m_viewModel.sourceCompletions.reset();
        if (result->fromScriptModule && !ScriptCompletionCurrent(*result, intent.index)) {
            m_viewModel.sourceLanguageStatus = {result->source, SourceLanguageStatusKind::ScriptModuleUnavailable};
            return;
        }
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
        if (m_viewModel.sourceScriptHover && !m_viewModel.sourceScriptHover->source.IsCurrent())
            m_viewModel.sourceScriptHover.reset();
        std::erase_if(m_viewModel.sourceLanguageDiagnostics, [](const auto &entry) {
            return !entry.second->context.source.IsCurrent();
        });
    }

    /** @copydoc EditorWorkspaceController::UpdateSourceLanguage */
    void EditorWorkspaceController::UpdateSourceLanguage() {
        RetireStaleSourceLanguageProjections();
        UpdateScriptSyntax();
        if (!m_sourceLanguage)
            return;
        const auto now = Language::ProjectLanguageWorkspace::Clock::now();
        ExpireSourceLanguageRequest(now);
        for (auto &result : m_sourceLanguage->Poll(m_viewModel.sourceDocuments, now)) {
            if (std::holds_alternative<Language::LanguageDiagnostics>(result.payload))
                PublishSourceLanguageDiagnostics(std::move(result));
            else
                AcceptSourceLanguage(std::move(result));
        }
        if (const auto *error = m_sourceLanguage->ProviderError(Language::ClangdProviderId);
            error && !m_viewModel.sourceLanguageUnavailable) {
            m_viewModel.sourceLanguageUnavailable = true;
            std::erase_if(m_viewModel.sourceLanguageDiagnostics, [this](const auto &entry) {
                const auto path = WorkspaceNativePath(entry.second->context.source.Identity().key.source.Value());
                return m_sourceOpenService.Classify(path).kind == SourceFileKind::NativeSource;
            });
            LOG_WARN("editor.source.language", "%s", error->message.c_str());
        }
    }

    /** @copydoc EditorWorkspaceController::UpdateScriptSyntax */
    void EditorWorkspaceController::UpdateScriptSyntax() {
        if (!m_sourceRecoveryJobs)
            return;
        const auto active = m_viewModel.workspacePanelHost.ActiveDocument();
        const auto tabs = m_viewModel.workspacePanelHost.DocumentTabs();
        const auto found = std::ranges::find_if(tabs, [active](const WorkspaceDocumentTab &tab) {
            return tab.identity.instance == active;
        });
        SourceDocumentSnapshot source;
        if (found != tabs.end() && found->identity.key.kind == DocumentKind::Source) {
            auto captured = m_sourceOpenService.Documents().Snapshot(found->identity.instance);
            if (captured.HasValue())
                source = std::move(captured).Value();
        }
        auto publication = m_scriptSyntax.Poll(*m_sourceRecoveryJobs, m_sourceOpenService, source);
        if (publication)
            PublishSourceLanguageDiagnostics(std::move(*publication));
    }

    /** @copydoc EditorWorkspaceController::PublishSourceLanguageDiagnostics */
    void EditorWorkspaceController::PublishSourceLanguageDiagnostics(Language::LanguageResult result) {
        const auto instance = result.context.source.Identity().instance;
        m_viewModel.sourceLanguageDiagnostics.insert_or_assign(instance,
                                                               std::make_shared<const Language::LanguageResult>(std::move(result)));
    }
}  // namespace Horo::Editor
