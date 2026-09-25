#pragma once

#include "Horo/Assets/AssetImporter.h"
#include "Horo/Editor/EditorMenuModel.h"
#include "Horo/Editor/EditorServiceRegistry.h"
#include "Horo/Editor/EditorStatusBarModel.h"
#include "Horo/Editor/GuiRoute.h"
#include "Horo/Editor/GuiScreen.h"
#include "Horo/Editor/ScreenRegistry.h"
#include "Horo/Editor/WorkspacePanelRegistry.h"
#include "Horo/Foundation/Diagnostics.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Result.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace Horo {
    class EngineDataBus;
    class NativeDialogs;
}  // namespace Horo

namespace Horo::Input {
    class InputRouter;
}  // namespace Horo::Input

namespace Horo::Extensions {
    class ExtensionInventory;
    class ExtensionMarketplaceService;
    class ExtensionManager;
}  // namespace Horo::Extensions

namespace Horo::Editor {

    struct EditorGuiContext;
    class EditorModalHost;
    class EditorSettingsService;
    class LocalizationService;
    class ProjectCreationService;
    class RendererAvailabilitySnapshot;
    class EditorStatusBar;
    class BuildWorkflowPreviewState;

    /**
     * @file GuiScreenHost.h
     * @brief Top-level screen host, route navigation, and leave-guard contracts.
     */

    /** @brief Controlled process-level request to rebuild the editor with a project's renderer composition. */
    struct EditorRendererRestartRequest {
        std::string backendId;
        std::string projectRoot;
        std::string projectName;
    };

    /**
     * @brief Coordinates top-level screens, route transitions, and leave guards.
     */
    class GuiScreenHost {  // NOSONAR(cpp:S1820, cpp:S1448)
    public:
        /**
         * @brief Constructs the screen host with required application service references.
         * @param context Immutable GUI theme, localization, and event context.
         * @param modalHost Root modal lifecycle owner.
         * @param settingsService Committed editor-settings authority.
         * @param localization Active editor localization service.
         * @param engineEvents Engine-wide event bus.
         * @param creationService Project creation operation service.
         * @param jobs Application job system borrowed by screen-owned background work.
         * @param inputRouter Application input-context router.
         * @param rendererAvailability Immutable renderer availability projection.
         * @param screenRegistry Registered top-level screen factories.
         * @param workspacePanelRegistry Registered workspace panel factories.
         * @param logoTexture Optional renderer-owned editor logo texture.
         * @param extensionInventory Optional installed-extension inventory.
         * @param extensionMarketplace Optional extension marketplace service.
         * @param nativeDialogs Optional host-owned file picker for editor workflows.
         */
        explicit GuiScreenHost(const EditorGuiContext &context, EditorModalHost &modalHost,  // NOSONAR(cpp:S107) Service aggregate
                               EditorSettingsService &settingsService, LocalizationService &localization, EngineDataBus &engineEvents,
                               ProjectCreationService &creationService, JobSystem &jobs, Input::InputRouter &inputRouter,
                               const RendererAvailabilitySnapshot &rendererAvailability, ScreenRegistry screenRegistry,
                               WorkspacePanelRegistry workspacePanelRegistry, std::uintptr_t logoTexture = 0,
                               Extensions::ExtensionInventory *extensionInventory = nullptr,
                               Extensions::ExtensionMarketplaceService *extensionMarketplace = nullptr,
                               NativeDialogs *nativeDialogs = nullptr);

        ~GuiScreenHost();

        GuiScreenHost(const GuiScreenHost &) = delete;
        GuiScreenHost &operator=(const GuiScreenHost &) = delete;
        GuiScreenHost(GuiScreenHost &&) = delete;
        GuiScreenHost &operator=(GuiScreenHost &&) = delete;

        /**
         * @brief Constructs and enters the initial route after composition has registered every borrowed service.
         * @param initialRoute Initial typed route to activate.
         * @return Success when the initial screen entered, or a typed lifecycle/navigation failure.
         */
        [[nodiscard]] Result<void> Start(GuiRoute initialRoute);

        /**
         * @brief Starts the isolated UI gallery without entering a normal editor route.
         * @param scenarioId Registered scenario to open.
         * @return Success when the preview surface was admitted.
         */
        [[nodiscard]] Result<void> StartUiPreview(std::string_view scenarioId);

        /** @brief Leaves and destroys the active screen exactly once, then revokes borrowed services. */
        void Shutdown() noexcept;

        /** @brief Reports whether deterministic shutdown already revoked the host. */
        [[nodiscard]] bool IsShutdown() const noexcept;

        /**
         * @brief Requests navigation to a new top-level route.
         * @param destination Target route.
         * @return Success when accepted. Requests made from a screen callback are queued until that callback returns.
         */
        Result<void> Navigate(GuiRoute destination);

        /**
         * @brief Requests application close, passing through leave guards.
         * @return Success if allowed, or error if blocked/denied.
         */
        Result<void> RequestCloseApplication();

        /** @brief Returns the currently active route. */
        [[nodiscard]] const GuiRoute &ActiveRoute() const noexcept;

        /** @brief Reports whether application exit has been approved and requested. */
        [[nodiscard]] bool IsApplicationCloseRequested() const noexcept;

        /** @brief Requests a controlled renderer-session restart before opening a project. */
        [[nodiscard]] Result<void> RequestRendererRestart(EditorRendererRestartRequest request);

        /** @brief Forces process shutdown after an unrecoverable renderer or platform failure. */
        void RequestFatalShutdown() noexcept;

        /** @brief Returns the pending renderer-session restart, if any. */
        [[nodiscard]] const std::optional<EditorRendererRestartRequest> &RendererRestartRequest() const noexcept;

        /** @brief Sets active project creation operation ID being tracked across route transitions. */
        void SetActiveCreationId(std::optional<ProjectCreationOperationId> id) noexcept;

        /** @brief Returns active project creation operation ID if one is currently tracked. */
        [[nodiscard]] std::optional<ProjectCreationOperationId> GetActiveCreationId() const noexcept;

        /** @brief Sets the current project root for import/file operations. */
        void SetCurrentProjectRoot(const std::filesystem::path &root) noexcept {
            currentProjectRoot_ = root;
        }

        /** @brief Returns the current project root, or current_path() as fallback. */
        [[nodiscard]] std::filesystem::path CurrentProjectRoot() const noexcept;

        /** @brief Updates the active screen and checks pending leave dialogs. */
        void OnUpdate(float dt);

        /** @brief Routes one host fixed tick to the active screen. */
        void OnFixedUpdate(double fixedDeltaSeconds);

        /** @brief Renders the active screen and any active leave-resolution modals. */
        void Draw();

        /**
         * @brief Routes a platform-neutral application menu invocation through host navigation and the active screen.
         * @param invocation Invocation selected by the user.
         */
        void DispatchMenuInvocation(const EditorMenuInvocation &invocation);

        /** @brief Opens one inert editor UI preview scenario by its catalog identity. */
        [[nodiscard]] bool OpenUiPreview(std::string_view scenarioId);

        /** @brief Returns mutable service registry used for dependency injection. */
        [[nodiscard]] EditorServiceRegistry &Services() noexcept;

        /** @brief Returns const service registry used for dependency injection. */
        [[nodiscard]] const EditorServiceRegistry &Services() const noexcept;

        /** @brief Returns mutable screen factory registry. */
        [[nodiscard]] ScreenRegistry &Screens() noexcept;

        /** @brief Returns const screen factory registry. */
        [[nodiscard]] const ScreenRegistry &Screens() const noexcept;

        /** @brief Returns the host-owned registry used by built-ins, modules, and plugin adapters. */
        [[nodiscard]] EditorStatusItemRegistry &StatusItems() noexcept;

        /** @brief Handles files dropped onto the editor window, dispatching to the import modal. */
        void HandleDropFiles(const std::vector<std::filesystem::path> &files);

        /** @brief Returns the host-owned status-item registry for read-only inspection. */
        [[nodiscard]] const EditorStatusItemRegistry &StatusItems() const noexcept;

    private:
        Result<void> ExecuteLeaveCheckAndCommit(const LeaveTarget &target);
        Result<void> CommitApplicationClose();
        void FlushPendingNavigation();
        void DrawUiPreview();
        void CommitRoute(GuiRoute destination);
        void PresentLeaveDialog(const LeaveRequirement &requirement, const LeaveTarget &target);
        void ExecuteLeaveResolution(LeaveAction action, const LeaveRequirement &requirement, const LeaveTarget &target);
        std::unique_ptr<GuiScreen> CreateScreen(const GuiRoute &route);

        const EditorGuiContext *context_;
        EditorModalHost *modalHost_;
        EditorSettingsService *settingsService_;
        LocalizationService *localization_;
        EngineDataBus *engineEvents_;
        std::uintptr_t logoTexture_{0};
        Extensions::ExtensionInventory *extensionInventory_{};
        Extensions::ExtensionMarketplaceService *extensionMarketplace_{};
        NativeDialogs *nativeDialogs_{};
        Input::InputRouter *inputRouter_{};

        EditorServiceRegistry services_;
        ScreenRegistry screenRegistry_;
        WorkspacePanelRegistry workspacePanelRegistry_;
        EditorStatusItemRegistry statusItemRegistry_;
        std::unique_ptr<EditorStatusBar> statusBar_;
        std::unique_ptr<BuildWorkflowPreviewState> buildPreviewState_;
        std::vector<std::string_view> activeStatusPanelIds_;

        JobSystem m_importJobs{JobSystemConfig{.workerCount = 1}};
        std::unique_ptr<Assets::AssetImporterCatalog> importerCatalogCandidate_;
        std::unique_ptr<Extensions::ExtensionManager> extensionManager_;
        std::shared_ptr<const Assets::AssetImporterCatalogSnapshot> importerCatalog_;

        GuiRoute activeRoute_{GuiRouteKind::Welcome, WelcomeRouteParameters{}};
        std::string uiPreviewScenario_;
        GuiRouteRevision activeRevision_{0};

        std::unique_ptr<GuiScreen> activeScreen_;
        std::optional<GuiRoute> pendingNavigation_;
        std::optional<ProjectCreationOperationId> activeCreationId_;
        std::optional<EditorRendererRestartRequest> rendererRestartRequest_;
        std::filesystem::path currentProjectRoot_;
        bool closeRequested_{false};
        bool navigationBusy_{false};
        bool isScreenCallbackActive_{false};
        bool started_{false};
        bool shutdown_{false};

        std::optional<LeaveRequirement> pendingRequirement_;
        std::optional<LeaveTarget> pendingTarget_;
        std::size_t resolutionAttemptCount_{0};
    };

}  // namespace Horo::Editor
