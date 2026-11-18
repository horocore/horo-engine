#include "EditorMountedWorldViewport.h"

#include "EditorProjectSceneHostOwner.h"
#include "Horo/WorldStreaming/WorldArtifactErrors.h"

#include <algorithm>
#include <utility>

namespace Horo::Application::Internal {
    /** @copydoc EditorMountedWorldViewport::Publish */
    Result<void> EditorMountedWorldViewport::Publish(EditorMountedWorldViewportView value) {
        if (current_.owner.IsValid() || !value.owner.IsValid() || !value.view.IsValid() || !value.packet)
            return Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Invalid));
        const auto scene = value.packet->Scene();
        if (scene.HasError())
            return Result<void>::Failure(scene.ErrorValue());
        if (scene.Value().lightSceneDomain != value.owner.value)
            return Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Stale));
        current_ = std::move(value);
        return Result<void>::Success();
    }

    /** @copydoc EditorMountedWorldViewport::Withdraw */
    void EditorMountedWorldViewport::Withdraw(const Runtime::SceneRuntimeId owner) noexcept {
        if (current_.owner == owner)
            current_ = {};
    }
}  // namespace Horo::Application::Internal

namespace Horo::Application::Internal::EditorProjectSceneDetail {
    /** @copydoc ProjectPlaySceneHost::CompleteWorldStartup */
    Result<void> ProjectPlaySceneHost::CompleteWorldStartup(const Runtime::FrameContext &frame) {
        if (!worldStartup_ || !scenes_->ActiveScene())
            return Result<void>::Success();
        if (frame.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Cancelled));

        const auto &viewport = *worldViewport_;
        auto view = viewport.issueView();
        if (view.HasError())
            return Result<void>::Failure(view.ErrorValue());
        using namespace WorldStreaming;
        auto requirement = OriginRebaseParticipantId::Create(identity_.value);
        if (requirement.HasError())
            return Result<void>::Failure(requirement.ErrorValue());
        auto renderer = Render::RenderOriginSceneAdapter::Create(mountedWorld_->Origin(), view.Value(),
                                                                 {requirement.Value(), OriginRebaseParticipantRevision::Create(1).Value()},
                                                                 {1, std::max<std::size_t>(1, viewport.meshes.size()), 1});
        if (renderer.HasError())
            return Result<void>::Failure(renderer.ErrorValue());
        if (auto active = mountedWorld_->ActivateInitialCell(*worldSelection_->initialCell, *worldStartup_, &frame); active.HasError())
            return active;
        const auto parent = scenes_->ActiveScene();
        if (!parent)
            return Result<void>::Failure(MakeError(WorldArtifactErrors::Stale));
        auto packet = mountedWorld_->CaptureGeometry(*parent, viewport.camera, viewport.meshes, *viewportPrimitives_, *renderer.Value());
        if (packet.HasError())
            return Result<void>::Failure(packet.ErrorValue());
        auto owned = std::make_shared<const MountedWorldRenderPacket>(std::move(packet).Value());
        if (auto published = viewport.publication->Publish({identity_, view.Value(), std::move(owned)}); published.HasError())
            return published;
        worldRenderer_ = std::move(renderer).Value();
        worldStartup_.reset();
        return Result<void>::Success();
    }
}  // namespace Horo::Application::Internal::EditorProjectSceneDetail
