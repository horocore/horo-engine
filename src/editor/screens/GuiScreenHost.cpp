#include "Horo/Editor/GuiScreenHost.h"

#include "../modals/extension_pages/EditorPageModal.h"
#include "../modals/extension_pages/EditorPageThemeCapture.h"
#include "EditorPageFileStore.h"
#include "Horo/Application/ProjectAssetImportCommitter.h"
#include "Horo/Editor/AssetImportModal.h"
#include "Horo/Editor/AudioComponentPreview.h"
#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorModalHost.h"
#include "Horo/Editor/EditorSettingsService.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/GuiLiveReload.h"
#include "Horo/Editor/Localization/LocalizationService.h"
#include "Horo/Editor/ProjectCreationService.h"
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Editor/SettingsModal.h"
#include "Horo/Extensions/EditorActivityAbi.h"
#include "Horo/Extensions/EditorActivityHost.h"
#include "Horo/Extensions/EditorInspectorAbi.h"
#include "Horo/Extensions/EditorInspectorHost.h"
#include "Horo/Extensions/EditorPageAbi.h"
#include "Horo/Extensions/EditorPageHost.h"
#include "Horo/Extensions/EditorViewportAbi.h"
#include "Horo/Extensions/EditorViewportHost.h"
#include "Horo/Extensions/ExtensionInventory.h"
#include "Horo/Extensions/ExtensionManager.h"
#include "Horo/Extensions/ExtensionMarketplace.h"
#include "Horo/Extensions/NativeExtensionServiceHost.h"
#include "Horo/Foundation/DataBus.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/OperationStore.h"
#include "Horo/Gameplay/GameModuleRetirement.h"
#include "Horo/Runtime/Input.h"
#include "LeaveConfirmationView.h"
#include "NavigationErrors.h"
#include "ScriptModuleHostServices.h"
#include "editor/gameplay/EditorPlaySaveRetirement.h"
#include "editor/input/EditorInputActions.h"
#include "editor/modals/build_release/BuildActivity.h"
#include "editor/modals/build_release/BuildModal.h"
#include "editor/modals/build_release/BuildReleaseModal.h"
#include "editor/project_model/RendererAvailability.h"
#include "editor/status_bar/EditorStatusBar.h"
#include "editor/update/UpdateExperienceSession.h"
#include "runtime/assets/importer/builtin/obj_mesh/ObjMeshImporter.h"
#include "workspace/terrain/TerrainAssetFormat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <imgui.h>

namespace Horo::Editor {
    namespace {
        /** @brief Resolves presentation keys without changing the host-owned action policy. */
        [[nodiscard]] const char *LeaveActionKey(const LeaveAction action) noexcept {
            switch (action) {
                using enum Horo::Editor::LeaveAction;
                case Save:
                    return "save";
                case Discard:
                    return "discard";
                case CancelOperation:
                    return "cancel_operation";
                case KeepRunning:
                    return "keep_running";
                case Wait:
                    return "wait";
                case Stay:
                    return "stay";
            }
            return "stay";
        }

        /** @brief Registers built-in importers and publishes their typed activation outcome. */
        void ActivateBuiltInImporters(Assets::AssetImporterCatalog &catalog, Extensions::ExtensionInventory *inventory) {
            if (inventory != nullptr && !inventory->IsEnabled("horo.builtin.assets"))
                return;

            Result<void> registered = RegisterAllBuiltinImporters(catalog);
            if (registered.HasValue())
                registered = catalog.Register(TerrainRawImporterContribution());
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

        /** @brief Copies actual inventory availability into the existing surface authority after startup admission.
         * Desired enablement changes remain restart-applied while a provider is active.
         */
        void PublishRecoveryReasons(const Extensions::EditorSurfaceRegistry &registry, const Extensions::ExtensionInventory &inventory) {
            using enum Extensions::EditorSurfaceProviderStatus;
            for (const auto &entry : inventory.Entries()) {
                if (entry.runtimeActive)
                    continue;
                auto status = Missing;
                if (!entry.enabled)
                    status = Disabled;
                else if (entry.hostCompatibility == Extensions::ExtensionHostCompatibilityState::Incompatible ||
                         entry.hostCompatibility == Extensions::ExtensionHostCompatibilityState::UnsupportedInHostProfile)
                    status = Incompatible;
                else if (entry.activationOutcome == Extensions::ExtensionActivationOutcome::Failed)
                    status = Failed;
                for (const auto &providerModule : entry.modules)
                    if (const auto recorded = registry.SetProviderStatus({entry.packageId, providerModule.id}, status); recorded.HasError())
                        LOG_WARN("editor.extensions", "Recovery reason admission failed: %s", recorded.ErrorValue().message.c_str());
            }
        }

        /** @brief Reads only admitted package resources during startup; links and escaping paths fail closed. */
        std::vector<std::string> RegisterPackageThemes(const Extensions::ExtensionManifest &manifest) {
            std::vector<std::string> registered;
            std::error_code error;
            const auto root = std::filesystem::canonical(manifest.rootPath, error);
            if (error)
                return registered;
            for (const auto &contribution : manifest.contributions) {
                if (contribution.type != "editor.theme")
                    continue;
                auto path = root;
                bool valid = true;
                const std::filesystem::path relative{contribution.resource};
                if (relative.empty() || relative.is_absolute())
                    continue;
                for (const auto &component : relative) {
                    path /= component;
                    if (component == ".." || std::filesystem::is_symlink(std::filesystem::symlink_status(path, error)) || error) {
                        valid = false;
                        break;
                    }
                }
                Theme::ThemeEntry theme;
                if (!valid || !std::filesystem::is_regular_file(path, error) || error ||
                    !Theme::LoadThemeFromJson(path.string().c_str(), theme)) {
                    LOG_WARN("editor.extensions", "Package theme resource rejected: %s", contribution.id.c_str());
                    continue;
                }
                theme.id = contribution.id;
                if (Theme::RegisterPackageTheme(std::move(theme)))
                    registered.push_back(contribution.id);
            }
            return registered;
        }

        /** @brief Loads eligible user extensions and publishes their typed activation outcomes. */
        std::vector<std::string> ActivateUserExtensions(Extensions::ExtensionManager &manager, Extensions::ExtensionInventory &inventory) {
            std::vector<std::string> themes;
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
                if (const auto *manifest = manager.LoadedManifest(extension.packageId)) {
                    auto registered = RegisterPackageThemes(*manifest);
                    themes.insert(themes.end(), registered.begin(), registered.end());
                }
                if (auto activated = inventory.MarkRuntimeActive(extension.packageId); activated.HasError())
                    LOG_ERROR("editor.extensions", "Extension activation state was rejected for '%s'.", extension.packageId.c_str());
            }
            return themes;
        }

        void ShutdownNativeServices(const std::shared_ptr<Extensions::NativeExtensionServiceHost> &host) noexcept {
            if (host)
                host->BeginShutdown();
        }
    }  // namespace

    /** @copydoc GuiScreenHost::GuiScreenHost */
    GuiScreenHost::GuiScreenHost(const EditorGuiContext &context, GuiScreenHostComposition composition)
        : context_(context), modalHost_(&composition.modalHost), inputRouter_(&composition.inputRouter),
          settingsService_(&composition.settingsService), localization_(&composition.localization),
          engineEvents_(&composition.engineEvents), logoTexture_(composition.logoTexture),
          extensionInventory_(composition.extensionInventory), extensionMarketplace_(composition.extensionMarketplace),
          nativeDialogs_(composition.nativeDialogs), screenRegistry_(std::move(composition.screenRegistry)),
          workspacePanelRegistry_(std::move(composition.workspacePanelRegistry)), dispatchSource_(std::move(composition.dispatchSource)) {
        gameplayRetirements_ = composition.gameplayRetirements ? std::move(composition.gameplayRetirements)
                                                               : std::make_shared<Gameplay::GameModuleRetirementQueue>();
        RegisterApplicationServices(composition);
        InitializeExtensions(composition);
        statusBar_ = std::make_unique<EditorStatusBar>(context, statusItemRegistry_);
        activeStatusPanelIds_.reserve(16);
        InitializeStatusItems(composition.rendererAvailability);
    }

    /** @copydoc GuiScreenHost::RegisterApplicationServices */
    void GuiScreenHost::RegisterApplicationServices(const GuiScreenHostComposition &composition) {
        services_.Register(*this);
        services_.RegisterConst(context_);
        services_.Register(composition.modalHost);
        services_.Register(composition.settingsService);
        services_.Register(composition.localization);
        services_.Register(composition.engineEvents);
        services_.Register(composition.creationService);
        services_.Register(composition.jobs);
        services_.Register(composition.inputRouter);
        services_.RegisterConst(composition.rendererAvailability);
        services_.Register(logoTexture_);
        services_.Register<ScreenRegistry>(screenRegistry_);
        services_.Register<WorkspacePanelRegistry>(workspacePanelRegistry_);
        services_.Register<EditorStatusItemRegistry>(statusItemRegistry_);
        terrainCookFactory_ = composition.terrainCook;
        terrainAssetFactory_ = composition.terrainAssets;
        if (terrainCookFactory_)
            services_.RegisterConst<Application::TerrainAuthoringCookFactory>(*terrainCookFactory_);
        if (terrainAssetFactory_)
            services_.RegisterConst<Application::TerrainAuthoringAssetFactory>(*terrainAssetFactory_);
        if (extensionInventory_ != nullptr)
            services_.Register<Extensions::ExtensionInventory>(*extensionInventory_);
        if (extensionMarketplace_ != nullptr)
            services_.Register<Extensions::ExtensionMarketplaceService>(*extensionMarketplace_);
    }

    /** @copydoc GuiScreenHost::InitializeExtensionServices */
    void GuiScreenHost::InitializeExtensionServices(const GuiScreenHostComposition &composition, std::vector<std::string> &capabilities) {
        if (composition.extensionServicePolicy.revision == 0)
            return;
        extensionCapabilities_ = std::make_shared<Extensions::ApplicationCapabilityRegistry>();
        extensionBackendServices_ = std::make_shared<Extensions::BackendServiceRegistry>();
        extensionScriptInvocations_ = std::make_shared<Extensions::ScriptInvocationRegistry>();
        extensionServices_ =
            std::make_shared<Extensions::NativeExtensionServiceHost>(extensionCapabilities_, extensionBackendServices_,
                                                                     extensionScriptInvocations_, composition.extensionServicePolicy);
        services_.Register<Extensions::NativeExtensionServiceHost>(*extensionServices_);
        capabilities.emplace_back("extension.services");
        capabilities.emplace_back("horo.script.reference");
        auto moduleScripts = Extensions::ScriptModuleHostServices::Create(extensionServices_, composition.jobs,
                                                                          Extensions::ScriptCapabilityProfile::Gameplay);
        if (moduleScripts.HasError()) {
            LOG_ERROR("editor.extensions", "Module-script composition is unavailable: %s", moduleScripts.ErrorValue().message.c_str());
            return;
        }
        moduleScriptServices_ = std::move(moduleScripts).Value();
        services_.Register<Extensions::ScriptModuleHostServices>(*moduleScriptServices_);
    }

    /** @copydoc GuiScreenHost::InitializeExtensions */
    void GuiScreenHost::InitializeExtensions(const GuiScreenHostComposition &composition) {
        auto &jobs = composition.jobs;
        std::vector<std::string> capabilities{HORO_EDITOR_ACTIVITY_HOST_CAPABILITY, HORO_EDITOR_COMMAND_HOST_CAPABILITY};
        importerCatalogCandidate_ = std::make_unique<Assets::AssetImporterCatalog>();
        const auto uiPolicy =
            composition.extensionUiPolicy.revision ? composition.extensionUiPolicy : Extensions::ExtensionAdmissionPolicy{.revision = 1};
        editorActivityHost_ = std::make_shared<Extensions::EditorActivityHost>(jobs, uiPolicy);
        capabilities.emplace_back(HORO_EDITOR_CUSTOM_UI_HOST_CAPABILITY);
        services_.Register<Extensions::EditorActivityHost>(*editorActivityHost_);
        InitializeExtensionPages(composition, capabilities);
        InitializeExtensionInspectors(composition, capabilities);
        if (composition.extensionViewportPolicy.revision) {
            editorViewportHost_ =
                std::make_shared<Extensions::EditorViewportHost>(editorActivityHost_->Registry(), composition.extensionViewportPolicy);
            services_.Register<Extensions::EditorViewportHost>(*editorViewportHost_);
            capabilities.emplace_back(HORO_EDITOR_VIEWPORT_HOST_CAPABILITY);
        }
        InitializeExtensionServices(composition, capabilities);
        extensionManager_ =
            std::make_unique<Extensions::ExtensionManager>(importerCatalogCandidate_.get(), Extensions::ExtensionHostProfile::Interactive,
                                                           std::move(capabilities), composition.extensionArtifactGate,
                                                           Extensions::ExtensionManager::NativeLibraryLoader{},
                                                           Extensions::ExtensionPlatformProviderCommit{},
                                                           Extensions::ExtensionEditorHosts{.activities = editorActivityHost_,
                                                                                            .pages = editorPageHost_,
                                                                                            .inspectors = editorInspectorHost_,
                                                                                            .viewports = editorViewportHost_},
                                                           extensionServices_);
        ActivateBuiltInImporters(*importerCatalogCandidate_, extensionInventory_);
        if (extensionInventory_ != nullptr) {
            extensionThemeIds_ = ActivateUserExtensions(*extensionManager_, *extensionInventory_);
            if (!context_.settings.settings.themeId.empty() && !Theme::SelectThemeById(context_.settings.settings.themeId))
                LOG_WARN("editor.theme", "Saved package theme is unavailable: %s", context_.settings.settings.themeId.c_str());
            PublishRecoveryReasons(editorActivityHost_->Registry(), *extensionInventory_);
        }
        if (auto published = importerCatalogCandidate_->Publish(); published.HasValue())
            importerCatalog_ = std::move(published).Value();
        if (!importerCatalog_) {
            LOG_ERROR("editor.asset_import", "Unable to publish the built-in asset importer catalog.");
            importerCatalog_ = std::make_shared<const Assets::AssetImporterCatalogSnapshot>();
        }
        services_.RegisterConst<Assets::AssetImporterCatalogSnapshot>(*importerCatalog_);
    }

    /** @copydoc GuiScreenHost::InitializeExtensionPages */
    void GuiScreenHost::InitializeExtensionPages(const GuiScreenHostComposition &composition, std::vector<std::string> &capabilities) {
        if (composition.extensionSettingsFiles && !composition.extensionUserSettingsRoot.empty() &&
            composition.extensionPagePolicy.revision != 0) {
            extensionPageTheme_ = std::make_unique<EditorPageThemeCapture>();
            if (auto captured = extensionPageTheme_->Update(context_); captured.HasError()) {
                LOG_ERROR("editor.extensions", "Native page theme capture unavailable: %s", captured.ErrorValue().message.c_str());
                return;
            }
            extensionPageStore_ =
                std::make_unique<EditorPageFileStore>(*composition.extensionSettingsFiles, composition.extensionUserSettingsRoot);
            editorPageHost_ = std::make_shared<Extensions::EditorPageHost>(composition.jobs, editorActivityHost_->Registry(),
                                                                           *extensionPageStore_, composition.extensionPagePolicy);
            services_.Register<Extensions::EditorPageHost>(*editorPageHost_);
            capabilities.emplace_back(HORO_EDITOR_PAGE_HOST_CAPABILITY);
        }
    }

    /** @copydoc GuiScreenHost::InitializeExtensionInspectors */
    void GuiScreenHost::InitializeExtensionInspectors(const GuiScreenHostComposition &composition, std::vector<std::string> &capabilities) {
        if (!composition.extensionInspectorPolicy.revision)
            return;
        if (!extensionPageTheme_)
            extensionPageTheme_ = std::make_unique<EditorPageThemeCapture>();
        if (auto captured = extensionPageTheme_->Update(context_); captured.HasError()) {
            LOG_ERROR("editor.extensions", "Inspector theme capture unavailable: %s", captured.ErrorValue().message.c_str());
            return;
        }
        editorInspectorHost_ =
            std::make_shared<Extensions::EditorInspectorHost>(editorActivityHost_->Registry(), composition.extensionInspectorPolicy);
        services_.Register<Extensions::EditorInspectorHost>(*editorInspectorHost_);
        services_.RegisterConst<Extensions::EditorThemeFrame>(*extensionPageTheme_->Frame());
        capabilities.emplace_back(HORO_EDITOR_INSPECTOR_HOST_CAPABILITY);
    }

    /** @copydoc GuiScreenHost::InitializeStatusItems */
    void GuiScreenHost::InitializeStatusItems(const RendererAvailabilitySnapshot &rendererAvailability) {
        static_cast<void>(statusItemRegistry_.Register(EditorStatusItemDescriptor{.id = BackgroundBuildStatusId,
                                                                                  .alignment = EditorStatusBarAlignment::Left,
                                                                                  .priority = 90,
                                                                                  .order = 35,
                                                                                  .maxWidth = 240,
                                                                                  .presentation = EditorStatusItemPresentation::Plain,
                                                                                  .interactive = true,
                                                                                  .actionId = BackgroundBuildActionId},
                                                       EditorStatusItemContent{.available = false}));
        static_cast<void>(
            statusItemRegistry_.Register(EditorStatusItemDescriptor{.id = "horo.status.navigation",
                                                                    .labelKey = "status.navigation.label",
                                                                    .alignment = EditorStatusBarAlignment::Left,
                                                                    .priority = 70,
                                                                    .order = 30,
                                                                    .maxWidth = 96.0F},
                                         EditorStatusItemContent{.value = localization_->Get("editor", "status.navigation.idle")}));
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
        static_cast<void>(
            statusItemRegistry_.Register(EditorStatusItemDescriptor{.id = "horo.status.document",
                                                                    .alignment = EditorStatusBarAlignment::Left,
                                                                    .priority = 100,
                                                                    .order = 0,
                                                                    .maxWidth = 140.0F,
                                                                    .presentation = EditorStatusItemPresentation::Pill},
                                         EditorStatusItemContent{.iconResourceId = "horo.status.document",
                                                                 .label = localization_->Get("editor", "status.document.saved"),
                                                                 .tone = EditorStatusItemTone::Success,
                                                                 .available = false}));
        static_cast<void>(
            statusItemRegistry_.Register(EditorStatusItemDescriptor{.id = "horo.status.selection",
                                                                    .labelKey = "status.selection.label",
                                                                    .alignment = EditorStatusBarAlignment::Left,
                                                                    .priority = 80,
                                                                    .order = 10,
                                                                    .maxWidth = 112.0F},
                                         EditorStatusItemContent{.value = localization_->Get("editor", "status.selection.none"),
                                                                 .available = false}));
    }

    GuiScreenHost::~GuiScreenHost() {
        Shutdown();
    }

    /** @copydoc GuiScreenHost::Start */
    Result<void> GuiScreenHost::Start(GuiRoute initialRoute) {
        if (shutdown_)
            return Result<void>::Failure(MakeError(GuiLive::HostSymbol<&NavigationErrors::HostShutdown>()));
        if (started_)
            return Result<void>::Failure(MakeError(GuiLive::HostSymbol<&NavigationErrors::HostAlreadyStarted>()));
        if (!IsRoutePayloadValid(initialRoute)) {
            return Result<void>::Failure(
                MakeError(GuiLive::HostSymbol<&NavigationErrors::InvalidRouteParameters>(), "Invalid initial route payload."));
        }

        std::unique_ptr<GuiScreen> initialScreen = CreateScreen(initialRoute);
        if (!initialScreen) {
            return Result<void>::Failure(MakeError(GuiLive::HostSymbol<&NavigationErrors::ScreenCreationFailed>()));
        }
        if (Result<void> entered = EnterScreen(*initialScreen, initialRoute); entered.HasError()) {
            return entered;
        }

        activeRoute_ = std::move(initialRoute);
        activeRevision_ = GuiRouteRevision{1};
        activeScreen_ = std::move(initialScreen);
        started_ = true;
        return Result<void>::Success();
    }

    namespace {
        /** @brief Defers module admission shutdown while the explicit Play queue still owns accepted callbacks. */
        void CloseUnborrowedGameplayModules(const EditorServiceRegistry &services, Gameplay::GameModuleRetirementQueue &modules) {
            const auto *retirement = services.TryGet<EditorPlaySaveRetirement>();
            if (!retirement || !retirement->HasPending())
                (void)modules.Close();
        }

        /** @brief Pumps real deferred Play settlement once without waiting or borrowing an old screen/controller. */
        void PollPlaySaveRetirement(const EditorServiceRegistry &services) {
            if (auto *retirement = services.TryGet<EditorPlaySaveRetirement>()) {
                if (const auto result = retirement->Poll(); result.HasError())
                    LOG_ERROR("editor.play.save", "Deferred Play retirement: %s", result.ErrorValue().message.c_str());
            }
        }
    }  // namespace

    /** @copydoc GuiScreenHost::Shutdown */
    void GuiScreenHost::Shutdown() noexcept {
        if (fixedScreen_) {
            shutdownPending_ = true;
            return;
        }
        if (shutdown_) {
            return;
        }
        shutdown_ = true;
        if (activeScreen_) {
            activeScreen_->OnLeave();
            activeScreen_.reset();
        }
        ShutdownExtensions();
        CloseUnborrowedGameplayModules(services_, *gameplayRetirements_);
        services_.Clear();
    }

    void GuiScreenHost::ShutdownExtensions() noexcept {
        if (editorViewportHost_)
            editorViewportHost_->BeginShutdown();
        if (editorInspectorHost_)
            editorInspectorHost_->BeginShutdown();
        if (editorPageHost_)
            editorPageHost_->BeginShutdown();
        if (editorActivityHost_)
            editorActivityHost_->BeginShutdown();
        ShutdownNativeServices(extensionServices_);
        if (extensionManager_)
            extensionManager_->UnloadAll();
        for (const auto &id : extensionThemeIds_)
            Theme::UnregisterPackageTheme(id);
        extensionThemeIds_.clear();
        if (settingsService_)
            settingsService_->SetContributedInputActions({});
    }

    /** @copydoc GuiScreenHost::GameplayRetirements */
    std::shared_ptr<Gameplay::GameModuleRetirementQueue> GuiScreenHost::GameplayRetirements() const noexcept {
        return gameplayRetirements_;
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
            return Result<void>::Failure(MakeError(GuiLive::HostSymbol<&NavigationErrors::HostShutdown>()));
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

    /** @copydoc GuiScreenHost::SetCurrentProjectRoot */
    void GuiScreenHost::SetCurrentProjectRoot(const std::filesystem::path &root) {
        if (currentProjectRoot_ != root)
            nativeBuildSession_.reset();
        if (extensionPageStore_)
            extensionPageStore_->SetProject(root);
        currentProjectRoot_ = root;
    }

    /** @copydoc GuiScreenHost::CurrentProjectRoot */
    std::filesystem::path GuiScreenHost::CurrentProjectRoot() const noexcept {
        if (!currentProjectRoot_.empty())
            return currentProjectRoot_;
        return std::filesystem::current_path();
    }

    Result<void> GuiScreenHost::CheckNavigationAdmission(const GuiRoute &destination) const {
        if (gameplayClosePending_)
            return Result<void>::Failure(
                MakeError(GuiLive::HostSymbol<&NavigationErrors::Busy>(), "Application gameplay retirement is still pending."));
        if (shutdown_)
            return Result<void>::Failure(MakeError(GuiLive::HostSymbol<&NavigationErrors::HostShutdown>()));
        if (!started_)
            return Result<void>::Failure(MakeError(GuiLive::HostSymbol<&NavigationErrors::HostNotStarted>()));
        if (!IsRoutePayloadValid(destination)) {
            return Result<void>::Failure(MakeError(GuiLive::HostSymbol<&NavigationErrors::InvalidRouteParameters>(),
                                                   "Invalid route payload for requested route kind."));
        }
        if (navigationBusy_) {
            return Result<void>::Failure(MakeError(GuiLive::HostSymbol<&NavigationErrors::Busy>(), "Navigation already in progress."));
        }
        if (pendingRequirement_.has_value() || pendingTarget_.has_value()) {
            return Result<void>::Failure(MakeError(GuiLive::HostSymbol<&NavigationErrors::Busy>(), "Leave resolution already pending."));
        }
        return Result<void>::Success();
    }

    Result<void> GuiScreenHost::Navigate(GuiRoute destination) {
        if (auto admitted = CheckNavigationAdmission(destination); admitted.HasError())
            return admitted;
        if (AreRoutesIdentical(activeRoute_, destination)) {
            return Result<void>::Success();
        }
        if (isScreenCallbackActive_ || fixedScreen_) {
            if (pendingNavigation_.has_value()) {
                return Result<void>::Failure(
                    MakeError(GuiLive::HostSymbol<&NavigationErrors::Busy>(), "Navigation already queued this frame."));
            }
            pendingNavigation_ = std::move(destination);
            return Result<void>::Success();
        }
        return ExecuteLeaveCheckAndCommit(LeaveTarget{destination});
    }

    Result<void> GuiScreenHost::RequestCloseApplication() {
        if (shutdown_)
            return Result<void>::Failure(MakeError(GuiLive::HostSymbol<&NavigationErrors::HostShutdown>()));
        if (!started_)
            return Result<void>::Failure(MakeError(GuiLive::HostSymbol<&NavigationErrors::HostNotStarted>()));
        if (navigationBusy_ || pendingRequirement_.has_value() || pendingTarget_.has_value()) {
            return Result<void>::Failure(MakeError(GuiLive::HostSymbol<&NavigationErrors::Busy>(), "Navigation already in progress."));
        }
        if (fixedScreen_) {
            gameplayClosePending_ = true;
            return Result<void>::Success();
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
            return Result<void>::Failure(
                MakeError(GuiLive::HostSymbol<&NavigationErrors::LeaveDenied>(), "Active screen denied leave transition."));
        }
        if (decision.disposition == LeaveDisposition::RequireResolution) {
            if (!decision.requirement.has_value()) {
                return Result<void>::Failure(MakeError(GuiLive::HostSymbol<&NavigationErrors::InvalidLeaveRequirement>(),
                                                       "Screen requested leave resolution without a requirement."));
            }
            if (resolutionAttemptCount_ >= 5) {
                return Result<void>::Failure(
                    MakeError(GuiLive::HostSymbol<&NavigationErrors::LeaveResolutionLimitExceeded>(), "Exceeded resolution attempts."));
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
        if (fixedScreen_) {
            pendingNavigation_ = std::move(destination);
            return;
        }
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

        if (const auto enterRes = EnterScreen(*candidate, destination); enterRes.HasError()) {
            LOG_ERROR("editor.screens", "Destination OnEnter failed: %s", enterRes.ErrorValue().message.c_str());
            if (activeScreen_) {
                if (const auto rollback = EnterScreen(*activeScreen_, activeRoute_); rollback.HasError()) {
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
        if (extensionPageStore_ && destination.kind != GuiRouteKind::EditorWorkspace)
            extensionPageStore_->SetProject({});
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

    Result<void> GuiScreenHost::EnterScreen(GuiScreen &screen, const GuiRoute &route) {
        if (auto entered = screen.OnEnter(route); entered.HasError())
            return entered;
        if (!dispatchSource_.IsValid())
            return Result<void>::Success();
        auto bound = screen.BindRuntimeDispatch(dispatchSource_);
        if (bound.HasError())
            screen.OnLeave();
        return bound;
    }

    std::unique_ptr<GuiScreen> GuiScreenHost::CreateScreen(const GuiRoute &route) {
        return screenRegistry_.CreateScreen(route, services_);
    }

    /** @copydoc GuiScreenHost::UpdateExtensionPages */
    void GuiScreenHost::UpdateExtensionPages() {
        if (extensionPageTheme_)
            static_cast<void>(extensionPageTheme_->Update(context_));
        if (!editorPageHost_)
            return;
        editorPageHost_->Update();
        PumpEditorPageModals(context_, editorPageHost_, extensionPageTheme_->Frame(), *modalHost_);
        if (modalHost_->HasOpenModal())
            return;
        for (const auto &page : editorPageHost_->Prepared()) {
            if (page.surface.kind == Extensions::EditorSurfaceKind::SettingsPage && page.state == Extensions::EditorPageState::Editing) {
                auto modal = CreateSettingsModal();
                modal->SelectExtensionPage(page.surface.provider, page.surface.id);
                static_cast<void>(modalHost_->OpenRoot(std::move(modal)));
                break;
            }
        }
    }

    void GuiScreenHost::UpdateBuildStatus(float dt) {
        if (!shutdown_ && localization_ && std::isfinite(dt) && dt > 0) {
            buildStatusElapsed_ += dt;
            if (buildStatusElapsed_ >= 0.25F) {
                buildStatusElapsed_ = 0;
                std::optional<Application::GameplayBuildSnapshot> build;
                if (const auto *builds = services_.TryGet<Application::GameplayBuildService>(); builds && nativeBuildSession_.has_value())
                    build = builds->Query(*nativeBuildSession_);
                const bool modalVisible = modalHost_ && dynamic_cast<const BuildModal *>(modalHost_->TopModal()) != nullptr;
                static_cast<void>(statusItemRegistry_.Update(BackgroundBuildStatusId, BackgroundBuildStatus(build ? &*build : nullptr,
                                                                                                            *localization_, modalVisible)));
            }
        }
    }

    void GuiScreenHost::UpdateExtensionCommands() {
        if (editorActivityHost_) {
            editorActivityHost_->Update();
            const auto revision = editorActivityHost_->Registry().Revision();
            if (settingsService_ && revision != extensionCommandRevision_) {
                std::vector<Input::ActionDescriptor> actions;
                for (const auto &entry : editorActivityHost_->PreparedCommands()) {
                    std::vector<Input::InputBinding> defaults;
                    if (const auto binding = ParseEditorShortcut(entry.command.shortcut))
                        defaults.push_back(*binding);
                    actions.push_back({Input::ActionId{entry.command.id.value}, Input::ActionValueType::Digital,
                                       Input::InputContextId{kEditorWorkspaceInputContext}, false, std::move(defaults)});
                }
                const auto profile = inputRouter_->Profile();
                if (const auto applied = ActivateEditorInputProfile(*inputRouter_, profile, actions); applied.HasError())
                    LOG_ERROR("editor.input", "Keeping last valid input actions after module command update: %s",
                              applied.ErrorValue().message.c_str());
                settingsService_->SetContributedInputActions(std::move(actions));
                extensionCommandRevision_ = revision;
            }
        }
    }

    void GuiScreenHost::OnUpdate(float dt) {
        if (fixedScreen_)
            return;
        if (shutdownPending_)
            Shutdown();
        if (shutdown_)
            return;
        (void)gameplayRetirements_->Drain();
        PollPlaySaveRetirement(services_);
        UpdateBuildStatus(dt);
        if (auto *audio = services_.TryGet<IAudioComponentPreview>())
            static_cast<void>(audio->Pump());
        if (context_.updates != nullptr)
            context_.updates->Poll();
        UpdateExtensionCommands();
        UpdateExtensionPages();
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

    /** @copydoc GuiScreenHost::OnInputSnapshot */
    void GuiScreenHost::OnInputSnapshot() {
        if (fixedScreen_)
            return;
        if (activeScreen_) {
            isScreenCallbackActive_ = true;
            activeScreen_->OnInputSnapshot();
            isScreenCallbackActive_ = false;
        }
        FlushPendingNavigation();
    }

    /** @copydoc GuiScreenHost::OnFixedUpdate */
    void GuiScreenHost::OnFixedUpdate(const std::uint64_t simulationTick, const double fixedDeltaSeconds) {
        if (fixedScreen_)
            return;
        (void)gameplayRetirements_->Drain();
        if (activeScreen_) {
            isScreenCallbackActive_ = true;
            activeScreen_->OnFixedUpdate(simulationTick, fixedDeltaSeconds);
            isScreenCallbackActive_ = false;
        }
        FlushPendingNavigation();
    }

    void GuiScreenHost::FlushPendingNavigation() {
        if (fixedScreen_)
            return;
        if (gameplayClosePending_ && !isScreenCallbackActive_) {
            gameplayClosePending_ = false;
            (void)RequestCloseApplication();
            return;
        }
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
        const ImGuiViewport *viewport = ImGui::GetMainViewport();
        const bool showStatusBar = activeRoute_.kind != GuiRouteKind::ProjectCreation;
        const float contentHeight = std::max(0.0F, viewport->WorkSize.y - (showStatusBar ? EditorStatusBar::Height : 0.0F));
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

        if (statusBar_ && showStatusBar) {
            const bool interactionEnabled = modalHost_ != nullptr && !modalHost_->HasOpenModal() && !pendingRequirement_.has_value();
            const auto invocation = statusBar_->Draw(ImVec2{contentRegion.x, contentRegion.y + contentRegion.height},
                                                     ImVec2{contentRegion.width, EditorStatusBar::Height},
                                                     EditorStatusBarContext{activeStatusPanelIds_}, interactionEnabled);
            if (invocation.has_value() && engineEvents_ != nullptr) {
                engineEvents_->Publish(*invocation);
                if (invocation->itemId == BackgroundBuildStatusId && invocation->actionId == BackgroundBuildActionId)
                    DispatchMenuInvocation({EditorMenuAction::OpenBuild});
            }
        }
        if (pendingRequirement_.has_value() && pendingTarget_.has_value()) {
            PresentLeaveDialog(*pendingRequirement_, *pendingTarget_);
        }
    }

    /** @copydoc GuiScreenHost::CreateSettingsModal */
    std::unique_ptr<SettingsModal> GuiScreenHost::CreateSettingsModal() {
        auto navigation =
            activeScreen_ ? activeScreen_->NavigationSettings() : Result<NavigationProjectSettingsSession *>::Success(nullptr);
        auto collision = activeScreen_ ? activeScreen_->CollisionSettings()
                                       : Result<std::shared_ptr<CollisionProjectSettingsSession>>::Failure(
                                             MakeError(CollisionSettingsError(CollisionSettingsFailure::Unsupported)));
        const auto prefab =
            activeScreen_ ? activeScreen_->PrefabSettings() : Result<std::shared_ptr<PrefabProjectSettingsSession>>::Success(nullptr);
        auto modal =
            std::make_unique<SettingsModal>(context_, *settingsService_, logoTexture_, extensionInventory_, extensionMarketplace_,
                                            SettingsModalProjectOpening{
                                                .navigation = navigation.HasValue() ? navigation.Value() : nullptr,
                                                .collision = collision.HasValue() ? std::weak_ptr{collision.Value()}
                                                                                  : std::weak_ptr<CollisionProjectSettingsSession>{},
                                                .collisionError =
                                                    collision.HasError() ? std::optional<Error>{collision.ErrorValue()} : std::nullopt,
                                                .prefab = prefab.HasValue() ? std::weak_ptr{prefab.Value()}
                                                                            : std::weak_ptr<PrefabProjectSettingsSession>{},
                                            },
                                            SettingsModalPagePresentation{editorPageHost_,
                                                                          extensionPageTheme_ ? extensionPageTheme_->Frame() : nullptr,
                                                                          editorActivityHost_});
        if (prefab.HasError())
            modal->Draft().prefab.error = prefab.ErrorValue();
        if (navigation.HasError())
            modal->Draft().navigationStatus = navigation.ErrorValue().message;
        return modal;
    }

    /** @copydoc GuiScreenHost::OpenEditorSettingsModal */
    void GuiScreenHost::OpenEditorSettingsModal() {
        if (settingsService_ && modalHost_)
            static_cast<void>(modalHost_->OpenRoot(CreateSettingsModal()));
    }

    namespace {
        /** @brief Routes workspace-owned actions to the current screen without giving a modal storage or project authority. */
        bool IsWorkspaceMenuAction(EditorMenuAction action) noexcept {
            using enum EditorMenuAction;
            switch (action) {
                case SaveScene:
                case SaveSceneAs:
                case SaveSceneCopyAs:
                case Undo:
                case Redo:
                case CreatePrimitive:
                case ManagePackages:
                case OpenSaveLoadManager:
                    return true;
                default:
                    return false;
            }
        }
    }  // namespace

    namespace {
        /** @brief Creates the real project mutation committer only when both existing authorities are present. */
        std::shared_ptr<Assets::IAssetImportCommitter> ImportCommitter(const EditorServiceRegistry &services) {
            auto *mutations = services.TryGet<ProjectMutationCoordinator>();
            auto *files = services.TryGet<DurableFileSystem>();
            if (!mutations || !files)
                return {};
            return std::make_shared<Application::ProjectAssetImportCommitter>(*mutations, *files, services.TryGet<Assets::AssetRegistry>());
        }

        /** @brief Inert borrowed import composition; no committer is allocated until actual modal admission. */
        struct ImportWorkflowServices final {
            const EditorServiceRegistry &registry;
            AssetImportModalServices modal;
        };

        /** @brief Opens the existing import workflow with its exact captured provider/source bindings. */
        void OpenImportWorkflow(EditorModalHost *host, const Theme::Fonts &fonts, JobSystem &jobs,
                                std::shared_ptr<const Assets::AssetImporterCatalogSnapshot> catalog, ImportWorkflowServices services,
                                const std::filesystem::path &project, const std::optional<std::filesystem::path> &destination) {
            if (!host || host->HasOpenModal())
                return;
            services.modal.committer = ImportCommitter(services.registry);
            auto modal = std::make_unique<AssetImportModal>(fonts, jobs, std::move(catalog), std::move(services.modal));
            modal->SetProjectRoot(project);
            if (destination)
                modal->SetDefaultDestination(*destination);
            static_cast<void>(host->OpenRoot(std::move(modal)));
        }

        /** @brief Routes the existing build/release action family without changing native job or modal ownership. */
        bool OpenBuildWorkflow(const EditorMenuInvocation &invocation, EditorModalHost *host, const EditorGuiContext &context,
                               const EditorServiceRegistry &services, const GuiRoute &route, const std::filesystem::path &project,
                               std::optional<std::uint64_t> &retainedBuild) {
            using enum EditorMenuAction;
            if (invocation.action != OpenBuild && invocation.action != OpenRelease)
                return false;
            if (!host || host->HasOpenModal())
                return true;
            const auto *environment = services.TryGetConst<Application::GameplayBuildEnvironment>();
            Application::GameplayBuildRequest request{.projectRoot =
                                                          route.kind == GuiRouteKind::EditorWorkspace ? project : std::filesystem::path{},
                                                      .environment = environment ? *environment : Application::GameplayBuildEnvironment{}};
            std::unique_ptr<EditorModal> modal;
            if (invocation.action == OpenBuild)
                modal = std::make_unique<BuildModal>(context, std::move(request), services.TryGet<Application::GameplayBuildService>(),
                                                     retainedBuild, services.TryGetConst<IBuildOutputQuery>());
            else
                modal = std::make_unique<BuildReleaseModal>(context, BuildReleasePage::Release, std::move(request),
                                                            services.TryGet<Application::GameplayBuildService>(), retainedBuild,
                                                            services.TryGet<IBuildReleaseJobs>());
            if (const auto opened = host->OpenRoot(std::move(modal)); opened.HasError())
                LOG_WARN("editor.build", "Build/release modal admission failed: %s", opened.ErrorValue().code.Value().c_str());
            return true;
        }
    }  // namespace

    /** @copydoc GuiScreenHost::DispatchMenuInvocation */
    void GuiScreenHost::DispatchMenuInvocation(const EditorMenuInvocation &invocation) {
        if (IsWorkspaceMenuAction(invocation.action)) {
            if (activeScreen_)
                static_cast<void>(activeScreen_->HandleMenuInvocation(invocation));
            return;
        }
        if (OpenBuildWorkflow(invocation, modalHost_, context_, services_, activeRoute_, currentProjectRoot_, nativeBuildSession_))
            return;
        using enum EditorMenuAction;
        switch (invocation.action) {
            case NewProject:
                static_cast<void>(Navigate(GuiRoute{GuiRouteKind::ProjectCreation, ProjectCreationRouteParameters{}}));
                return;
            case OpenProject:
                static_cast<void>(Navigate(GuiRoute{GuiRouteKind::Welcome, WelcomeRouteParameters{}}));
                return;
            case ImportAssets:
                OpenImportWorkflow(modalHost_, context_.theme.fonts, m_importJobs, importerCatalog_,
                                   {services_,
                                    {.assetRegistry = services_.TryGet<Assets::AssetRegistry>(),
                                     .operationStore = services_.TryGet<OperationStore>(),
                                     .localization = localization_,
                                     .nativeDialogs = nativeDialogs_,
                                     .inputRouter = inputRouter_}},
                                   currentProjectRoot_, invocation.assetDestination);
                return;
            case OpenEditorSettings:
                OpenEditorSettingsModal();
                return;
            case ExitApplication:
                static_cast<void>(RequestCloseApplication());
                return;
            case None:
            default:
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
        if (isScreenCallbackActive_ || fixedScreen_) {
            gameplayClosePending_ = true;
            return Result<void>::Success();
        }
        if (modalHost_->HasOpenModal()) {
            const Result<void> modalClosed = modalHost_->RequestCloseAllForShutdown();
            if (modalClosed.HasError()) {
                rendererRestartRequest_.reset();
                closeRequested_ = false;
                return modalClosed;
            }
        }
        if (activeScreen_) {
            activeScreen_->OnLeave();
            activeScreen_.reset();
        }
        if (auto closed = gameplayRetirements_->Close(); closed.HasError()) {
            gameplayClosePending_ = true;
            return closed;
        }
        gameplayClosePending_ = false;
        closeRequested_ = true;
        return Result<void>::Success();
    }

    /** @copydoc DrawLeaveConfirmation */
    std::optional<LeaveAction> DrawLeaveConfirmation(const LeaveRequirement &requirement, const EditorGuiContext &context) {
        static constexpr GuiLocalizationScope scope{"leave_confirmation"};
        const auto text = [&](const char *key) -> const std::string & {
            return scope.Text(context.localization, key);
        };
        const char *messageKey = "dirty_document";
        const char *titleKey = "title";
        switch (requirement.kind) {
            case LeaveRequirementKind::UnsavedDraft:
                messageKey = "unsaved_draft";
                break;
            case LeaveRequirementKind::RunningOperation:
                messageKey = "running_operation";
                titleKey = "operation_title";
                break;
            case LeaveRequirementKind::RecoveryDecision:
                messageKey = "recovery";
                break;
            case LeaveRequirementKind::NativeDialogCompletion:
                messageKey = "native_dialog";
                break;
            case LeaveRequirementKind::DirtyDocument:
                break;
        }
        const auto allowed = [&](const LeaveAction action) {
            return std::ranges::find(requirement.allowedActions, action) != requirement.allowedActions.end();
        };
        const auto &fonts = context.theme.fonts;
        const float scale = Theme::GetActiveTokens().sizes.uiScale;
        const float width =
            std::max(1.0F, std::min(Ui::ScaledLayoutValue(520), ImGui::GetMainViewport()->WorkSize.x - Ui::ScaledLayoutValue(32)));
        const float inset = Ui::ScaledLayoutValue(20);
        const float gap = Ui::ScaledLayoutValue(8);
        const auto metrics = DesignSystem::MetricsFor(Theme::GetActiveTokens(), Ui::ComponentSize::Medium);
        constexpr std::array actions{LeaveAction::Discard, LeaveAction::CancelOperation, LeaveAction::KeepRunning, LeaveAction::Wait,
                                     LeaveAction::Save};
        std::array<float, actions.size()> widths{};
        float totalWidth = 0;
        int count = 0;
        {
            Theme::ScopedTextStyle style(fonts.sans, metrics.fontSize, Theme::FontPx::Sans);
            for (std::size_t index = 0; index < actions.size(); ++index) {
                if (!allowed(actions[index]))
                    continue;
                widths[index] = ImGui::CalcTextSize(text(LeaveActionKey(actions[index])).c_str()).x + metrics.paddingX * 2;
                totalWidth += widths[index] + (count++ != 0 ? gap : 0);
            }
        }
        const bool stacked = totalWidth > std::max(1.0F, width - inset * 2);
        const float footerHeight =
            Ui::ScaledLayoutValue(24) + metrics.minimumHeight * (stacked ? count : 1) + (stacked ? std::max(0, count - 1) * gap : 0);
        float bodyHeight = 0;
        {
            Theme::ScopedTextStyle style(fonts.sans, Theme::TextPx::Body(), Theme::FontPx::Sans);
            bodyHeight = ImGui::CalcTextSize(text(messageKey).c_str(), nullptr, false, std::max(1.0F, width - inset * 2 - 1)).y +
                         Ui::ScaledLayoutValue(32);
        }
        std::optional<LeaveAction> selected;
        {
            Ui::ScopedModalShell shell({.id = "LeaveConfirmation",
                                        .title = text(titleKey).c_str(),
                                        .requestedSize = {520, (Ui::ScaledLayoutValue(38) + bodyHeight + footerHeight) / scale},
                                        .viewportPadding = 32,
                                        .minimumWidth = 240,
                                        .minimumHeight = 120,
                                        .headerHeight = 38,
                                        .headerHorizontalPadding = 16,
                                        .footerHeight = footerHeight / scale,
                                        .showClose = allowed(LeaveAction::Stay),
                                        .foregroundBorder = true,
                                        .closeButtonVariant = Ui::CloseButtonVariant::Compact,
                                        .titleFontSize = Theme::TextPx::Label(),
                                        .modalPopup = true},
                                       fonts);
            if (!shell.IsOpen())
                return std::nullopt;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {inset, Ui::ScaledLayoutValue(16)});
            ImGui::BeginChild("##LeaveBody", {0, shell.BodyHeight()}, false, ImGuiWindowFlags_AlwaysUseWindowPadding);
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, text(messageKey), Theme::Text(), fonts);
            ImGui::EndChild();
            ImGui::PopStyleVar();
            shell.BeginFooter({inset, Ui::ScaledLayoutValue(12)});
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {gap, gap});
            float x = ImGui::GetCursorPosX() + std::max(0.0F, ImGui::GetContentRegionAvail().x - totalWidth);
            int drawn = 0;
            for (std::size_t index = 0; index < actions.size(); ++index) {
                if (!allowed(actions[index]))
                    continue;
                if (stacked)
                    x = ImGui::GetCursorPosX() + std::max(0.0F, ImGui::GetContentRegionAvail().x - widths[index]);
                else if (drawn != 0)
                    ImGui::SameLine(0, gap);
                ImGui::SetCursorPosX(x);
                if (const auto action = actions[index];
                    Ui::Button({.label = text(LeaveActionKey(action)).c_str(),
                                .variant = action == LeaveAction::Save ? Ui::ButtonVariant::Primary : Ui::ButtonVariant::Secondary,
                                .font = fonts.sans}))
                    selected = action;
                x += widths[index] + gap;
                ++drawn;
            }
            ImGui::PopStyleVar();
            shell.EndFooter();
            if (allowed(LeaveAction::Stay) && (shell.CloseRequested() || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
                selected = LeaveAction::Stay;
            if (selected)
                ImGui::CloseCurrentPopup();
        }
        return selected;
    }

    /** @copydoc GuiScreenHost::PresentLeaveDialog */
    void GuiScreenHost::PresentLeaveDialog(const LeaveRequirement &requirement, const LeaveTarget &target) {
        if (const auto action = DrawLeaveConfirmation(requirement, context_))
            ExecuteLeaveResolution(*action, requirement, target);
    }

}  // namespace Horo::Editor
