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
    /** @copydoc ProjectPlaySceneHost::PrepareWorldRenderer */
    Result<std::unique_ptr<Render::RenderOriginSceneAdapter>> ProjectPlaySceneHost::PrepareWorldRenderer(
        const Render::RenderViewId view) const {
        using namespace WorldStreaming;
        const auto participant = OriginRebaseParticipantId::Create(identity_.value);
        if (participant.HasError())
            return Result<std::unique_ptr<Render::RenderOriginSceneAdapter>>::Failure(participant.ErrorValue());
        return Render::RenderOriginSceneAdapter::Create(mountedWorld_->Origin(), view,
                                                        {participant.Value(), OriginRebaseParticipantRevision::Create(1).Value()},
                                                        {1, std::max<std::size_t>(1, worldViewport_->meshes.size()), 1});
    }

    /** @copydoc ProjectPlaySceneHost::QueueMountedXrScene */
    Result<void> ProjectPlaySceneHost::QueueMountedXrScene(const Runtime::RuntimeSceneDefinition &definition,
                                                           const MountedWorldStartup &boundary) {
        const auto view = worldViewport_->issueView();
        if (view.HasError())
            return Result<void>::Failure(view.ErrorValue());
        auto renderer = PrepareWorldRenderer(view.Value());
        if (renderer.HasError())
            return Result<void>::Failure(renderer.ErrorValue());
        worldRenderer_ = std::move(renderer).Value();
        return mountedWorld_->QueueInitialCell(definition, *worldSelection_->initialCell, boundary,
                                               resources_.registry_.Snapshot().Revision());
    }

    /** @copydoc ProjectPlaySceneHost::CaptureMountedXrGeometry */
    Result<std::shared_ptr<const Render::RenderOriginLocalScene>> ProjectPlaySceneHost::CaptureMountedXrGeometry(
        const Runtime::RuntimeSceneView &candidate) {
        using Output = Result<std::shared_ptr<const Render::RenderOriginLocalScene>>;
        if (!mountedWorld_ || !worldRenderer_ || !worldViewport_ || !viewportPrimitives_)
            return Output::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Stale));
        auto captured = mountedWorld_->CaptureInitialGeometry(candidate, worldViewport_->camera, worldViewport_->meshes,
                                                              *viewportPrimitives_, *worldRenderer_);
        if (captured.HasError())
            return Output::Failure(captured.ErrorValue());

        struct RetainedGeometry final {
            MountedWorldRenderPacket packet;
            std::optional<Render::RenderOriginLocalScene> view;
        };

        auto owner = std::make_shared<RetainedGeometry>(RetainedGeometry{std::move(captured).Value(), {}});
        auto view = owner->packet.OriginScene();
        if (view.HasError())
            return Output::Failure(view.ErrorValue());
        owner->view.emplace(std::move(view).Value());
        return Output::Success({owner, &*owner->view});
    }

    /** @copydoc ProjectPlaySceneHost::CompleteWorldStartup */
    Result<void> ProjectPlaySceneHost::CompleteWorldStartup(const Runtime::FrameContext &frame) {
        if (selectedXrHost_ && mountedWorld_) {
            const auto active = mountedWorld_->ReconcileInitialCell();
            return active.HasError() ? Result<void>::Failure(active.ErrorValue()) : Result<void>::Success();
        }
        if (!worldStartup_ || !scenes_->ActiveScene())
            return Result<void>::Success();
        if (frame.cancellation.IsCancellationRequested())
            return Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Cancelled));

        const auto &viewport = *worldViewport_;
        auto view = viewport.issueView();
        if (view.HasError())
            return Result<void>::Failure(view.ErrorValue());
        auto renderer = PrepareWorldRenderer(view.Value());
        if (renderer.HasError())
            return Result<void>::Failure(renderer.ErrorValue());
        if (auto active = mountedWorld_->ActivateInitialCell(*worldSelection_->initialCell, *worldStartup_, &frame); active.HasError())
            return active;
        const auto parent = scenes_->ActiveScene();
        if (!parent)
            return Result<void>::Failure(MakeError(WorldStreaming::WorldArtifactErrors::Stale));
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
