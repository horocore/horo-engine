#include "Horo/Editor/DefaultScreenFactories.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorServiceRegistry.h"
#include "Horo/Editor/GuiScreenHost.h"
#include "Horo/Editor/ProjectCreationController.h"
#include "Horo/Editor/ProjectCreationService.h"
#include "Horo/Editor/RecentProject.h"
#include "Horo/Editor/ScreenRegistry.h"
#include "Horo/Editor/WelcomeController.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Runtime/Input.h"
#include "ProjectCreationView.h"
#include "editor/project_model/RendererAvailability.h"
#include "editor/screens/NavigationErrors.h"

#include <memory>
#include <vector>

namespace Horo::Editor {
    namespace {
        class ProjectCreationScreen final : public GuiScreen {
        public:
            /** @brief Catalog scope shared by this surface and its view helpers. */
            static constexpr GuiLocalizationScope kTextScope = ProjectCreationTextScope;

            explicit ProjectCreationScreen(const EditorServiceRegistry &services)
                : GuiScreen(kTextScope), host_(services.Get<GuiScreenHost>()), context_(services.GetConst<EditorGuiContext>()),
                  creationService_(services.Get<ProjectCreationService>()), inputRouter_(services.Get<Input::InputRouter>()),
                  rendererAvailability_(services.GetConst<RendererAvailabilitySnapshot>()),
                  logoTexture_(services.TryGet<std::uintptr_t>() ? *services.TryGet<std::uintptr_t>() : 0),
                  controller_(rendererAvailability_) {}

            ScreenId Id() const override {
                return static_cast<ScreenId>(GuiRouteKind::ProjectCreation);
            }

            Result<void> OnEnter(const GuiRoute &route) override {
                controller_ = ProjectCreationController{rendererAvailability_};
                state_ = ProjectCreationViewState{};
                if (std::holds_alternative<ProjectCreationRouteParameters>(route.parameters)) {
                    const auto &params = std::get<ProjectCreationRouteParameters>(route.parameters);
                    if (params.initialTemplate.has_value()) {
                        controller_.SetTemplateId(*params.initialTemplate);
                    }
                }
                leaveResolved_ = false;
                LOG_DEBUG("editor.screens", "ProjectCreationScreen entered.");
                return Result<void>::Success();
            }

            void OnUpdate(float) override {
                // No continuous simulation needed during manual project creation configuration.
            }

            void Draw(const GuiContentRegion &contentRegion) override {
                const auto command =
                    GuiLive::Invoke<&DrawProjectCreationView>(context_.theme.liveReload, controller_, state_, context_, inputRouter_,
                                                              rendererAvailability_, contentRegion, logoTexture_);
                if (command == ProjectCreationViewCommand::ReturnToWelcome) {
                    static_cast<void>(host_.Navigate(GuiRoute{GuiRouteKind::Welcome, WelcomeRouteParameters{}}));
                } else if (command == ProjectCreationViewCommand::CreateProject) {
                    const auto request = controller_.BuildCreationRequest();
                    if (!request) {
                        LOG_ERROR("editor.project_creation", "BuildCreationRequest failed due to validation errors.");
                        for (const auto &diagnostic : controller_.Validate().diagnostics) {
                            LOG_ERROR("editor.project_creation", " - %s", diagnostic.message.c_str());
                        }
                        return;
                    }
                    auto handle = creationService_.StartCreate(*request);
                    if (!handle.HasValue()) {
                        LOG_ERROR("editor.project_creation", "StartCreate failed: [%s] %s", handle.ErrorValue().code.Value().c_str(),
                                  handle.ErrorValue().message.c_str());
                        return;
                    }
                    LOG_INFO("editor.project_creation", "Dispatched async creation job for '%s' (operationId=%llu)",
                             request->projectName.c_str(), static_cast<unsigned long long>(handle.Value().id));

                    leaveResolved_ = true;
                    host_.SetActiveCreationId(handle.Value().id);

                    auto recent = LoadRecentProjectsFromDisk();
                    std::erase_if(recent, [&request](const RecentProjectEntry &entry) {
                        return entry.rootPath == request->projectRoot.string();
                    });
                    recent.emplace(recent.begin(), request->projectName, request->projectRoot.string(), "Just now", request->templateId);
                    SaveRecentProjectsToDisk(recent);

                    static_cast<void>(
                        host_.Navigate(GuiRoute{GuiRouteKind::ProjectLoading,
                                                ProjectLoadingRouteParameters{request->projectRoot.string(), request->projectName}}));
                }
            }

            LeaveDecision CanLeave(const LeaveTarget &target) const override {
                using enum LeaveDisposition;
                using enum LeaveAction;

                if (leaveResolved_) {
                    return LeaveDecision{.disposition = Allow, .requirement = std::nullopt};
                }
                if (const bool hasDraftChanges = !state_.projectName.empty() || !state_.projectPath.empty() || state_.step > 1;
                    hasDraftChanges &&
                    (std::holds_alternative<ApplicationCloseTarget>(target.value) ||
                     (std::holds_alternative<GuiRoute>(target.value) && std::get<GuiRoute>(target.value).kind == GuiRouteKind::Welcome))) {
                    LeaveRequirement req{.kind = LeaveRequirementKind::UnsavedDraft,
                                         .subject = 1,
                                         .revision = 1,
                                         .allowedActions = {Discard, Stay}};
                    return LeaveDecision{.disposition = RequireResolution, .requirement = req};
                }
                return LeaveDecision{.disposition = Allow, .requirement = std::nullopt};
            }

            Result<LeaveDecision> ResolveLeave(const LeaveTarget &, const LeaveResolution &resolution) override {
                using enum LeaveDisposition;
                using enum LeaveAction;

                if (resolution.subject != 1 || resolution.revision != 1) {
                    return Result<LeaveDecision>::Failure(MakeError(NavigationErrors::ProjectCreationStaleLeaveSubject));
                }
                if (resolution.action == Discard) {
                    leaveResolved_ = true;
                    return Result<LeaveDecision>::Success(LeaveDecision{.disposition = Allow, .requirement = std::nullopt});
                }
                if (resolution.action == Stay) {
                    return Result<LeaveDecision>::Success(LeaveDecision{.disposition = Deny, .requirement = std::nullopt});
                }
                return Result<LeaveDecision>::Failure(MakeError(NavigationErrors::ProjectCreationLeaveActionNotAllowed));
            }

            void OnLeave() override {
                LOG_DEBUG("editor.screens", "ProjectCreationScreen leaving.");
            }

        private:
            GuiScreenHost &host_;
            const EditorGuiContext &context_;
            ProjectCreationService &creationService_;
            Input::InputRouter &inputRouter_;
            const RendererAvailabilitySnapshot &rendererAvailability_;
            std::uintptr_t logoTexture_;
            ProjectCreationController controller_;
            ProjectCreationViewState state_;
            mutable bool leaveResolved_{false};
        };
    }  // namespace

    void RegisterProjectCreationScreen(ScreenRegistry &registry) {
        registry.Register(GuiRouteKind::ProjectCreation, [](const EditorServiceRegistry &services, const GuiRoute &) {
            return std::make_unique<ProjectCreationScreen>(services);
        });
    }
}  // namespace Horo::Editor
