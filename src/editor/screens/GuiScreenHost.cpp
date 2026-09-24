#include "Horo/Editor/GuiScreenHost.h"

#include "Horo/Editor/AssetImportModal.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Editor/ProjectCreationService.h"
#include "Horo/Editor/SettingsModal.h"
#include "Horo/Extensions/ExtensionInventory.h"
#include "Horo/Extensions/ExtensionManager.h"
#include "Horo/Extensions/ExtensionMarketplace.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/OperationStore.h"
#include "Horo/Runtime/Input.h"
#include "NavigationErrors.h"
#include "editor/project_model/RendererAvailability.h"
#include "editor/status_bar/EditorStatusBar.h"
#include "editor/ui_preview/EditorUiPreviewCatalog.h"
#include "editor/ui_preview/EditorUiPreviewGallery.h"
#include "runtime/assets/importer/builtin/obj_mesh/ObjMeshImporter.h"

#include <algorithm>
#include <imgui.h>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] const char *GetLeaveActionLabel(LeaveAction action) noexcept {
            using enum LeaveAction;
            switch (action) {
                case Discard:
                    return "Discard & Leave";
                case CancelOperation:
                    return "Cancel Operation";
                case Save:
                    return "Save & Leave";
                case Stay:
                    return "Stay Here";
                default:
                    return "Action";
            }
        }

        /** @brief Registers built-in importers and publishes their typed activation outcome. */
        void ActivateBuiltInImporters(Assets::AssetImporterCatalog &catalog, Extensions::ExtensionInventory *inventory) {
            if (inventory != nullptr && !inventory->IsEnabled("horo.builtin.assets"))
                return;

            const Result<void> registered = RegisterAllBuiltinImporters(catalog);
            if (inventory == nullptr)
                return;
            if (registered.HasError()) {
                static_cast<void>(inventory->RecordActivationFailure("horo.builtin.assets",
                                                                     Extensions::ExtensionActivationFailureReason::ContributionRejected,
                                                                     registered.ErrorValue().message));
                return;
            }
            if (auto activated = inventory->MarkRuntimeActive("horo.builtin.assets"); activated.HasError())
                LOG_ERROR("editor.extensions", "Built-in extension activation state was rejected.");
        }

        /** @brief Loads eligible user extensions and publishes their typed activation outcomes. */
        void ActivateUserExtensions(Extensions::ExtensionManager &manager, Extensions::ExtensionInventory &inventory) {
            for (const auto &extension : inventory.Entries()) {
                if (extension.origin != Extensions::ExtensionOrigin::UserInstalled ||
                    extension.ActivationState().DesiredActivation() != Extensions::ExtensionDesiredActivation::Active)
                    continue;

                if (Result<std::string> loaded = manager.LoadExtension(extension.absoluteRootPath.string()); loaded.HasError()) {
                    static_cast<void>(inventory.RecordActivationFailure(extension.packageId,
                                                                        Extensions::ExtensionActivationFailureReason::HostLoadFailed,
                                                                        loaded.ErrorValue().message));
                    continue;
                }
                if (auto activated = inventory.MarkRuntimeActive(extension.packageId); activated.HasError())
                    LOG_ERROR("editor.extensions", "Extension activation state was rejected for '%s'.", extension.packageId.c_str());
            }
        }

    }  // namespace

    GuiScreenHost::GuiScreenHost(const EditorGuiContext &context, EditorModalHost &modalHost,  // NOSONAR(cpp:S107)
                                 EditorSettingsService &settingsService, LocalizationService &localization, EngineDataBus &engineEvents,
                                 ProjectCreationService &creationService, JobSystem &jobs, Input::InputRouter &inputRouter,
                                 const RendererAvailabilitySnapshot &rendererAvailability, ScreenRegistry screenRegistry,
                                 WorkspacePanelRegistry workspacePanelRegistry, std::uintptr_t logoTexture,
                                 Extensions::ExtensionInventory *extensionInventory,
                                 Extensions::ExtensionMarketplaceService *extensionMarketplace)

        : context_(&context), modalHost_(&modalHost), settingsService_(&settingsService), localization_(&localization),
          engineEvents_(&engineEvents), logoTexture_(logoTexture), extensionInventory_(extensionInventory),
          extensionMarketplace_(extensionMarketplace), screenRegistry_(std::move(screenRegistry)),
          workspacePanelRegistry_(std::move(workspacePanelRegistry)) {
        services_.Register(*this);
        services_.RegisterConst(context);
        services_.Register(modalHost);
        services_.Register(settingsService);
        services_.Register(localization);
        services_.Register(engineEvents);
        services_.Register(creationService);
        services_.Register(jobs);
        services_.Register(inputRouter);
        services_.RegisterConst(rendererAvailability);
        services_.Register(logoTexture_);
        services_.Register<ScreenRegistry>(screenRegistry_);
        services_.Register<WorkspacePanelRegistry>(workspacePanelRegistry_);
        services_.Register<EditorStatusItemRegistry>(statusItemRegistry_);
        if (extensionInventory_ != nullptr)
            services_.Register<Extensions::ExtensionInventory>(*extensionInventory_);
        if (extensionMarketplace_ != nullptr)
            services_.Register<Extensions::ExtensionMarketplaceService>(*extensionMarketplace_);
        importerCatalogCandidate_ = std::make_unique<Assets::AssetImporterCatalog>();
        extensionManager_ = std::make_unique<Extensions::ExtensionManager>(importerCatalogCandidate_.get());
        ActivateBuiltInImporters(*importerCatalogCandidate_, extensionInventory_);
        if (extensionInventory_ != nullptr)
            ActivateUserExtensions(*extensionManager_, *extensionInventory_);
        if (auto published = importerCatalogCandidate_->Publish(); published.HasValue())
            importerCatalog_ = std::move(published).Value();
        if (!importerCatalog_) {
            LOG_ERROR("editor.asset_import", "Unable to publish the built-in asset importer catalog.");
            importerCatalog_ = std::make_shared<const Assets::AssetImporterCatalogSnapshot>();
        }
        services_.RegisterConst<Assets::AssetImporterCatalogSnapshot>(*importerCatalog_);
        statusBar_ = std::make_unique<EditorStatusBar>(context, statusItemRegistry_);
        activeStatusPanelIds_.reserve(16);

        static_cast<void>(
            statusItemRegistry_.Register(EditorStatusItemDescriptor{.id = "horo.status.navigation",
                                                                    .labelKey = "status.navigation.label",
                                                                    .alignment = EditorStatusBarAlignment::Left,
                                                                    .priority = 70,
                                                                    .order = 30,
                                                                    .maxWidth = 96.0F},
                                         EditorStatusItemContent{.value = localization.Get("editor", "status.navigation.idle")}));
        static_cast<void>(
            statusItemRegistry_.Register(EditorStatusItemDescriptor{.id = "horo.status.backend",
                                                                    .alignment = EditorStatusBarAlignment::Right,
                                                                    .priority = 100,
                                                                    .order = 10,
                                                                    .maxWidth = 96.0F,
                                                                    .presentation = EditorStatusItemPresentation::Plain},
                                         EditorStatusItemContent{
                                             .label = rendererAvailability.Find(rendererAvailability.ActiveBackendId()) != nullptr
                                                          ? rendererAvailability.Find(rendererAvailability.ActiveBackendId())->displayName
                                                          : std::string{rendererAvailability.ActiveBackendId()}}));
        static_cast<void>(statusItemRegistry_.Register(EditorStatusItemDescriptor{.id = "horo.status.document",
                                                                                  .alignment = EditorStatusBarAlignment::Left,
                                                                                  .priority = 100,
                                                                                  .order = 0,
                                                                                  .maxWidth = 140.0F,
                                                                                  .presentation = EditorStatusItemPresentation::Pill},
                                                       EditorStatusItemContent{.iconResourceId = "horo.status.document",
                                                                               .label = localization.Get("editor", "status.document.saved"),
                                                                               .tone = EditorStatusItemTone::Success,
                                                                               .available = false}));
        static_cast<void>(statusItemRegistry_.Register(EditorStatusItemDescriptor{.id = "horo.status.selection",
                                                                                  .labelKey = "status.selection.label",
                                                                                  .alignment = EditorStatusBarAlignment::Left,
                                                                                  .priority = 80,
                                                                                  .order = 10,
                                                                                  .maxWidth = 112.0F},
                                                       EditorStatusItemContent{.value = localization.Get("editor", "status.selection.none"),
                                                                               .available = false}));
    }

    GuiScreenHost::~GuiScreenHost() {
        Shutdown();
    }

    /** @copydoc GuiScreenHost::Start */
    Result<void> GuiScreenHost::Start(GuiRoute initialRoute) {
        if (shutdown_)
            return Result<void>::Failure(MakeError(NavigationErrors::HostShutdown));
        if (started_)
            return Result<void>::Failure(MakeError(NavigationErrors::HostAlreadyStarted));
        if (!IsRoutePayloadValid(initialRoute)) {
            return Result<void>::Failure(MakeError(NavigationErrors::InvalidRouteParameters, "Invalid initial route payload."));
        }

        std::unique_ptr<GuiScreen> initialScreen = CreateScreen(initialRoute);
        if (!initialScreen) {
            return Result<void>::Failure(MakeError(NavigationErrors::ScreenCreationFailed));
        }
        if (Result<void> entered = initialScreen->OnEnter(initialRoute); entered.HasError()) {
            return entered;
        }

        activeRoute_ = std::move(initialRoute);
        activeRevision_ = GuiRouteRevision{1};
        activeScreen_ = std::move(initialScreen);
        started_ = true;
        return Result<void>::Success();
    }

    /** @copydoc GuiScreenHost::Shutdown */
    void GuiScreenHost::Shutdown() noexcept {
        if (shutdown_) {
            return;
        }
        shutdown_ = true;
        if (activeScreen_) {
            activeScreen_->OnLeave();
            activeScreen_.reset();
        }
        services_.Clear();
    }

    bool GuiScreenHost::IsShutdown() const noexcept {
        return shutdown_;
    }

    EditorServiceRegistry &GuiScreenHost::Services() noexcept {
        return services_;
    }

    const EditorServiceRegistry &GuiScreenHost::Services() const noexcept {
        return services_;
    }

    ScreenRegistry &GuiScreenHost::Screens() noexcept {
        return screenRegistry_;
    }

    const ScreenRegistry &GuiScreenHost::Screens() const noexcept {
        return screenRegistry_;
    }

    /** @copydoc GuiScreenHost::StatusItems */
    EditorStatusItemRegistry &GuiScreenHost::StatusItems() noexcept {
        return statusItemRegistry_;
    }

    /** @copydoc GuiScreenHost::StatusItems */
    const EditorStatusItemRegistry &GuiScreenHost::StatusItems() const noexcept {
        return statusItemRegistry_;
    }

    const GuiRoute &GuiScreenHost::ActiveRoute() const noexcept {
        return activeRoute_;
    }

    bool GuiScreenHost::IsApplicationCloseRequested() const noexcept {
        return closeRequested_;
    }

    /** @copydoc GuiScreenHost::RequestRendererRestart */
    Result<void> GuiScreenHost::RequestRendererRestart(EditorRendererRestartRequest request) {
        if (shutdown_)
            return Result<void>::Failure(MakeError(NavigationErrors::HostShutdown));
        rendererRestartRequest_ = std::move(request);
        const Result<void> close = RequestCloseApplication();
        if (close.HasError())
            rendererRestartRequest_.reset();
        return close;
    }

    void GuiScreenHost::RequestFatalShutdown() noexcept {
        modalHost_->ForceDetachAllForShutdown();
        closeRequested_ = true;
    }

    /** @copydoc GuiScreenHost::RendererRestartRequest */
    const std::optional<EditorRendererRestartRequest> &GuiScreenHost::RendererRestartRequest() const noexcept {
        return rendererRestartRequest_;
    }

    void GuiScreenHost::SetActiveCreationId(std::optional<ProjectCreationOperationId> id) noexcept {
        activeCreationId_ = id;
    }

    /** @copydoc GuiScreenHost::GetActiveCreationId */
    std::optional<ProjectCreationOperationId> GuiScreenHost::GetActiveCreationId() const noexcept {
        return activeCreationId_;
    }

    /** @copydoc GuiScreenHost::CurrentProjectRoot */
    std::filesystem::path GuiScreenHost::CurrentProjectRoot() const noexcept {
        if (!currentProjectRoot_.empty())
            return currentProjectRoot_;
        return std::filesystem::current_path();
    }

    Result<void> GuiScreenHost::Navigate(GuiRoute destination) {
        if (shutdown_)
            return Result<void>::Failure(MakeError(NavigationErrors::HostShutdown));
        if (!started_)
            return Result<void>::Failure(MakeError(NavigationErrors::HostNotStarted));
        if (!IsRoutePayloadValid(destination)) {
            return Result<void>::Failure(
                MakeError(NavigationErrors::InvalidRouteParameters, "Invalid route payload for requested route kind."));
        }
        if (navigationBusy_) {
            return Result<void>::Failure(MakeError(NavigationErrors::Busy, "Navigation already in progress."));
        }
        if (pendingRequirement_.has_value() || pendingTarget_.has_value()) {
            return Result<void>::Failure(MakeError(NavigationErrors::Busy, "Leave resolution already pending."));
        }
        if (AreRoutesIdentical(activeRoute_, destination)) {
            return Result<void>::Success();
        }
        if (isScreenCallbackActive_) {
            if (pendingNavigation_.has_value()) {
                return Result<void>::Failure(MakeError(NavigationErrors::Busy, "Navigation already queued this frame."));
            }
            pendingNavigation_ = std::move(destination);
            return Result<void>::Success();
        }
        return ExecuteLeaveCheckAndCommit(LeaveTarget{destination});
    }

    Result<void> GuiScreenHost::RequestCloseApplication() {
        if (shutdown_)
            return Result<void>::Failure(MakeError(NavigationErrors::HostShutdown));
        if (!started_)
            return Result<void>::Failure(MakeError(NavigationErrors::HostNotStarted));
        if (navigationBusy_ || pendingRequirement_.has_value() || pendingTarget_.has_value()) {
            return Result<void>::Failure(MakeError(NavigationErrors::Busy, "Navigation already in progress."));
        }
        return ExecuteLeaveCheckAndCommit(LeaveTarget{ApplicationCloseTarget{}});
    }

    Result<void> GuiScreenHost::ExecuteLeaveCheckAndCommit(const LeaveTarget &target) {
        if (!activeScreen_) {
            if (std::holds_alternative<GuiRoute>(target.value)) {
                CommitRoute(std::get<GuiRoute>(target.value));
            } else {
                return CommitApplicationClose();
            }
            return Result<void>::Success();
        }

        const auto decision = activeScreen_->CanLeave(target);
        if (decision.disposition == LeaveDisposition::Deny) {
            return Result<void>::Failure(MakeError(NavigationErrors::LeaveDenied, "Active screen denied leave transition."));
        }
        if (decision.disposition == LeaveDisposition::RequireResolution) {
            if (!decision.requirement.has_value()) {
                return Result<void>::Failure(
                    MakeError(NavigationErrors::InvalidLeaveRequirement, "Screen requested leave resolution without a requirement."));
            }
            if (resolutionAttemptCount_ >= 5) {
                return Result<void>::Failure(MakeError(NavigationErrors::LeaveResolutionLimitExceeded, "Exceeded resolution attempts."));
            }
            pendingRequirement_ = decision.requirement;
            pendingTarget_ = target;
            resolutionAttemptCount_++;
            return Result<void>::Success();
        }

        if (std::holds_alternative<GuiRoute>(target.value)) {
            CommitRoute(std::get<GuiRoute>(target.value));
        } else {
            return CommitApplicationClose();
        }
        return Result<void>::Success();
    }

    void GuiScreenHost::CommitRoute(GuiRoute destination) {
        navigationBusy_ = true;
        resolutionAttemptCount_ = 0;
        pendingRequirement_.reset();
        pendingTarget_.reset();

        std::unique_ptr<GuiScreen> candidate = CreateScreen(destination);
        if (!candidate) {
            navigationBusy_ = false;
            return;
        }

        if (activeScreen_) {
            activeScreen_->OnLeave();
        }

        if (const auto enterRes = candidate->OnEnter(destination); enterRes.HasError()) {
            LOG_ERROR("editor.screens", "Destination OnEnter failed: %s", enterRes.ErrorValue().message.c_str());
            if (activeScreen_) {
                if (const auto rollback = activeScreen_->OnEnter(activeRoute_); rollback.HasError()) {
                    LOG_ERROR("editor.screens", "Active screen rollback OnEnter failed: %s", rollback.ErrorValue().message.c_str());
                    activeScreen_.reset();
                    closeRequested_ = true;
                }
            }
            navigationBusy_ = false;
            return;
        }

        const GuiRouteKind prevKind = activeRoute_.kind;
        const GuiRouteRevision prevRev = activeRevision_;
        activeRoute_ = std::move(destination);
        activeRevision_++;
        activeScreen_ = std::move(candidate);
        navigationBusy_ = false;

        LOG_DEBUG("editor.routing", "Route committed: kind=%d revision=%llu", static_cast<int>(activeRoute_.kind),
                  static_cast<unsigned long long>(activeRevision_));

        if (engineEvents_) {
            GuiRouteChangedEvent ev{prevKind, activeRoute_.kind, prevRev, activeRevision_};
            engineEvents_->Publish(ev);
        }
    }

    std::unique_ptr<GuiScreen> GuiScreenHost::CreateScreen(const GuiRoute &route) {
        return screenRegistry_.CreateScreen(route, services_);
    }

    void GuiScreenHost::OnUpdate(float dt) {
        if (localization_ != nullptr) {
            static_cast<void>(
                statusItemRegistry_.Update("horo.status.navigation",
                                           EditorStatusItemContent{.value = localization_->Get("editor", navigationBusy_
                                                                                                             ? "status.navigation.busy"
                                                                                                             : "status.navigation.idle")}));
        }
        if (activeScreen_) {
            isScreenCallbackActive_ = true;
            activeScreen_->OnUpdate(dt);
            isScreenCallbackActive_ = false;
        }
        FlushPendingNavigation();
    }

    /** @copydoc GuiScreenHost::OnFixedUpdate */
    void GuiScreenHost::OnFixedUpdate(const double fixedDeltaSeconds) {
        if (activeScreen_) {
            isScreenCallbackActive_ = true;
            activeScreen_->OnFixedUpdate(fixedDeltaSeconds);
            isScreenCallbackActive_ = false;
        }
        FlushPendingNavigation();
    }

    void GuiScreenHost::FlushPendingNavigation() {
        if (!pendingNavigation_.has_value()) {
            return;
        }
        GuiRoute destination = std::move(*pendingNavigation_);
        pendingNavigation_.reset();
        if (const Result<void> result = Navigate(std::move(destination)); result.HasError()) {
            LOG_ERROR("editor.screens", "Deferred navigation failed: %s", result.ErrorValue().message.c_str());
        }
    }

    void GuiScreenHost::Draw() {
        if (!uiPreviewScenario_.empty()) {
            DrawUiPreview();
            return;
        }
        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        const float contentHeight = std::max(0.0F, viewport->WorkSize.y - EditorStatusBar::Height);
        const GuiContentRegion contentRegion{viewport->WorkPos.x, viewport->WorkPos.y, viewport->WorkSize.x, contentHeight};

        if (activeScreen_) {
            isScreenCallbackActive_ = true;
            activeScreen_->Draw(contentRegion);
            isScreenCallbackActive_ = false;
        }

        FlushPendingNavigation();

        activeStatusPanelIds_.clear();
        if (activeScreen_) {
            activeScreen_->CollectActivePanelIds(activeStatusPanelIds_);
        }

        if (statusBar_) {
            const bool interactionEnabled = modalHost_ != nullptr && !modalHost_->HasOpenModal() && !pendingRequirement_.has_value();
            const auto invocation = statusBar_->Draw(ImVec2{contentRegion.x, contentRegion.y + contentRegion.height},
                                                     ImVec2{contentRegion.width, EditorStatusBar::Height},
                                                     EditorStatusBarContext{activeStatusPanelIds_}, interactionEnabled);
            if (invocation.has_value() && engineEvents_ != nullptr) {
                engineEvents_->Publish(*invocation);
            }
        }
        if (pendingRequirement_.has_value() && pendingTarget_.has_value()) {
            PresentLeaveDialog(*pendingRequirement_, *pendingTarget_);
        }
    }

    /** @copydoc GuiScreenHost::DispatchMenuInvocation */
    void GuiScreenHost::DispatchMenuInvocation(const EditorMenuInvocation &invocation) {
        using enum EditorMenuAction;
        switch (invocation.action) {
            case NewProject:
                static_cast<void>(Navigate(GuiRoute{GuiRouteKind::ProjectCreation, ProjectCreationRouteParameters{}}));
                return;
            case OpenProject:
                static_cast<void>(Navigate(GuiRoute{GuiRouteKind::Welcome, WelcomeRouteParameters{}}));
                return;
            case ImportAssets:
                if (context_ && modalHost_ && !modalHost_->HasOpenModal()) {
                    auto modal = std::make_unique<AssetImportModal>(context_->theme.fonts, m_importJobs, importerCatalog_,
                                                                    services_.TryGet<Assets::AssetRegistry>(),
                                                                    services_.TryGet<OperationStore>(), localization_);
                    modal->SetProjectRoot(CurrentProjectRoot());
                    if (invocation.assetDestination.has_value())
                        modal->SetDefaultDestination(*invocation.assetDestination);
                    static_cast<void>(modalHost_->OpenRoot(std::move(modal)));
                }
                return;
            case OpenEditorSettings:
                if (context_ && settingsService_ && modalHost_) {
                    auto modal = std::make_unique<SettingsModal>(*context_, *settingsService_, logoTexture_, extensionInventory_,
                                                                 extensionMarketplace_);
                    static_cast<void>(modalHost_->OpenRoot(std::move(modal)));
                }
                return;
            case ExitApplication:
                static_cast<void>(RequestCloseApplication());
                return;
            case SaveScene:
            case SaveSceneAs:
            case SaveSceneCopyAs:
            case Undo:
            case Redo:
            case CreatePrimitive:
                if (activeScreen_) {
                    static_cast<void>(activeScreen_->HandleMenuInvocation(invocation));
                }
                return;
            case None:
                return;
        }
    }

    void GuiScreenHost::HandleDropFiles(const std::vector<std::filesystem::path> &files) {
        if (files.empty() || !modalHost_)
            return;

        // Auto-open import modal if not already open
        if (!modalHost_->HasOpenModal()) {
            EditorMenuInvocation inv{EditorMenuAction::ImportAssets};
            DispatchMenuInvocation(inv);
        }

        // Feed files into the import modal if it's the top modal.
        auto *topModal = modalHost_->TopModal();
        auto *importModal = dynamic_cast<AssetImportModal *>(topModal);
        if (importModal) {
            CancellationToken cancellation;
            static_cast<void>(importModal->BeginImport(files, CurrentProjectRoot(), cancellation));
        }
    }

    void GuiScreenHost::ExecuteLeaveResolution(LeaveAction action, const LeaveRequirement &requirement, const LeaveTarget &target) {
        if (action == LeaveAction::Stay) {
            pendingRequirement_.reset();
            pendingTarget_.reset();
            resolutionAttemptCount_ = 0;
            return;
        }
        if (!activeScreen_ || !pendingRequirement_.has_value() || !pendingTarget_.has_value()) {
            return;
        }

        const LeaveRequirement current = *pendingRequirement_;
        const bool staleRequirement = current.subject != requirement.subject || current.revision != requirement.revision;
        if (const bool actionAllowed = std::ranges::find(current.allowedActions, action) != current.allowedActions.end();
            staleRequirement || !actionAllowed) {
            LOG_ERROR("editor.screens", "Rejected stale or disallowed leave resolution.");
            return;
        }

        const auto result = activeScreen_->ResolveLeave(target, LeaveResolution{requirement.subject, requirement.revision, action});
        if (result.HasError()) {
            LOG_ERROR("editor.screens", "Leave resolution failed: %s", result.ErrorValue().message.c_str());
            return;
        }

        const LeaveDecision &decision = result.Value();
        if (decision.disposition == LeaveDisposition::Deny) {
            pendingRequirement_.reset();
            pendingTarget_.reset();
            resolutionAttemptCount_ = 0;
            return;
        }
        if (decision.disposition == LeaveDisposition::RequireResolution) {
            if (!decision.requirement.has_value()) {
                LOG_ERROR("editor.screens", "Leave resolution requested without a requirement.");
                return;
            }
            const LeaveRequirement &next = *decision.requirement;
            const bool progressed = next.subject != current.subject || next.revision > current.revision;
            ++resolutionAttemptCount_;
            if (!progressed || resolutionAttemptCount_ >= 5) {
                LOG_ERROR("editor.screens", "Leave resolution chain made no progress or exceeded its limit.");
                pendingRequirement_.reset();
                pendingTarget_.reset();
                resolutionAttemptCount_ = 0;
                return;
            }
            pendingRequirement_ = next;
            return;
        }

        pendingRequirement_.reset();
        pendingTarget_.reset();
        resolutionAttemptCount_ = 0;
        if (std::holds_alternative<ApplicationCloseTarget>(target.value)) {
            static_cast<void>(CommitApplicationClose());
        } else {
            CommitRoute(std::get<GuiRoute>(target.value));
        }
    }

    Result<void> GuiScreenHost::CommitApplicationClose() {
        if (modalHost_->HasOpenModal()) {
            const Result<void> modalClosed = modalHost_->RequestCloseAllForShutdown();
            if (modalClosed.HasError()) {
                rendererRestartRequest_.reset();
                closeRequested_ = false;
                return modalClosed;
            }
        }
        closeRequested_ = true;
        return Result<void>::Success();
    }

    void GuiScreenHost::PresentLeaveDialog(const LeaveRequirement &requirement, const LeaveTarget &target) {
        ImGui::OpenPopup("Unsaved Changes");
        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5F, 0.5F));
        if (!ImGui::BeginPopupModal("Unsaved Changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            return;
        }

        if (requirement.kind == LeaveRequirementKind::UnsavedDraft) {
            ImGui::Text("You have unsaved project creation changes.\nDo you want to discard them and leave?");
        } else if (requirement.kind == LeaveRequirementKind::RunningOperation) {
            ImGui::Text("A background operation is running.\nDo you want to cancel the operation and leave?");
        } else {
            ImGui::Text("This screen requires confirmation before leaving.");
        }
        ImGui::Separator();

        std::optional<LeaveAction> selectedAction;
        for (const auto action : requirement.allowedActions) {
            if (ImGui::Button(GetLeaveActionLabel(action), ImVec2(140, 0))) {
                ImGui::CloseCurrentPopup();
                selectedAction = action;
            }
            ImGui::SameLine();
        }
        ImGui::NewLine();
        ImGui::EndPopup();
        if (selectedAction.has_value()) {
            ExecuteLeaveResolution(*selectedAction, requirement, target);
        }
    }

}  // namespace Horo::Editor
