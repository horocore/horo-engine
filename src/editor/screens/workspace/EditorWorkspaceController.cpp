#include "editor/screens/workspace/EditorWorkspaceController.h"

#include "Horo/Assets/AssetReimport.h"
#include "Horo/Editor/EditorWorkspaceEvents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "Horo/Editor/ProjectIntegrityValidatorService.h"
#include "Horo/Editor/WorkspacePanelRegistry.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/PathUtils.h"
#include "Horo/Gameplay/GameplayErrors.h"
#include "editor/document/EditorViewportPicking.h"
#include "editor/document/RuntimeSceneConversion.h"
#include "editor/document/SceneDocumentComparison.h"
#include "editor/document/SceneDocumentPersistence.h"
#include "editor/menu/EditorMenuPlatform.h"
#include "editor/project_model/EditorModelErrors.h"
#include "editor/screens/workspace/GameplayBehaviorRequestValidation.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <nlohmann/json.hpp>
#include <random>
#include <span>
#include <utility>
#include <vector>

namespace Horo::Editor {
    namespace {
        const ErrorDomainId SceneComparisonCaptureDomain{"horo.editor.scene_comparison_capture"};
        const ErrorCodeDescriptor SceneComparisonUnavailable{
            .domain = SceneComparisonCaptureDomain,
            .code = ErrorCode{"scene_comparison_capture.unavailable"},
            .defaultSeverity = ErrorSeverity::Error,
            .summary = "The active scene is unavailable for comparison.",
        };

        [[nodiscard]] std::string ReplaceMessageToken(std::string message, const std::string_view token, const std::string_view value) {
            std::size_t offset = 0;
            while ((offset = message.find(token, offset)) != std::string::npos) {
                message.replace(offset, token.size(), value);
                offset += value.size();
            }
            return message;
        }

        [[nodiscard]] std::filesystem::path ResolveProjectRoot(const std::filesystem::path &projectRoot) {
            std::error_code error;
            std::filesystem::path resolved = std::filesystem::absolute(projectRoot, error).lexically_normal();
            if (error) {
                error.clear();
                resolved = (std::filesystem::current_path(error) / projectRoot).lexically_normal();
            }
            if (error)
                return resolved;

            const std::filesystem::path canonical = std::filesystem::weakly_canonical(resolved, error);
            return error ? resolved : canonical;
        }
    }  // namespace

    EditorWorkspaceController::EditorWorkspaceController(const std::filesystem::path &projectRoot,
                                                         Runtime::RuntimeSceneService &runtimeScene,
                                                         const Assets::AssetRegistrySnapshot &assetRegistry,
                                                         const EditorWorkspaceDependencies &dependencies)
        : m_runtimeScene(runtimeScene), m_assetRegistry(assetRegistry), m_sourceOpenService(projectRoot, m_documentRegistry),
          m_mutableAssetRegistry(dependencies.mutableAssetRegistry), m_mutations(dependencies.mutations),
          m_durableFiles(dependencies.durableFiles), m_importerCatalog(dependencies.importerCatalog),
          m_assetPreviews(dependencies.jobs != nullptr ? std::make_unique<Assets::AssetPreviewService>(*dependencies.jobs) : nullptr),
          m_sourceOpenNavigator(dependencies.sourceOpenNavigator), m_diagnosticSourceNavigator(dependencies.diagnosticSourceNavigator),
          m_gameplayBuilds(dependencies.gameplayBuilds), m_gameplayBuildEnvironment(dependencies.gameplayBuildEnvironment),
          m_localization(dependencies.localization),
          m_sceneFileWatch(dependencies.jobs != nullptr ? std::make_unique<SceneFileWatchService>(*dependencies.jobs) : nullptr) {
        if (dependencies.engineEvents != nullptr) {
            m_engineEventBridge = std::make_unique<EditorEngineEventBridge>(*dependencies.engineEvents, m_dataBus);
            m_engineEventBridge->Attach();
        }
        m_viewModel.workspacePanelHost.AttachDocumentIdentityRegistry(m_documentRegistry);
        if (!m_diagnosticSourceNavigator) {
            m_diagnosticSourceNavigator = [](const DiagnosticSourceRequest &source) {
                const std::filesystem::path path{source.absolutePath};
                return OpenInExternalEditor(path) || RevealInNativeFileManager(path);
            };
        }
        const std::filesystem::path absoluteProjectRoot = ResolveProjectRoot(projectRoot);
        InitializeProjectState(absoluteProjectRoot);
        InitializeWorkspaceLayout();
        InitializeInitialScene(absoluteProjectRoot);
        RefreshSceneProjections();
    }

    void EditorWorkspaceController::InitializeProjectState(const std::filesystem::path &absoluteProjectRoot) {
        m_viewModel.projectRoot = absoluteProjectRoot.string();
        m_viewModel.assetRegistryRevision = m_assetRegistry.Revision();
        RebuildContentBrowserProjection(m_viewModel.projectRoot, {});
        if (m_durableFiles != nullptr) {
            ProjectIntegrityValidatorService validator{*m_durableFiles};
            if (const Result<void> repaired = validator.Repair(absoluteProjectRoot); repaired.HasError())
                LOG_ERROR("editor.project_validator", "Project integrity repair failed for '%s': %s", absoluteProjectRoot.string().c_str(),
                          repaired.ErrorValue().message.c_str());
        }
        m_gameplayRegistry = ProjectGameplayRegistry::Discover(absoluteProjectRoot);
        for (const ProjectGameplayDiagnostic &diagnostic : m_gameplayRegistry->Diagnostics())
            LOG_ERROR("editor.gameplay", "Gameplay source '%s' is invalid: %s", diagnostic.source.string().c_str(),
                      diagnostic.error.message.c_str());
        for (const Gameplay::BehaviorRegistration &registration : m_gameplayRegistry->Registry().Registrations())
            m_viewModel.availableBehaviors.push_back(registration.descriptor);
    }

    void EditorWorkspaceController::InitializeWorkspaceLayout() {
        m_viewModel.panelDockAreas = {{"horo.hierarchy", WorkspaceDockArea::Left},  {"horo.viewport", WorkspaceDockArea::Document},
                                      {"horo.game", WorkspaceDockArea::Document},   {"horo.global_dock", WorkspaceDockArea::Bottom},
                                      {"horo.inspector", WorkspaceDockArea::Right}, {"horo.input_mapping", WorkspaceDockArea::Right}};
        static_cast<void>(m_viewModel.activityBarLayout.Insert("horo.hierarchy", ActivityBarSlot{ActivityBarRail::Left, 0, 0}));
        static_cast<void>(m_viewModel.activityBarLayout.Insert("horo.viewport", ActivityBarSlot{ActivityBarRail::DocumentTop, 0, 0}));
        static_cast<void>(m_viewModel.activityBarLayout.Insert("horo.game", ActivityBarSlot{ActivityBarRail::DocumentTop, 0, 1}));
        static_cast<void>(m_viewModel.activityBarLayout.Insert("horo.global_dock", ActivityBarSlot{ActivityBarRail::Left, 2, 0}));
        static_cast<void>(m_viewModel.activityBarLayout.Insert("horo.inspector", ActivityBarSlot{ActivityBarRail::Right, 0, 0}));
        static_cast<void>(m_viewModel.activityBarLayout.Insert("horo.input_mapping", ActivityBarSlot{ActivityBarRail::Right, 1, 0}));
    }

    void EditorWorkspaceController::InitializeInitialScene(const std::filesystem::path &absoluteProjectRoot) {
        if (!LoadInitialScene(absoluteProjectRoot) && !m_initializationError.has_value())
            CreateBootstrapScene();
        if (m_defaultScenePath.has_value())
            InspectInitialSceneRecovery(absoluteProjectRoot);
    }

    bool EditorWorkspaceController::LoadInitialScene(const std::filesystem::path &absoluteProjectRoot) {
        const Result<std::optional<LoadedProjectScene>> loaded = LoadProjectDefaultScene(absoluteProjectRoot);
        if (loaded.HasError()) {
            m_initializationError = loaded.ErrorValue();
            LOG_ERROR("editor.scene_document", "Default scene load failed: %s", loaded.ErrorValue().message.c_str());
            return false;
        }
        if (!loaded.Value().has_value())
            return false;
        LoadedProjectScene projectScene = *loaded.Value();
        if (const Result<void> installed = m_document.LoadSaved(std::move(projectScene.objects), std::move(projectScene.prefabInstances));
            installed.HasError()) {
            m_initializationError = installed.ErrorValue();
            LOG_ERROR("editor.scene_document", "Default scene validation failed: %s", installed.ErrorValue().message.c_str());
            return false;
        }
        m_defaultScenePath = std::move(projectScene.absolutePath);
        m_sceneFingerprint = std::move(projectScene.fingerprint);
        m_history.Clear();
        LOG_INFO("editor.scene_document", "Loaded default scene '%s'.", m_defaultScenePath->string().c_str());
        return true;
    }

    void EditorWorkspaceController::CreateBootstrapScene() {
        const Math::Quaternion pitch = Math::Quaternion::FromAxisAngle({1.0F, 0.0F, 0.0F}, -0.42F);
        const Math::Quaternion yaw = Math::Quaternion::FromAxisAngle({0.0F, 1.0F, 0.0F}, 0.55F);
        if (const Result<SceneCommandResult> created = m_documentCommands.Execute(CreateSceneObjectCommand{
                .name = "Box",
                .localTransform = Math::Transform{.rotation = pitch * yaw},
                .primitiveMesh = PrimitiveMeshDescriptor{},
            });
            created.HasError()) {
            LOG_ERROR("editor.scene_document", "Bootstrap scene creation failed: %s", created.ErrorValue().message.c_str());
            return;
        }
        static_cast<void>(m_document.MarkSaved(m_document.Revision(), m_document.State()));
        m_history.Clear();
    }

    void EditorWorkspaceController::InspectInitialSceneRecovery(const std::filesystem::path &absoluteProjectRoot) {
        const Result<std::optional<ProjectSceneRecoveryRecord>> recovery =
            InspectProjectSceneRecovery(absoluteProjectRoot, *m_defaultScenePath);
        if (recovery.HasError()) {
            LOG_ERROR("editor.scene_recovery", "Recovery inspection failed: %s", recovery.ErrorValue().message.c_str());
            return;
        }
        if (recovery.Value().has_value()) {
            m_viewModel.recoveryAvailable = true;
            LOG_WARN("editor.scene_recovery", "Validated recovery is available for '%s'; canonical scene was not modified.",
                     m_defaultScenePath->string().c_str());
        }
    }

    /** @copydoc EditorWorkspaceController::UpdateExternalSceneWatch */
    void EditorWorkspaceController::UpdateExternalSceneWatch(const float elapsedSeconds) {
        if (m_sceneFileWatch == nullptr || !m_defaultScenePath.has_value() || !m_sceneFingerprint.has_value())
            return;

        for (SceneFileWatchUpdate &update : m_sceneFileWatch->DrainUpdates()) {
            if (update.error.has_value()) {
                if (!m_sceneFileWatchErrorPresented) {
                    LOG_WARN("editor.scene_document", "Background scene inspection failed for '%s': %s",
                             m_defaultScenePath->string().c_str(), update.error->message.c_str());
                    m_sceneFileWatchErrorPresented = true;
                }
                continue;
            }
            if (!update.fingerprint.has_value())
                continue;
            m_sceneFileWatchErrorPresented = false;
            const bool conflict = *update.fingerprint != *m_sceneFingerprint;
            if (conflict && !m_viewModel.sceneExternalConflict) {
                LOG_WARN("editor.scene_document", "Canonical scene changed outside this document session: '%s'.",
                         m_defaultScenePath->string().c_str());
            }
            m_viewModel.sceneExternalConflict = conflict;
        }

        if (!std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0F || m_sceneFileWatch->HasPendingInspection())
            return;
        m_sceneFileWatchElapsedSeconds += elapsedSeconds;
        if (m_sceneFileWatchElapsedSeconds < 1.0F)
            return;
        m_sceneFileWatchElapsedSeconds = 0.0F;
        const Result<std::uint64_t> requested =
            m_sceneFileWatch->Request(std::filesystem::path{m_viewModel.projectRoot}, *m_defaultScenePath);
        if (requested.HasError() && !m_sceneFileWatchErrorPresented) {
            LOG_WARN("editor.scene_document", "Background scene inspection could not be scheduled: %s",
                     requested.ErrorValue().message.c_str());
            m_sceneFileWatchErrorPresented = true;
        }
    }

    /** @copydoc EditorWorkspaceController::CaptureExternalSceneComparison */
    Result<SceneDocumentComparisonRequest> EditorWorkspaceController::CaptureExternalSceneComparison() const {
        if (!m_defaultScenePath.has_value())
            return Result<SceneDocumentComparisonRequest>::Failure(MakeError(SceneComparisonUnavailable));
        return Result<SceneDocumentComparisonRequest>::Success({
            .absoluteProjectRoot = std::filesystem::path{m_viewModel.projectRoot},
            .absoluteScenePath = *m_defaultScenePath,
            .document = m_document.Snapshot(),
        });
    }

    void EditorWorkspaceController::UpdateFps(const float fps) {
        m_viewModel.fps = fps;
    }

    void EditorWorkspaceController::UpdateGameplaySources(const float elapsedSeconds) {
        UpdateGameplayBuild(elapsedSeconds);
        if (m_nativeGameplayReloadQuarantined || m_gameplayRegistry == nullptr || !std::isfinite(elapsedSeconds) || elapsedSeconds <= 0.0F)
            return;
        m_gameplaySourceWatchElapsedSeconds += elapsedSeconds;
        if (m_gameplaySourceWatchElapsedSeconds < 0.5F)
            return;
        m_gameplaySourceWatchElapsedSeconds = 0.0F;
        for (const ProjectGameplayDiagnostic &diagnostic : m_gameplayRegistry->ReloadChangedLuaSources())
            LOG_ERROR("editor.gameplay", "Lua reload kept the last working revision for '%s': %s", diagnostic.source.string().c_str(),
                      diagnostic.error.message.c_str());
        if (!m_gameplayRegistry->ConsumeNativeArtifactChange())
            return;
        if (m_playSession.IsActive()) {
            m_nativeGameplayReloadPending = true;
            return;
        }
        std::unique_ptr<ProjectGameplayRegistry> candidate = ProjectGameplayRegistry::Discover(m_viewModel.projectRoot);
        if (candidate->HasBlockingDiagnostics()) {
            for (const ProjectGameplayDiagnostic &diagnostic : candidate->Diagnostics())
                LOG_ERROR("editor.gameplay", "Native reload kept the last working module for '%s': %s", diagnostic.source.string().c_str(),
                          diagnostic.error.message.c_str());
            return;
        }
        m_gameplayRegistry = std::move(candidate);
        RefreshAvailableBehaviorProjection();
    }

    /** @copydoc EditorWorkspaceController::RefreshAssets */

    /** @copydoc EditorWorkspaceController::UpdateContentBrowser */

    std::string EditorWorkspaceController::Localized(const std::string_view key, const std::string_view fallback) const {
        return m_localization != nullptr ? m_localization->Get("editor", key) : std::string{fallback};
    }

    std::vector<SceneObjectId> EditorWorkspaceController::CollectExistingDeleteObjects(const std::vector<SceneObjectId> &objects,
                                                                                       std::string &singleName,
                                                                                       std::size_t &requestedCount) const {
        std::vector<SceneObjectId> requested;
        requested.reserve(objects.size());
        for (const SceneObjectId object : objects) {
            if (object.IsValid() && std::ranges::find(requested, object) == requested.end())
                requested.push_back(object);
        }
        requestedCount = requested.size();

        std::vector<SceneObjectId> existing;
        existing.reserve(requested.size());
        const std::span<const SceneObjectSnapshot> documentObjects = m_document.Objects();
        for (const SceneObjectId object : requested) {
            const auto found = std::ranges::find(documentObjects, object, &SceneObjectSnapshot::id);
            if (found == documentObjects.end())
                continue;
            existing.push_back(object);
            if (existing.size() == 1)
                singleName = found->name;
        }
        return existing;
    }

    void EditorWorkspaceController::HandleDeleteSelectedObjects(const std::vector<SceneObjectId> &objects) {
        std::string singleName;
        std::size_t requestedCount = 0;
        std::vector<SceneObjectId> existing = CollectExistingDeleteObjects(objects, singleName, requestedCount);
        const std::size_t skipped = requestedCount - existing.size();
        if (existing.empty()) {
            m_notifications.Publish("scene", NotificationSeverity::Warning,
                                    Localized("workspace.hierarchy.delete_blocked", "Selected objects cannot be deleted."),
                                    Localized("workspace.hierarchy.delete_failed_title", "Delete failed"), "scene_delete_blocked");
            return;
        }

        Result<SceneCommandResult> result = m_documentCommands.Execute(DeleteSceneObjectsCommand{existing});
        if (result.HasError()) {
            HandleDocumentCommandResult(result, "Delete selected objects");
            m_notifications.Publish("scene", NotificationSeverity::Error,
                                    Localized("workspace.hierarchy.delete_blocked", "Selected objects cannot be deleted."),
                                    Localized("workspace.hierarchy.delete_failed_title", "Delete failed"), "scene_delete_failed");
            return;
        }
        const bool committed = result.Value().committed;
        HandleDocumentCommandResult(result, "Delete selected objects");
        if (!committed)
            return;

        std::string message;
        NotificationSeverity severity = NotificationSeverity::Success;
        if (skipped > 0) {
            message =
                Localized("workspace.hierarchy.delete_partial", "{deletedCount} objects deleted, {skippedCount} could not be deleted.");
            message = ReplaceMessageToken(std::move(message), "{deletedCount}", std::to_string(existing.size()));
            message = ReplaceMessageToken(std::move(message), "{skippedCount}", std::to_string(skipped));
            severity = NotificationSeverity::Warning;
        } else if (existing.size() == 1) {
            message = Localized("workspace.hierarchy.delete_success_single", "\"{name}\" deleted.");
            message = ReplaceMessageToken(std::move(message), "{name}", singleName);
        } else {
            message = Localized("workspace.hierarchy.delete_success_multiple", "{count} objects deleted.");
            message = ReplaceMessageToken(std::move(message), "{count}", std::to_string(existing.size()));
        }
        m_notifications.Publish("scene", severity, std::move(message),
                                Localized("workspace.hierarchy.delete_success_title", "Objects deleted"), "scene_delete_result");
    }

}  // namespace Horo::Editor
