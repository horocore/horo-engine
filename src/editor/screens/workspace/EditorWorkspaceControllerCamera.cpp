#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Runtime/Camera/CameraErrors.h"
#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <limits>

namespace Horo::Editor {
    namespace {
        /** @brief The baseline preview surface admits camera tracks without inventing other owner bindings. */
        bool EligiblePreview(const Cinematic::SequencePlaybackActivation &activation, const bool playActive) noexcept {
            return !playActive && activation.plan.TrackCount() == 0 && activation.plan.EventCount() == 0;
        }

        /** @brief Copies the current editor owner's base camera; never captures a pre-cinematic value. */
        Runtime::CameraProposal AuthoringProposal(const EditorViewportCamera &camera) {
            const auto kind = camera.projection == Runtime::CameraProjection::Perspective ? Render::RenderProjectionKind::Perspective
                                                                                          : Render::RenderProjectionKind::Orthographic;
            return {{},
                    {camera.position,
                     camera.target,
                     camera.up,
                     {kind, camera.verticalFovRadians, camera.orthographicHeight, camera.nearPlane, camera.farPlane}}};
        }

        /** @brief Projects the committed copied runtime values into the existing viewport extraction contract. */
        EditorViewportCamera ViewportCamera(const Render::RenderCameraView &camera) {
            const auto kind = camera.projection.kind == Render::RenderProjectionKind::Perspective ? Runtime::CameraProjection::Perspective
                                                                                                  : Runtime::CameraProjection::Orthographic;
            return {kind,
                    camera.position,
                    camera.target,
                    camera.up,
                    camera.projection.verticalFovRadians,
                    camera.projection.orthographicHeight,
                    camera.projection.nearPlane,
                    camera.projection.farPlane};
        }
    }  // namespace

    EditorWorkspaceController::CameraPreviewState::CameraPreviewState(Cinematic::CinematicRuntimeService &&runtimeValue,
                                                                      Runtime::CameraService &&cameraValue)
        : runtime(std::move(runtimeValue)), camera(std::move(cameraValue)),
          crossings(runtime.ServiceSnapshot().budget.maximumBoundaryOccurrences) {}

    EditorWorkspaceController::~EditorWorkspaceController() = default;

    /** @copydoc EditorWorkspaceController::StartCameraCutPreview */
    Result<void> EditorWorkspaceController::StartCameraCutPreview(Cinematic::SequencePlaybackActivation activation,
                                                                  const Cinematic::CameraCutActivation &cuts,
                                                                  const Runtime::CameraViewContextId view) {
        const auto scene = m_runtimeScene.ActiveScene();
        if (!scene || !EligiblePreview(activation, m_playSession.IsActive()))
            return Result<void>::Failure(MakeError(Runtime::CameraErrors::InvalidContext));
        auto camera = Runtime::CameraService::Create({view, scene->RuntimeId(), Runtime::CameraViewDomain::EditorPreview});
        if (camera.HasError())
            return Result<void>::Failure(camera.ErrorValue());
        auto runtime = Cinematic::CinematicRuntimeService::Create({activation.player.handle.session});
        if (runtime.HasError())
            return Result<void>::Failure(runtime.ErrorValue());
        auto candidate = std::make_unique<CameraPreviewState>(std::move(runtime).Value(), std::move(camera).Value());
        auto playback =
            Cinematic::CinematicCameraPlayback::Activate(candidate->runtime, candidate->camera, std::move(activation), cuts, *scene);
        if (playback.HasError())
            return Result<void>::Failure(playback.ErrorValue());
        candidate->playback.emplace(std::move(playback).Value());
        if (const auto started = candidate->runtime.Play(candidate->playback->Player()); started.HasError())
            return Result<void>::Failure(started.ErrorValue());
        if (const auto published = candidate->playback->Publish(*scene); published.HasError())
            return published;
        m_cameraPreview = std::move(candidate);
        m_activeRuntimeRevision = {};
        SynchronizeRuntimeScenePreview();
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::AdvanceCameraCutPreview */
    Result<void> EditorWorkspaceController::AdvanceCameraCutPreview(const Cinematic::SequenceTime delta) {
        const auto scene = m_runtimeScene.ActiveScene();
        if (!m_cameraPreview || !scene)
            return Result<void>::Failure(MakeError(Runtime::CameraErrors::Closed));
        const Cinematic::SequenceFrameScratch scratch{{},
                                                      {},
                                                      m_cameraPreview->crossings,
                                                      m_cameraPreview->runtime.ServiceSnapshot().budget.maximumBoundaryOccurrences};
        if (const auto evaluated = m_cameraPreview->playback->Evaluate(delta, *scene, scratch, {}); evaluated.HasError()) {
            StopCameraCutPreview();
            return Result<void>::Failure(evaluated.ErrorValue());
        }
        m_activeRuntimeRevision = {};
        SynchronizeRuntimeScenePreview();
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::SeekCameraCutPreview */
    Result<void> EditorWorkspaceController::SeekCameraCutPreview(const Cinematic::SequenceTime position) {
        if (!m_cameraPreview)
            return Result<void>::Failure(MakeError(Runtime::CameraErrors::Closed));
        if (const auto seeked = m_cameraPreview->runtime.Seek(m_cameraPreview->playback->Player(), position); seeked.HasError())
            return Result<void>::Failure(seeked.ErrorValue());
        const auto scene = m_runtimeScene.ActiveScene();
        if (!scene) {
            StopCameraCutPreview();
            return Result<void>::Failure(MakeError(Runtime::CameraErrors::InvalidContext));
        }
        if (const auto published = m_cameraPreview->playback->Publish(*scene); published.HasError()) {
            StopCameraCutPreview();
            return published;
        }
        m_activeRuntimeRevision = {};
        SynchronizeRuntimeScenePreview();
        return Result<void>::Success();
    }

    /** @copydoc EditorWorkspaceController::StopCameraCutPreview */
    void EditorWorkspaceController::StopCameraCutPreview() {
        m_cameraPreview.reset();
        m_activeRuntimeRevision = {};
        SynchronizeRuntimeScenePreview();
    }

    Result<EditorViewportCamera> EditorWorkspaceController::ResolveCameraCutPreview(const Runtime::RuntimeSceneView scene) {
        const auto &base = m_viewport.Current().camera;
        if (!m_cameraPreview)
            return Result<EditorViewportCamera>::Success(base);
        if (scene.RuntimeId() != m_cameraPreview->camera.Context().scene) {
            m_cameraPreview.reset();
            return Result<EditorViewportCamera>::Success(base);
        }
        if (const auto published = m_cameraPreview->playback->Publish(scene); published.HasError()) {
            LOG_ERROR("editor.viewport", "Camera preview released after binding loss: %s", published.ErrorValue().message.c_str());
            m_cameraPreview.reset();
            return Result<EditorViewportCamera>::Success(base);
        }
        if (m_cameraPreview->frame == std::numeric_limits<std::uint64_t>::max())
            return Result<EditorViewportCamera>::Failure(MakeError(Runtime::CameraErrors::InvalidFrame));
        const auto selection = m_cameraPreview->camera.Commit(++m_cameraPreview->frame, AuthoringProposal(base));
        if (selection.HasError())
            return Result<EditorViewportCamera>::Failure(selection.ErrorValue());
        return Result<EditorViewportCamera>::Success(ViewportCamera(selection.Value().selected.values));
    }
}  // namespace Horo::Editor
