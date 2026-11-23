#include "EditorBuildCommandEndpoint.h"
#include "EditorInspectionCommandEndpoint.h"
#include "EditorSourcePresentation.h"
#include "EditorTestCommandEndpoint.h"
#include "EditorWorkspaceController.h"

#include <algorithm>

namespace Horo::Editor {
    namespace {
        const ErrorCodeDescriptor BuildAwaitingPlay{
            .domain = ErrorDomainId{"editor.gameplay"},
            .code = ErrorCode{"play_waits_for_build"},
            .defaultSeverity = ErrorSeverity::Info,
            .summary = "Play still owns a pending gameplay build result.",
        };

        using Validation = Proposals::SourceReviewValidation;

        /** @brief Projects rejection of captured evidence independently from a compiler or test failure. */
        bool StaleValidation(const Error &error) {
            const auto matches = [&error](const ErrorCodeDescriptor &descriptor) {
                return ErrorChainContains(error, descriptor.domain, descriptor.code);
            };
            static const ErrorDomainId TestDomain{"gameplay.test"};
            static const ErrorCode TestInputsStale{"build_inputs_stale"};
            static const ErrorDomainId BuildDomain{"horo.application.gameplay_build"};
            static const ErrorCode BuildInputsStale{"validation_inputs_changed"};
            return matches(Proposals::Errors::StaleBase) || matches(SourceDocumentErrors::Stale) ||
                   matches(SourceDocumentErrors::SaveConflict) || ErrorChainContains(error, TestDomain, TestInputsStale) ||
                   ErrorChainContains(error, BuildDomain, BuildInputsStale);
        }

        /** @brief Converts exact native build state to the panel's small presentation state. */
        Validation BuildValidationState(const std::optional<Application::GameplayBuildSnapshot> &build) {
            using enum Application::GameplayBuildState;
            if (!build || (build->error && StaleValidation(*build->error)))
                return Validation::Stale;
            switch (build->state) {
                case Succeeded:
                    return Validation::BuildPassed;
                case Cancelled:
                case Failed:
                case TimedOut:
                    return Validation::BuildFailed;
                default:
                    return Validation::Building;
            }
        }

        /** @brief Converts exact native test state to the panel's small presentation state. */
        Validation TestValidationState(const std::optional<Application::GameplayTestSnapshot> &test) {
            if (!test || (test->error && StaleValidation(*test->error)))
                return Validation::Stale;
            switch (test->state) {
                case OperationState::Succeeded:
                    return Validation::TestsPassed;
                case OperationState::Failed:
                case OperationState::Cancelled:
                    return Validation::TestsFailed;
                default:
                    return Validation::Testing;
            }
        }
    }  // namespace

    /** @copydoc EditorWorkspaceController::ReadSourceReviewStatus */
    Proposals::SourceTransactionStatus EditorWorkspaceController::ReadSourceReviewStatus() const {
        auto status = m_sourceReviews.Status();
        if (!status.audit || status.feedback == Proposals::SourceTransactionFeedback::Preparing || !m_gameplayBuilds || !m_durableFiles ||
            m_gameplayBuildEnvironment.gameplaySdkPackage.empty())
            return status;
        if (status.disposition == Proposals::SourceReviewDisposition::Applied) {
            status.validation = Validation::SaveRequired;
            return status;
        }
        const bool sameRun = m_sourceValidation && m_sourceValidation->evidence->audit == status.audit;
        if (status.disposition != Proposals::SourceReviewDisposition::Saved) {
            if (sameRun)
                status.validation = Validation::Stale;
            return status;
        }
        status.validation = Validation::Ready;
        if (!sameRun)
            return status;
        const auto &run = *m_sourceValidation;
        if (!std::ranges::all_of(run.evidence->sources, &SourceDocumentSnapshot::IsCurrent)) {
            status.validation = Validation::Stale;
            return status;
        }
        if (run.test)
            status.validation = TestValidationState(m_gameplayTests->Query(*run.test));
        else if (run.refused != Validation::Unavailable)
            status.validation = run.refused;
        else
            status.validation = BuildValidationState(m_gameplayBuilds->Query(*run.build));
        status.canRunTests = m_gameplayTests && !m_gameplayTestEnvironment.ctest.empty() &&
                             (status.validation == Validation::BuildPassed || status.validation == Validation::TestsFailed);
        return status;
    }

    /** @copydoc EditorWorkspaceController::RunSourceReviewValidation */
    void EditorWorkspaceController::RunSourceReviewValidation(const EditorWorkspaceViewCommandData &command) {
        using enum Validation;
        const auto status = ReadSourceReviewStatus();
        if (!status.audit || command.sourceReviewValidationAudit != status.audit || command.sourceReviewLifetime.IsCancellationRequested())
            return;
        const auto stop = [&command] {
            return command.sourceReviewLifetime.IsCancellationRequested();
        };
        if (command.command == EditorWorkspaceViewCommand::TestSourceReview) {
            if (status.canRunTests)
                StartSourceReviewTests(stop);
            return;
        }
        if ((status.validation == Ready || status.validation == BuildFailed) && !m_playAfterGameplayBuild)
            StartSourceReviewBuild(status.audit, stop);
    }

    /** @copydoc EditorWorkspaceController::StartSourceReviewBuild */
    void EditorWorkspaceController::StartSourceReviewBuild(const std::shared_ptr<const Proposals::SourceReviewAudit> &audit,
                                                           const std::function<bool()> &stop) {
        using enum Validation;
        RevokeSourceReviewValidation();
        auto captured = m_sourceReviews.CaptureSavedReview(m_sourceOpenService.Documents());
        if (captured.HasError()) {
            m_sourceValidation =
                SourceValidationRun{.evidence = std::make_shared<const SavedSourceReview>(SavedSourceReview{.audit = audit}),
                                    .refused = Stale};
            return;
        }
        auto evidence = std::make_shared<const SavedSourceReview>(std::move(captured).Value());
        auto request = MakeGameplayBuildRequest();
        request.validateSources = [evidence, root = request.projectRoot, files = m_durableFiles](const CancellationToken &cancel) {
            return evidence->VerifyDisk(root, *files, cancel);
        };
        const auto started = m_gameplayBuilds->StartIsolated(request, stop);
        const auto build = started.HasValue() ? std::optional{started.Value()} : std::nullopt;
        auto refusal = Unavailable;
        if (started.HasError())
            refusal = StaleValidation(started.ErrorValue()) ? Stale : BuildFailed;
        m_sourceValidation = SourceValidationRun{std::move(evidence), std::move(request), build, {}, refusal};
    }

    /** @copydoc EditorWorkspaceController::StartSourceReviewTests */
    void EditorWorkspaceController::StartSourceReviewTests(const std::function<bool()> &stop) {
        using enum Validation;
        auto &run = *m_sourceValidation;
        const auto build = m_gameplayBuilds->Query(*run.build);
        if (!build || build->state != Application::GameplayBuildState::Succeeded)
            return;
        if (const auto started = m_gameplayTests->StartForBuild({run.request, build->activeInputHash}, m_gameplayTestEnvironment, stop);
            started.HasValue()) {
            run.test = started.Value();
            run.refused = Unavailable;
        } else {
            run.test.reset();
            run.refused = StaleValidation(started.ErrorValue()) ? Stale : TestsFailed;
        }
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

    /** @copydoc EditorWorkspaceController::BindGameplayTestCommands */
    std::weak_ptr<const EditorTestCommandEndpoint> EditorWorkspaceController::BindGameplayTestCommands(
        std::string project, const std::uint64_t authorizationRevision) {
        RevokeGameplayTestCommands();
        if (!m_gameplayTests || m_gameplayTestEnvironment.ctest.empty())
            return {};
        m_testCommandEndpoint = std::make_shared<const EditorTestCommandEndpoint>(
            EditorTestCommandEndpoint{std::move(project), authorizationRevision, std::this_thread::get_id(),
                                      [this](const std::function<bool()> &stop) {
            auto started = m_gameplayTests->Start(ProjectRoot(), m_gameplayTestEnvironment, stop);
            if (started.HasValue())
                m_commandTestOperation = started.Value();
            return started;
        }, [this](const OperationId id) {
            return m_commandTestOperation == id ? m_gameplayTests->Query(id) : std::nullopt;
        }, [this](const OperationId id) {
            return m_commandTestOperation == id && m_gameplayTests->RequestCancel(id);
        }});
        return m_testCommandEndpoint;
    }

    /** @copydoc EditorWorkspaceController::RevokeGameplayTestCommands */
    void EditorWorkspaceController::RevokeGameplayTestCommands() noexcept {
        m_testCommandEndpoint.reset();
        if (m_commandTestOperation) {
            static_cast<void>(m_gameplayTests->RequestCancel(*m_commandTestOperation));
            m_commandTestOperation.reset();
        }
    }

    /** @copydoc EditorWorkspaceController::RevokeWorkspaceCommands */
    void EditorWorkspaceController::RevokeWorkspaceCommands() noexcept {
        if (m_agentSceneReview)
            m_agentSceneReview->lifetime.RequestCancellation();
        m_sourceCommandEndpoint.reset();
        m_inspectionCommandEndpoint.reset();
        m_debugCaptureLifetime.RequestCancellation();
        m_debugCaptureEndpoint.reset();
        m_debugCapture.Clear();
        m_debugCaptureAuthorized = {};
        m_debugCaptureEntity.reset();
        RevokeGameplayTestCommands();
        m_buildCommandEndpoint.reset();
        if (m_commandBuildSession) {
            static_cast<void>(m_gameplayBuilds->RequestCancel(*m_commandBuildSession));
            m_commandBuildSession.reset();
        }
        RevokeSourceReviewValidation();
    }

    /** @copydoc EditorWorkspaceController::RevokeSourceReviewValidation */
    void EditorWorkspaceController::RevokeSourceReviewValidation() noexcept {
        if (m_sourceValidation) {
            if (m_sourceValidation->test)
                static_cast<void>(m_gameplayTests->RequestCancel(*m_sourceValidation->test));
            if (m_sourceValidation->build)
                static_cast<void>(m_gameplayBuilds->RequestCancel(*m_sourceValidation->build));
            m_sourceValidation.reset();
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
