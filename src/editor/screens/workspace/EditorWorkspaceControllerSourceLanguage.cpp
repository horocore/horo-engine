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
        if (m_sourceOpenService.ProjectRoot().empty())
            return;
        auto bindings = Language::LoadScriptModuleSourceBindings(m_sourceOpenService.ProjectRoot());
        if (bindings.HasError()) {
            LOG_WARN("editor.source.language", "%s", bindings.ErrorValue().message.c_str());
            return;
        }
        m_viewModel.sourceScriptBindings = std::move(bindings).Value();
    }

    /** @copydoc EditorWorkspaceController::CaptureScriptLanguageImports */
    Result<std::vector<Extensions::ScriptModuleApiMetadata>> EditorWorkspaceController::CaptureScriptLanguageImports(
        const SourceDocumentSnapshot &source) {
        using Output = Result<std::vector<Extensions::ScriptModuleApiMetadata>>;
        const auto identity = source.Identity();
        const auto *module = Language::FindScriptModuleSourceBinding(m_viewModel.sourceScriptBindings, identity.key.source);
        if (!module || !m_scriptLanguageHost)
            return Output::Failure(MakeError(SourceDocumentErrors::Invalid));
        return m_scriptLanguageHost->DescribeActiveModuleApis(module->package, module->module, m_scriptLanguageProject.Lease());
    }

    /** @copydoc EditorWorkspaceController::QueryScriptLanguage */
    Result<Language::LanguageResult> EditorWorkspaceController::QueryScriptLanguage(const SourceLanguageIntent &intent) {
        using Output = Result<Language::LanguageResult>;
        const auto current = m_sourceOpenService.Documents().Snapshot(intent.source.Identity().instance);
        if (current.HasError() || !current.Value().SharesStateWith(intent.source))
            return Output::Failure(MakeError(SourceDocumentErrors::Stale));
        const auto imports = CaptureScriptLanguageImports(intent.source);
        const auto available = imports.HasValue() ? std::span<const Extensions::ScriptModuleApiMetadata>{imports.Value()}
                                                  : std::span<const Extensions::ScriptModuleApiMetadata>{};
        auto result = Language::QueryHoroScriptApi(intent.source, intent.kind, intent.view.caret, available);
        if (imports.HasError() && result.HasValue() && std::holds_alternative<std::monostate>(result.Value().payload))
            return Output::Failure(imports.ErrorValue());
        return result;
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
        } else {
            AcceptSourceHover(result, intent.view, true);
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
        m_viewModel.sourceReferences.reset();
        m_viewModel.sourceSymbols.reset();
        m_viewModel.sourceCodeActions.reset();
        m_viewModel.sourceCodeActionPreview.reset();
        m_viewModel.sourceLanguageHover.reset();
        if (Language::FindScriptModuleSourceBinding(m_viewModel.sourceScriptBindings, intent.source.Identity().key.source)) {
            RequestScriptLanguage(intent);
            return;
        }
        if (!m_sourceLanguage || (intent.kind != Definition && intent.kind != Completion && intent.kind != References &&
                                  intent.kind != Hover && intent.kind != Symbols && intent.kind != CodeActions))
            return;
        Language::LspSourceQuery selection{.offset = intent.view.caret};
        if (intent.kind == CodeActions) {
            const auto [start, end] = std::minmax(intent.view.anchor, intent.view.caret);
            selection = {.offset = start, .length = end - start};
        }
        const auto request = m_sourceLanguage->Request(intent.source, intent.kind, selection);
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

    /** @copydoc EditorWorkspaceController::FocusSourceLocation */
    bool EditorWorkspaceController::FocusSourceLocation(const Language::LanguageLocation &target) {
        const auto path = std::filesystem::u8path(target.source.Value());
        if (m_sourceOpenService.Classify(path).kind != SourceFileKind::NativeSource)
            return false;
        return PresentSourceCommand(target.source, target, [] {
            return false;
        }).HasValue();
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
        if (result.context.kind == SourceLanguageRequestKind::Hover) {
            AcceptSourceHover(result, view);
            return;
        }
        if (result.context.kind == SourceLanguageRequestKind::Symbols) {
            AcceptSourceSymbols(result);
            return;
        }
        if (result.context.kind == SourceLanguageRequestKind::CodeActions) {
            AcceptSourceCodeActions(result, view);
            return;
        }
        auto *navigation = std::get_if<Language::LanguageNavigation>(&result.payload);
        if (result.context.kind == SourceLanguageRequestKind::References) {
            if (!navigation || navigation->targets.empty()) {
                m_viewModel.sourceLanguageStatus = {result.context.source, SourceLanguageStatusKind::NoReferences};
                return;
            }
            m_viewModel.sourceReferences = std::make_shared<const SourceLanguageReferences>(
                SourceLanguageReferences{std::move(result.context.source), view, std::move(*navigation)});
            return;
        }
        if (!navigation || navigation->targets.empty() || !FocusSourceLocation(navigation->targets.front()))
            m_viewModel.sourceLanguageStatus = {result.context.source, SourceLanguageStatusKind::NoDefinition};
    }

    /** @copydoc EditorWorkspaceController::AcceptSourceCodeActions */
    void EditorWorkspaceController::AcceptSourceCodeActions(Language::LanguageResult &result, const SourceEditingView &view) {
        auto *actions = std::get_if<Language::LanguageCodeActions>(&result.payload);
        if (actions) {
            std::erase_if(actions->items, [](const Language::LanguageCodeAction &action) {
                return action.edits.empty();
            });
        }
        if (!actions || actions->items.empty()) {
            m_viewModel.sourceLanguageStatus = {result.context.source, SourceLanguageStatusKind::NoCodeActions};
            return;
        }
        m_viewModel.sourceCodeActions = std::make_shared<const SourceLanguageCodeActions>(
            SourceLanguageCodeActions{std::move(result.context.source), view, std::move(*actions)});
    }

    /** @copydoc EditorWorkspaceController::ReviewSourceCodeAction */
    void EditorWorkspaceController::ReviewSourceCodeAction(const SourceCodeActionIntent &intent) {
        const auto &result = intent.result;
        if (!result || result != m_viewModel.sourceCodeActions || !result->source.IsCurrent() ||
            result->source.Identity().instance != m_viewModel.workspacePanelHost.ActiveDocument() ||
            intent.index >= result->actions.items.size())
            return;
        if (intent.decision == SourceCodeActionDecision::Preview) {
            m_viewModel.sourceCodeActionPreview = intent;
            return;
        }
        const auto &preview = m_viewModel.sourceCodeActionPreview;
        if (!preview || preview->result != result || preview->index != intent.index)
            return;
        m_viewModel.sourceCodeActionPreview.reset();
        if (intent.decision != SourceCodeActionDecision::Apply)
            return;
        m_viewModel.sourceCodeActions.reset();
        auto plan = Language::PlanLanguageCodeAction(result->source, result->view, result->actions.items[intent.index]);
        if (plan.HasError()) {
            m_viewModel.sourceEditingResult = Result<SourceEditingPublication>::Failure(std::move(plan).ErrorValue());
            return;
        }
        m_viewModel.sourceEditingResult =
            SubmitSourceEditingCommand(m_sourceOpenService.Documents(),
                                       SourceEditSubmission{result->source.Identity().instance, std::move(plan).Value()});
    }

    /** @copydoc EditorWorkspaceController::AcceptSourceSymbols */
    void EditorWorkspaceController::AcceptSourceSymbols(Language::LanguageResult &result) {
        auto *symbols = std::get_if<Language::LanguageSymbols>(&result.payload);
        if (!symbols || symbols->items.empty()) {
            m_viewModel.sourceLanguageStatus = {result.context.source, SourceLanguageStatusKind::NoDocumentSymbols};
            return;
        }
        m_viewModel.sourceSymbols =
            std::make_shared<const SourceLanguageSymbols>(SourceLanguageSymbols{std::move(result.context.source), std::move(*symbols)});
    }

    /** @copydoc EditorWorkspaceController::SelectSourceSymbol */
    void EditorWorkspaceController::SelectSourceSymbol(const SourceSymbolIntent &intent) {
        const auto &result = intent.result;
        if (!result || result != m_viewModel.sourceSymbols || !result->source.IsCurrent() ||
            result->source.Identity().instance != m_viewModel.workspacePanelHost.ActiveDocument() ||
            intent.index >= result->symbols.items.size())
            return;
        m_viewModel.sourceLanguageFocus = std::make_shared<const Language::LanguageSourceFocus>(
            Language::LanguageSourceFocus{result->source, result->symbols.items[intent.index].selection});
    }

    /** @copydoc EditorWorkspaceController::AcceptSourceHover */
    void EditorWorkspaceController::AcceptSourceHover(Language::LanguageResult &result, const SourceEditingView &view,
                                                      const bool fromScriptModule) {
        auto *hover = std::get_if<Language::LanguageHover>(&result.payload);
        if (!hover || hover->text.empty()) {
            m_viewModel.sourceLanguageStatus = {result.context.source, SourceLanguageStatusKind::NoSymbol};
            return;
        }
        m_viewModel.sourceLanguageHover = {std::move(result.context.source), view, std::move(hover->text), fromScriptModule};
    }

    /** @copydoc EditorWorkspaceController::OpenSourceReference */
    void EditorWorkspaceController::OpenSourceReference(const SourceReferenceIntent &intent) {
        const auto &result = intent.result;
        if (!result || result != m_viewModel.sourceReferences || !result->source.IsCurrent() ||
            result->source.Identity().instance != m_viewModel.workspacePanelHost.ActiveDocument() ||
            intent.index >= result->navigation.targets.size())
            return;
        if (!FocusSourceLocation(result->navigation.targets[intent.index]))
            m_viewModel.sourceLanguageStatus = {result->source, SourceLanguageStatusKind::Refused};
        else
            m_viewModel.sourceLanguageStatus.reset();
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
        if (m_viewModel.sourceReferences && !m_viewModel.sourceReferences->source.IsCurrent())
            m_viewModel.sourceReferences.reset();
        if (m_viewModel.sourceSymbols && !m_viewModel.sourceSymbols->source.IsCurrent())
            m_viewModel.sourceSymbols.reset();
        if (m_viewModel.sourceCodeActions && !m_viewModel.sourceCodeActions->source.IsCurrent()) {
            m_viewModel.sourceCodeActionPreview.reset();
            m_viewModel.sourceCodeActions.reset();
        }
        if (m_viewModel.sourceLanguageHover && !m_viewModel.sourceLanguageHover->source.IsCurrent())
            m_viewModel.sourceLanguageHover.reset();
        std::erase_if(m_viewModel.sourceLanguageDiagnostics, [](const auto &entry) {
            return !entry.second->context.source.IsCurrent();
        });
    }

    /** @copydoc EditorWorkspaceController::UpdateSourceLanguage */
    void EditorWorkspaceController::UpdateSourceLanguage() {
        RetireStaleSourceLanguageProjections();
        UpdateScriptLanguageRevision();
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

    /** @copydoc EditorWorkspaceController::UpdateScriptLanguageRevision */
    void EditorWorkspaceController::UpdateScriptLanguageRevision() {
        const auto revision = ScriptAuthoringApiRevision();
        if (revision == m_scriptLanguageApiRevision)
            return;
        m_scriptLanguageApiRevision = revision;
        if (m_viewModel.sourceLanguageHover && m_viewModel.sourceLanguageHover->fromScriptModule)
            m_viewModel.sourceLanguageHover.reset();
        if (m_viewModel.sourceCompletions && m_viewModel.sourceCompletions->fromScriptModule)
            m_viewModel.sourceCompletions.reset();
        std::erase_if(m_viewModel.sourceLanguageDiagnostics, [](const auto &entry) {
            const auto *diagnostics = std::get_if<Language::LanguageDiagnostics>(&entry.second->payload);
            return diagnostics && std::ranges::any_of(diagnostics->items, [](const Language::LanguageDiagnostic &item) {
                return item.code == Language::LanguageDiagnosticCode::UnknownScriptImportMember;
            });
        });
    }

    /** @copydoc EditorWorkspaceController::ScriptAuthoringApiRevision */
    std::uint64_t EditorWorkspaceController::ScriptAuthoringApiRevision() const noexcept {
        return m_scriptLanguageHost ? m_scriptLanguageHost->AuthoringApiRevision() : 0;
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
        auto publication = m_scriptSyntax.Poll(*m_sourceRecoveryJobs, m_sourceOpenService, source, m_scriptLanguageApiRevision,
                                               [this](const SourceDocumentSnapshot &current) {
            auto imports = CaptureScriptLanguageImports(current);
            return imports.HasValue() ? std::move(imports).Value() : std::vector<Extensions::ScriptModuleApiMetadata>{};
        });
        if (publication)
            PublishScriptDiagnostics(std::move(*publication));
    }

    /** @copydoc EditorWorkspaceController::PublishScriptDiagnostics */
    void EditorWorkspaceController::PublishScriptDiagnostics(Language::LanguageResult publication) {
        if (ScriptAuthoringApiRevision() != m_scriptLanguageApiRevision)
            return;
        for (auto &item : std::get<Language::LanguageDiagnostics>(publication.payload).items) {
            if (item.code == Language::LanguageDiagnosticCode::UnknownScriptImportMember) {
                item.message = Localized("workspace.source_language.unknown_script_member",
                                         "The registered import schema does not contain this member.");
            }
        }
        PublishSourceLanguageDiagnostics(std::move(publication));
    }

    /** @copydoc EditorWorkspaceController::PublishSourceLanguageDiagnostics */
    void EditorWorkspaceController::PublishSourceLanguageDiagnostics(Language::LanguageResult result) {
        const auto instance = result.context.source.Identity().instance;
        m_viewModel.sourceLanguageDiagnostics.insert_or_assign(instance,
                                                               std::make_shared<const Language::LanguageResult>(std::move(result)));
    }
}  // namespace Horo::Editor
