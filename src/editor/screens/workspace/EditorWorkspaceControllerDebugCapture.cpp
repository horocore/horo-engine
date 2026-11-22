#include "editor/screens/workspace/EditorWorkspaceController.h"

#include <algorithm>

namespace Horo::Editor {
    namespace {
        const ErrorCodeDescriptor CaptureUnavailable{.domain = ErrorDomainId{"editor.debug_capture"},
                                                     .code = ErrorCode{"capture_unavailable"},
                                                     .defaultSeverity = ErrorSeverity::Warning,
                                                     .summary = "The exact visible Play target is unavailable."};
        const ErrorCodeDescriptor CaptureBusy{.domain = ErrorDomainId{"editor.debug_capture"},
                                              .code = ErrorCode{"capture_busy"},
                                              .defaultSeverity = ErrorSeverity::Warning,
                                              .summary = "A capture is already running or its start rate is limited."};
    }  // namespace

    /** @copydoc EditorWorkspaceController::BindDebugCapture */
    std::weak_ptr<const EditorDebugCaptureEndpoint> EditorWorkspaceController::BindDebugCapture(std::string project,
                                                                                                std::uint64_t authorizationRevision) {
        m_debugCapture.Clear();
        m_debugCaptureEntity.reset();
        m_debugCaptureEndpoint = std::make_shared<const EditorDebugCaptureEndpoint>(
            EditorDebugCaptureEndpoint{.project = std::move(project),
                                       .authorizationRevision = authorizationRevision,
                                       .owner = std::this_thread::get_id(),
                                       .start =
                                           [this](const EditorDebugCaptureRequest &request) {
            return StartDebugCapture(request);
        },
                                       .read =
                                           [this](std::uint64_t id) {
            return m_debugCapture.Read(id, EditorDebugCaptureStorage::Clock::now());
        },
                                       .cancel = [this](std::uint64_t id) {
            return m_debugCapture.Cancel(id, EditorDebugCaptureStorage::Clock::now());
        }});
        return m_debugCaptureEndpoint;
    }

    /** @copydoc EditorWorkspaceController::StartDebugCapture */
    Result<std::uint64_t> EditorWorkspaceController::StartDebugCapture(const EditorDebugCaptureRequest &request) {
        using Output = Result<std::uint64_t>;
        const auto scene = m_playSession.SceneView();
        if (m_playSession.State() != EditorPlaySessionState::Playing || !scene || !scene->IsCurrent())
            return Output::Failure(MakeError(CaptureUnavailable));
        const SceneObjectId object{request.target};
        const auto authored = std::ranges::find(m_viewModel.objects, object, &SceneObject::id);
        if (authored == m_viewModel.objects.end() || !authored->effectivelyVisible)
            return Output::Failure(MakeError(CaptureUnavailable));
        const auto entity = scene->Find({request.target});
        if (!entity)
            return Output::Failure(MakeError(CaptureUnavailable));
        const auto id = m_debugCapture.Start(request, EditorDebugCaptureStorage::Clock::now());
        if (!id)
            return Output::Failure(MakeError(CaptureBusy));
        m_debugCaptureScene = scene->RuntimeId();
        m_debugCaptureEntity = entity;
        m_debugCaptureObject = object;
        return Output::Success(*id);
    }

    /** @copydoc EditorWorkspaceController::UpdateDebugCapture */
    void EditorWorkspaceController::UpdateDebugCapture() {
        if (!m_debugCaptureEntity)
            return;
        const auto scene = m_playSession.SceneView();
        if (m_playSession.State() != EditorPlaySessionState::Playing || !scene || !scene->IsCurrent() ||
            scene->RuntimeId() != m_debugCaptureScene || scene->Find({m_debugCaptureObject.value}) != m_debugCaptureEntity) {
            m_debugCapture.RetireScene();
            m_debugCaptureEntity.reset();
            return;
        }
        const auto now = EditorDebugCaptureStorage::Clock::now();
        if (!m_debugCapture.Due(now))
            return;
        const auto pose = ResolveSceneObjectWorldTransforms(*scene, m_debugCaptureObject);
        if (pose.HasError()) {
            m_debugCapture.RetireScene();
            return;
        }
        const auto point = Math::TransformPoint(pose.Value().localToWorld, {});
        if (!Math::IsFinite(point)) {
            m_debugCapture.RetireScene();
            return;
        }
        m_debugCapture.Sample({point.x, point.y, point.z}, now);
    }
}  // namespace Horo::Editor
