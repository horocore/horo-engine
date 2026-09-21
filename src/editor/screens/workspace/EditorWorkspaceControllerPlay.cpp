#include "Horo/Foundation/Logging/Logger.h"
#include "editor/document/RuntimeSceneConversion.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] Math::Transform ResolveRuntimeEntityTransform(const Runtime::RuntimeSceneView &runtimeView,
                                                                    const Runtime::RuntimeEntityView &entity) {
            if (!entity.authoredObject.has_value())
                return *entity.localTransform;
            const Result<SceneObjectWorldTransforms> world =
                ResolveSceneObjectWorldTransforms(runtimeView, SceneObjectId{entity.authoredObject->value});
            if (world.HasError())
                return *entity.localTransform;
            if (const Result<Math::Transform> decomposed = Math::TryDecomposeAffineTRS(world.Value().localToWorld); decomposed.HasValue()) {
                return decomposed.Value();
            }
            return *entity.localTransform;
        }
    }  // namespace

    Application::GameplayBuildRequest EditorWorkspaceController::MakeGameplayBuildRequest() const {
        return Application::GameplayBuildRequest{
            .projectRoot = m_viewModel.projectRoot,
            .environment = m_gameplayBuildEnvironment,
        };
    }

    bool EditorWorkspaceController::HasNativeGameplaySources() const {
        const std::filesystem::path sourceRoot = std::filesystem::path{m_viewModel.projectRoot} / "source" / "gameplay";
        std::error_code error;
        if (!std::filesystem::is_directory(sourceRoot, error))
            return false;
        std::filesystem::recursive_directory_iterator iterator{sourceRoot, error};
        const std::filesystem::recursive_directory_iterator end;
        while (iterator != end && !error) {
            const std::filesystem::directory_entry entry = *iterator;
            iterator.increment(error);
            if (!entry.is_regular_file(error))
                continue;
            const std::string extension = entry.path().extension().string();
            if (extension == ".c" || extension == ".cc" || extension == ".cpp" || extension == ".cxx" || extension == ".ixx" ||
                extension == ".cppm")
                return true;
        }
        return false;
    }

    void EditorWorkspaceController::StartGameplayBuild(const bool playWhenReady) {
        LOG_INFO("editor.gameplay", "StartGameplayBuild requested (playWhenReady=%s, project='%s').", playWhenReady ? "true" : "false",
                 m_viewModel.projectRoot.c_str());
        if (m_gameplayBuilds == nullptr || m_gameplayBuildEnvironment.gameplaySdkPackage.empty()) {
            LOG_ERROR("editor.gameplay", "Gameplay build failed to start: build service or SDK package is unavailable.");
            if (playWhenReady) {
                m_viewModel.playState = EditorPlayState::Failed;
                m_viewModel.playError = "Native gameplay build service is unavailable.";
            }
            return;
        }
        const Result<Application::GameplayBuildSessionId> started = m_gameplayBuilds->Start(MakeGameplayBuildRequest());
        if (started.HasError()) {
            LOG_ERROR("editor.gameplay", "Gameplay build service failed to start build: %s", started.ErrorValue().message.c_str());
            if (playWhenReady) {
                m_viewModel.playState = EditorPlayState::Failed;
                m_viewModel.playError = started.ErrorValue().message;
            }
            return;
        }
        m_gameplayBuildSession = started.Value();
        LOG_INFO("editor.gameplay", "Gameplay build session #%" PRIu64 " started.", *m_gameplayBuildSession);
        m_playAfterGameplayBuild = m_playAfterGameplayBuild || playWhenReady;
        if (playWhenReady) {
            m_prePlayDocumentPanelId = m_viewModel.activeDocumentPanelId;
            m_viewModel.activeDocumentPanelId = "horo.game";
            m_viewModel.playState = EditorPlayState::Starting;
            m_viewModel.playError.clear();
        }
    }

    void EditorWorkspaceController::UpdateGameplayBuild(const float elapsedSeconds) {
        if (m_nativeBuildDebounceSeconds >= 0.0F && std::isfinite(elapsedSeconds) && elapsedSeconds > 0.0F) {
            m_nativeBuildDebounceSeconds -= elapsedSeconds;
            if (m_nativeBuildDebounceSeconds <= 0.0F) {
                m_nativeBuildDebounceSeconds = -1.0F;
                StartGameplayBuild(false);
            }
        }
        if (!m_gameplayBuildSession.has_value() || m_gameplayBuilds == nullptr)
            return;
        const std::optional<Application::GameplayBuildSnapshot> snapshot = m_gameplayBuilds->Query(*m_gameplayBuildSession);
        if (!snapshot.has_value())
            return;
        switch (snapshot->state) {
            case Application::GameplayBuildState::Succeeded: {
                LOG_INFO("editor.gameplay", "Gameplay build session #%" PRIu64 " succeeded.", *m_gameplayBuildSession);
                const bool startPlay = m_playAfterGameplayBuild;
                m_gameplayBuildSession.reset();
                m_playAfterGameplayBuild = false;
                RefreshGameplayRegistry();
                if (startPlay)
                    BeginPlaySession();
                break;
            }
            case Application::GameplayBuildState::Failed:
            case Application::GameplayBuildState::Cancelled:
            case Application::GameplayBuildState::TimedOut: {
                const std::string errorMsg = snapshot->error.has_value() ? snapshot->error->message : "Gameplay build failed.";
                LOG_ERROR("editor.gameplay", "Gameplay build session #%" PRIu64 " %s: %s", *m_gameplayBuildSession,
                          snapshot->state == Application::GameplayBuildState::Failed      ? "failed"
                          : snapshot->state == Application::GameplayBuildState::Cancelled ? "was cancelled"
                                                                                          : "timed out",
                          errorMsg.c_str());
                m_gameplayBuildSession.reset();
                if (m_playAfterGameplayBuild) {
                    m_playAfterGameplayBuild = false;
                    m_viewModel.playState = EditorPlayState::Failed;
                    m_viewModel.playError = errorMsg;
                }
                break;
            }
            default:
                break;
        }
    }

    void EditorWorkspaceController::StartPlaySession() {
        LOG_INFO("editor.play_session", "StartPlaySession initiated for project '%s'.", m_viewModel.projectRoot.c_str());
        if (m_nativeGameplayReloadQuarantined) {
            m_viewModel.playState = EditorPlayState::Failed;
            m_viewModel.playError = MakeError(Gameplay::GameplayErrors::GameplayReloadRestartRequired).message;
            LOG_ERROR("editor.play_session", "%s", m_viewModel.playError.c_str());
            return;
        }
        if (HasNativeGameplaySources() && (m_gameplayBuilds == nullptr || !m_gameplayBuilds->IsUpToDate(MakeGameplayBuildRequest()))) {
            LOG_INFO("editor.play_session", "Native gameplay build required before play. Starting gameplay build...");
            StartGameplayBuild(true);
            return;
        }
        RefreshGameplayRegistry();
        BeginPlaySession();
    }

    void EditorWorkspaceController::BeginPlaySession() {
        LOG_INFO("editor.play_session", "Beginning play session for '%s'...", m_viewModel.projectRoot.c_str());
        if (m_gameplayRegistry == nullptr)
            m_gameplayRegistry = ProjectGameplayRegistry::Discover(m_viewModel.projectRoot);
        if (m_gameplayRegistry->HasBlockingDiagnostics()) {
            const std::string diagMsg = m_gameplayRegistry->Diagnostics().front().error.message;
            LOG_ERROR("editor.play_session", "Play session blocked by gameplay diagnostic: %s", diagMsg.c_str());
            m_viewModel.playState = EditorPlayState::Failed;
            m_viewModel.playError = diagMsg;
            m_prePlayDocumentPanelId = m_viewModel.activeDocumentPanelId;
            m_viewModel.activeDocumentPanelId = "horo.game";
            m_notifications.Publish("gameplay", NotificationSeverity::Error, diagMsg, "Play session blocked", "play_blocked", 0.0F,
                                    {{"Open logs", "open_logs"}});
            return;
        }

        m_prePlayDocumentPanelId = m_viewModel.activeDocumentPanelId;
        std::unique_ptr<Runtime::RuntimeScene> preparedScene;
        if (auto cloned = m_runtimeScene.CloneActive(Runtime::SceneRuntimeId{0x8000000000000001ULL}); cloned.HasValue())
            preparedScene = std::move(cloned).Value();
        const Result<void> started = m_playSession.Start(m_document.Snapshot(), m_gameplayRegistry->Registry(),
                                                         m_gameplayRegistry->Components(), std::move(preparedScene));
        m_viewModel.activeDocumentPanelId = "horo.game";
        if (started.HasError())
            LOG_ERROR("editor.play_mode", "Play Mode failed to start: %s", started.ErrorValue().message.c_str());
        RefreshPlayStateProjection();
        ExtractPlayViewportScene();
    }

    void EditorWorkspaceController::StopPlaySession() {
        m_playSession.Stop();
        m_nativeGameplayReloadPending = false;
        if (m_pendingGameplayRegistry && !m_pendingGameplayRegistry->HasBlockingDiagnostics()) {
            m_gameplayRegistry = std::move(m_pendingGameplayRegistry);
            RefreshAvailableBehaviorProjection();
        } else {
            m_pendingGameplayRegistry.reset();
        }
        m_viewModel.playError.clear();
        m_viewModel.playState = EditorPlayState::Idle;
        m_viewModel.activeDocumentPanelId = m_prePlayDocumentPanelId.empty() ? "horo.viewport" : m_prePlayDocumentPanelId;
        m_activeRuntimeRevision = {};
    }

    void EditorWorkspaceController::RefreshPlayStateProjection() {
        switch (m_playSession.State()) {
            case EditorPlaySessionState::Idle:
                m_viewModel.playState = EditorPlayState::Idle;
                break;
            case EditorPlaySessionState::Starting:
                m_viewModel.playState = EditorPlayState::Starting;
                break;
            case EditorPlaySessionState::Playing:
                m_viewModel.playState = EditorPlayState::Playing;
                break;
            case EditorPlaySessionState::Paused:
                m_viewModel.playState = EditorPlayState::Paused;
                break;
            case EditorPlaySessionState::Reloading:
                m_viewModel.playState = EditorPlayState::Starting;
                break;
            case EditorPlaySessionState::Stopping:
                m_viewModel.playState = EditorPlayState::Stopping;
                break;
            case EditorPlaySessionState::Failed:
                m_viewModel.playState = EditorPlayState::Failed;
                break;
        }
        m_viewModel.playError = m_playSession.LastError().has_value() ? m_playSession.LastError()->message : std::string{};
    }

    void EditorWorkspaceController::UpdatePlayPresentation(const float elapsedSeconds) {
        if (!std::isfinite(elapsedSeconds) || elapsedSeconds < 0.0F)
            return;
        m_playSession.PresentationUpdate(Gameplay::FrameDeltaTime{static_cast<double>(elapsedSeconds)});
        ExtractPlayViewportScene();
    }

    void EditorWorkspaceController::UpdatePlayFixed(const std::span<const Gameplay::GameplayInputAction> input,
                                                    const double fixedDeltaSeconds) {
        if (!std::isfinite(fixedDeltaSeconds) || fixedDeltaSeconds <= 0.0)
            return;
        ApplyNativeGameplayReload();
        ApplyPendingGameplayRegistry();
        if (const Result<void> updated = m_playSession.FixedUpdate(input, Gameplay::FixedDeltaTime{fixedDeltaSeconds}); updated.HasError())
            LOG_ERROR("editor.play_mode", "Play Mode fixed update failed: %s", updated.ErrorValue().message.c_str());
        RefreshPlayStateProjection();
        ExtractPlayViewportScene();
    }

    EditorViewportCamera EditorWorkspaceController::ResolvePlayViewportCamera(const Runtime::RuntimeSceneView &runtimeView) const {
        EditorViewportCamera camera = m_viewport.Current().camera;
        for (std::size_t slot = 0; slot < runtimeView.SlotCount(); ++slot) {
            const std::optional<Runtime::RuntimeEntityView> entity = runtimeView.EntityAt(slot);
            if (!entity || !entity->components->camera.has_value())
                continue;
            const Runtime::CameraComponent &authoredCamera = *entity->components->camera;
            const Math::Transform worldTransform = ResolveRuntimeEntityTransform(runtimeView, *entity);
            return EditorViewportCamera{
                .projection = authoredCamera.projection,
                .position = worldTransform.translation,
                .target = worldTransform.translation + worldTransform.rotation.Rotate({0.0F, 0.0F, -1.0F}),
                .up = worldTransform.rotation.Rotate({0.0F, 1.0F, 0.0F}),
                .verticalFovRadians = authoredCamera.verticalFieldOfViewRadians,
                .orthographicHeight = authoredCamera.orthographicHeight,
                .nearPlane = authoredCamera.nearPlane,
                .farPlane = authoredCamera.farPlane,
            };
        }
        return camera;
    }

    void EditorWorkspaceController::ExtractPlayViewportScene() {
        const Runtime::RuntimeScene *scene = m_playSession.Scene();
        if (scene == nullptr)
            return;
        const Runtime::RuntimeSceneView runtimeView = scene->View();
        const EditorViewportCamera camera = ResolvePlayViewportCamera(runtimeView);
        LoadDocumentAssetMeshes();
        const SceneDocumentSnapshot document = m_document.Snapshot();
        Result<EditorViewportSceneSnapshot> extracted =
            ExtractEditorViewportScene(runtimeView, m_playSession.AuthoringRevision(), camera, m_primitiveMeshCache, &document,
                                       &m_assetMeshCache, false);
        if (extracted.HasError()) {
            LOG_ERROR("editor.play_mode", "Game viewport extraction failed: %s", extracted.ErrorValue().message.c_str());
            return;
        }
        m_viewportScene = std::move(extracted).Value();
        ++m_viewportSceneRevision;
        m_viewModel.viewportCamera = camera;
        RefreshViewportLightProjection();
    }

    void EditorWorkspaceController::RefreshSelectionProjection() {
        const SelectionSnapshot &selection = m_selection.Current();
        m_viewModel.primarySelection = selection.primary;
        m_viewModel.selectedObjects = selection.objects;
        m_viewModel.viewportCamera = m_viewportScene.camera;
        m_viewModel.primarySelectionWorldTransform.reset();
        m_viewModel.primarySelectionPreviewWorldTransform.reset();
        m_viewModel.primarySelectionParentWorldTransform.reset();
        m_viewModel.primarySelectionWorldBounds.reset();
        if (const std::optional<Runtime::RuntimeSceneView> active = m_runtimeScene.ActiveScene();
            selection.primary && active && m_viewportScene.runtimeSceneId == active->RuntimeId()) {
            const Result<SceneObjectWorldTransforms> transforms = ResolveSceneObjectWorldTransforms(*active, *selection.primary);
            if (transforms.HasValue()) {
                m_viewModel.primarySelectionWorldTransform = transforms.Value().localToWorld;
                m_viewModel.primarySelectionParentWorldTransform = transforms.Value().parentToWorld;
            } else {
                LOG_ERROR("editor.viewport", "Selected object transform projection failed: %s", transforms.ErrorValue().message.c_str());
            }
        }
        if (m_viewportScene.instances.size() != m_viewportScene.instanceObjects.size()) {
            return;
        }
        for (std::size_t index = 0; index < m_viewportScene.instances.size(); ++index) {
            m_viewportScene.instances[index].presentation.tint = {0.12F, 0.72F, 1.0F};
            m_viewportScene.instances[index].presentation.tintStrength =
                std::ranges::find(selection.objects, m_viewportScene.instanceObjects[index]) != selection.objects.end() ? 0.65F : 0.0F;
            if (selection.primary == m_viewportScene.instanceObjects[index]) {
                const Result<Math::Aabb> bounds =
                    Math::TransformAabb(m_viewportScene.instances[index].localBounds, m_viewportScene.instances[index].localToWorld);
                if (bounds.HasValue())
                    m_viewModel.primarySelectionWorldBounds = bounds.Value();
                else
                    LOG_ERROR("editor.viewport", "Selected object bounds projection failed: %s", bounds.ErrorValue().message.c_str());
            }
        }
    }

    void EditorWorkspaceController::RefreshViewportLightProjection() {
        m_viewModel.viewportLights.clear();
        if (m_viewportScene.lights.size() != m_viewportScene.lightObjects.size())
            return;
        m_viewModel.viewportLights.reserve(m_viewportScene.lights.size());
        for (std::size_t index = 0; index < m_viewportScene.lights.size(); ++index) {
            m_viewModel.viewportLights.emplace_back(m_viewportScene.lightObjects[index], m_viewportScene.lights[index]);
        }
    }
}  // namespace Horo::Editor
