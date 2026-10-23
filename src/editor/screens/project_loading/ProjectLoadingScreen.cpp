#include "Horo/Editor/DefaultScreenFactories.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/GuiScreenHost.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Editor/ProjectCreationService.h"
#include "Horo/Editor/ProjectOpenService.h"
#include "Horo/Editor/ScreenRegistry.h"
#include "Horo/Foundation/Logging/LogContext.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "ProjectLoadingView.h"
#include "editor/screens/NavigationErrors.h"

#include <memory>
#include <optional>
#include <string>

namespace Horo::Editor {
    namespace {
        /** @brief Returns the localization key associated with a project-open phase. */
        [[nodiscard]] constexpr const char *PhaseLocalizationKey(const ProjectOpenPhase phase) noexcept {
            using enum ProjectOpenPhase;
            switch (phase) {
                case Inspecting:
                    return "project_loading.status.inspecting";
                case CleaningRecovery:
                    return "project_loading.status.cleaning";
                case Recovering:
                    return "project_loading.status.recovering";
                case ValidatingCompatibility:
                    return "project_loading.status.validating";
                case PlanningMigration:
                    return "project_loading.status.planning";
                case Migrating:
                    return "project_loading.status.migrating";
                case UpdatingProjectMetadata:
                    return "project_loading.status.updating";
                case ValidatingDefaultScene:
                    return "project_loading.status.loading_scene";
                case RebuildingDerivedState:
                    return "project_loading.status.rebuilding";
                case RendererPreflight:
                    return "project_loading.status.renderer";
                case PreparingWorkspace:
                    return "project_loading.status.workspace";
                case ReadyToActivate:
                    return "project_loading.status.ready";
                case RequiresRendererRestart:
                    return "project_loading.status.renderer_restart";
                case Failed:
                    return "project_loading.status.failed";
                case Cancelled:
                    return "project_loading.status.cancelling";
            }
            return "project_loading.status.processing";
        }

        class ProjectLoadingScreen final : public GuiScreen {
        public:
            explicit ProjectLoadingScreen(const EditorServiceRegistry &services)
                : host_(services.Get<GuiScreenHost>()), context_(services.GetConst<EditorGuiContext>()),
                  creationService_(services.Get<ProjectCreationService>()), openService_(services.Get<ProjectOpenService>()) {}

            ScreenId Id() const override {
                return static_cast<ScreenId>(GuiRouteKind::ProjectLoading);
            }

            Result<void> OnEnter(const GuiRoute &route) override {
                state_ = {};
                hasPresentedLoadingView_ = false;
                readyStateAwaitingPresentation_ = false;
                readyStatePresented_ = false;
                if (std::holds_alternative<ProjectLoadingRouteParameters>(route.parameters)) {
                    const auto &params = std::get<ProjectLoadingRouteParameters>(route.parameters);
                    state_.projectRoot = params.projectRoot;
                    state_.projectName = params.projectName;
                }
                creationOperation_ = host_.GetActiveCreationId();
                logCtx_ = std::make_unique<Log::LogContext>("project", state_.projectName, "path", state_.projectRoot);
                if (!creationOperation_.has_value())
                    StartOpen();
                return Result<void>::Success();
            }

            void OnUpdate(float) override {
                // Route transitions are committed after the previous screen draws. Keep
                // this route alive until its first draw so fast operations cannot skip
                // the loading feedback entirely.
                if (!hasPresentedLoadingView_)
                    return;
                if (creationOperation_.has_value())
                    UpdateCreation();
                else
                    UpdateOpen();
            }

            void Draw(const GuiContentRegion &contentRegion) override {
                const ProjectLoadingViewCommand command =
                    GuiLive::Invoke<&DrawProjectLoadingView>(context_.theme.liveReload, state_, context_, contentRegion);
                hasPresentedLoadingView_ = true;
                if (readyStateAwaitingPresentation_)
                    readyStatePresented_ = true;
                using enum ProjectLoadingViewCommand;
                switch (command) {
                    case Cancel:
                    case Back:
                        CancelActive();
                        static_cast<void>(host_.Navigate({GuiRouteKind::Welcome, WelcomeRouteParameters{}}));
                        break;
                    case Retry:
                        state_.hasFailed = false;
                        state_.canRetry = false;
                        state_.isCancelled = false;
                        state_.progress = 0.0F;
                        StartOpen();
                        break;
                    case None:
                        break;
                }
            }

            LeaveDecision CanLeave(const LeaveTarget &target) const override {
                using enum LeaveDisposition;
                using enum LeaveAction;
                if (!IsRunning() || std::holds_alternative<GuiRoute>(target.value))
                    return {Allow, std::nullopt};
                if (std::holds_alternative<ApplicationCloseTarget>(target.value))
                    return {RequireResolution,
                            LeaveRequirement{LeaveRequirementKind::RunningOperation, Subject(), 1, {CancelOperation, Stay}}};
                return {Allow, std::nullopt};
            }

            Result<LeaveDecision> ResolveLeave(const LeaveTarget &, const LeaveResolution &resolution) override {
                using enum LeaveDisposition;
                using enum LeaveAction;
                if (resolution.subject != Subject() || resolution.revision != 1)
                    return Result<LeaveDecision>::Failure(MakeError(NavigationErrors::ProjectLoadingStaleLeaveSubject));
                if (resolution.action == CancelOperation) {
                    CancelActive();
                    return Result<LeaveDecision>::Success({Allow, std::nullopt});
                }
                if (resolution.action == Stay)
                    return Result<LeaveDecision>::Success({Deny, std::nullopt});
                return Result<LeaveDecision>::Failure(MakeError(NavigationErrors::ProjectLoadingLeaveActionNotAllowed));
            }

            void OnLeave() override {
                CancelActive();
                logCtx_.reset();
            }

        private:
            void StartOpen() {
                readyStateAwaitingPresentation_ = false;
                readyStatePresented_ = false;
                auto started = openService_.Start(
                    {.projectRoot = state_.projectRoot, .expectedProjectName = state_.projectName, .engineBuildIdentity = "HoroEditor"});
                if (started.HasError()) {
                    state_.hasFailed = true;
                    state_.canRetry = true;
                    state_.statusText = FailureText(started.ErrorValue());
                    return;
                }
                openOperation_.emplace(std::move(started).Value());
                state_.statusText = context_.localization.Get("editor", "project_loading.status.inspecting");
            }

            void UpdateCreation() {
                creationService_.PumpMainThread();
                const auto snapshot = creationService_.Query(*creationOperation_);
                if (!snapshot.has_value())
                    return;
                state_.progress = snapshot->progress * 30.0F;
                state_.statusText = context_.localization.Get("editor", "project_loading.status.creating");
                if (snapshot->state == ProjectCreationOperationState::Succeeded) {
                    host_.SetActiveCreationId(std::nullopt);
                    creationOperation_.reset();
                    StartOpen();
                } else if (snapshot->state == ProjectCreationOperationState::Failed) {
                    state_.hasFailed = true;
                    state_.canRetry = false;
                    state_.statusText = snapshot->error.has_value() ? snapshot->error->message
                                                                    : context_.localization.Get("editor", "project_loading.status.failed");
                }
            }

            void UpdateOpen() {
                if (!openOperation_.has_value())
                    return;
                openService_.PumpOwnerThread();
                const auto snapshot = openService_.Query(openOperation_->Id());
                if (!snapshot.has_value())
                    return;
                state_.progress = snapshot->progress * 100.0F;
                state_.projectName = snapshot->projectName.empty() ? state_.projectName : snapshot->projectName;
                state_.statusText = PhaseText(snapshot->phase);
                if (snapshot->outcome == ProjectOpenOutcome::ReadyToActivate) {
                    // Completion is first projected through the loading view. Workspace
                    // activation happens on the following update, after that state has
                    // participated in a rendered frame.
                    if (!readyStatePresented_) {
                        readyStateAwaitingPresentation_ = true;
                        return;
                    }
                    if (!snapshot->readySession.has_value()) {
                        state_.hasFailed = true;
                        state_.canRetry = true;
                        state_.statusText = context_.localization.Get("editor", "project_loading.status.failed");
                        return;
                    }
                    const auto navigated = host_.Navigate(
                        {GuiRouteKind::EditorWorkspace, EditorWorkspaceRouteParameters{*snapshot->readySession, std::nullopt}});
                    if (navigated.HasError()) {
                        state_.hasFailed = true;
                        state_.canRetry = true;
                        state_.statusText = navigated.ErrorValue().message;
                    }
                } else if (snapshot->outcome == ProjectOpenOutcome::RequiresRendererRestart) {
                    static_cast<void>(
                        host_.RequestRendererRestart({snapshot->requiredRendererBackend, state_.projectRoot, state_.projectName}));
                } else if (snapshot->outcome == ProjectOpenOutcome::Failed) {
                    state_.hasFailed = true;
                    state_.canRetry = true;
                    if (snapshot->diagnostic.has_value())
                        state_.statusText = FailureText(*snapshot->diagnostic);
                } else if (snapshot->outcome == ProjectOpenOutcome::Cancelled) {
                    state_.isCancelled = true;
                }
            }

            [[nodiscard]] std::string PhaseText(const ProjectOpenPhase phase) const {
                return context_.localization.Get("editor", PhaseLocalizationKey(phase));
            }

            [[nodiscard]] std::string FailureText(const Error &error) const {
                const std::string &code = error.code.Value();
                const char *key = "project_loading.status.failed";
                if (code == "project.migration.locked" || code == "project.open.busy")
                    key = "project_loading.error.busy";
                else if (code.find("recovery") != std::string::npos)
                    key = "project_loading.error.recovery";
                else if (code.find("renderer") != std::string::npos)
                    key = "project_loading.error.renderer";
                else if (code.find("derived_state") != std::string::npos || code == "project.open.worker_capacity_insufficient")
                    key = "project_loading.error.derived";
                else if (code.find("scene_preflight") != std::string::npos)
                    key = "project_loading.error.scene";
                else if (code.find("session_stale") != std::string::npos)
                    key = "project_loading.error.session";
                else if (code.find("migration") != std::string::npos)
                    key = "project_loading.error.migration";
                return context_.localization.Get("editor", key);
            }

            void CancelActive() {
                if (creationOperation_.has_value()) {
                    static_cast<void>(creationService_.RequestCancel(*creationOperation_));
                    host_.SetActiveCreationId(std::nullopt);
                }
                if (openOperation_.has_value())
                    static_cast<void>(openService_.RequestCancel(openOperation_->Id()));
            }

            [[nodiscard]] bool IsRunning() const {
                if (creationOperation_.has_value())
                    return true;
                if (!openOperation_.has_value())
                    return false;
                const auto snapshot = openService_.Query(openOperation_->Id());
                return snapshot.has_value() && snapshot->outcome == ProjectOpenOutcome::Running;
            }

            [[nodiscard]] std::uint64_t Subject() const {
                if (creationOperation_.has_value())
                    return *creationOperation_;
                if (openOperation_.has_value())
                    return openOperation_->Id().value;
                return 0;
            }

            GuiScreenHost &host_;
            const EditorGuiContext &context_;
            ProjectCreationService &creationService_;
            ProjectOpenService &openService_;
            ProjectLoadingViewState state_;
            std::optional<ProjectCreationOperationId> creationOperation_;
            std::optional<ProjectOpenOperationHandle> openOperation_;
            std::unique_ptr<Log::LogContext> logCtx_;
            bool hasPresentedLoadingView_ = false;
            bool readyStateAwaitingPresentation_ = false;
            bool readyStatePresented_ = false;
        };
    }  // namespace

    void RegisterProjectLoadingScreen(ScreenRegistry &registry) {
        registry.Register(GuiRouteKind::ProjectLoading, [](const EditorServiceRegistry &services, const GuiRoute &) {
            return std::make_unique<ProjectLoadingScreen>(services);
        });
    }
}  // namespace Horo::Editor
