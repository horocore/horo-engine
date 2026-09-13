#include "EditorViewportResources.h"

#include "Horo/Runtime/Render/RenderFrontend.h"
#include "editor/renderer/EditorRendererErrors.h"
#include "runtime/renderer/frontend/RenderFrontendErrors.h"

#include <limits>
#include <ranges>
#include <string>
#include <utility>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] Error MakeViewportError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }

        template <typename Handle>
        [[nodiscard]] Result<bool> ResourceReady(const Render::RenderFrontend &frontend, const Handle handle,
                                                 const Render::ResourceOperationId operation) {
            if (!handle.IsValid())
                return Result<bool>::Success(false);
            const auto state = frontend.ResourceState(handle);
            if (state.HasValue())
                return Result<bool>::Success(state.Value() == Render::RenderResourceState::Ready);
            if (operation.IsValid()) {
                if (const Result<void> completion = frontend.ResourceOperationResult(operation);
                    completion.HasError() &&
                    completion.ErrorValue().code.Value() != Render::FrontendErrors::ResourceOperationPending.code.Value())
                    return Result<bool>::Failure(completion.ErrorValue());
            }
            return Result<bool>::Failure(state.ErrorValue());
        }

        [[nodiscard]] Result<bool> AdvanceTexture(Render::RenderFrontend &frontend, const Render::RenderMemoryScopeId memoryScope,
                                                  const Render::RenderTextureDescriptor &descriptor, Render::RenderTextureHandle &handle,
                                                  Render::ResourceOperationId &operation) {
            if (!handle.IsValid()) {
                auto created = frontend.CreateTexture(memoryScope, descriptor);
                if (created.HasError())
                    return Result<bool>::Failure(created.ErrorValue());
                handle = created.Value().handle;
                operation = created.Value().operation;
            }
            return ResourceReady(frontend, handle, operation);
        }

        [[nodiscard]] Result<bool> AdvanceTextureView(Render::RenderFrontend &frontend,
                                                      const Render::RenderTextureViewDescriptor &descriptor,
                                                      Render::RenderTextureViewHandle &handle, Render::ResourceOperationId &operation) {
            if (!handle.IsValid()) {
                auto created = frontend.CreateTextureView(descriptor);
                if (created.HasError())
                    return Result<bool>::Failure(created.ErrorValue());
                handle = created.Value().handle;
                operation = created.Value().operation;
            }
            return ResourceReady(frontend, handle, operation);
        }

        [[nodiscard]] Result<bool> AdvanceRenderTarget(Render::RenderFrontend &frontend, const Render::RenderTargetDescriptor &descriptor,
                                                       Render::RenderTargetHandle &handle, Render::ResourceOperationId &operation) {
            if (!handle.IsValid()) {
                auto created = frontend.CreateRenderTarget(descriptor);
                if (created.HasError())
                    return Result<bool>::Failure(created.ErrorValue());
                handle = created.Value().handle;
                operation = created.Value().operation;
            }
            return ResourceReady(frontend, handle, operation);
        }
    }  // namespace

    EditorViewportResources::EditorViewportResources(Render::RenderFrontend &frontend, const EditorViewportResourceConfig &config) noexcept
        : frontend_(&frontend), config_(config) {}

    Result<std::optional<Render::RenderTargetHandle>> EditorViewportResources::Prepare(const Render::RenderSceneView &scene,
                                                                                       const EditorViewportExtent requestedExtent) {
        if (const Result<void> meshes = SynchronizeMeshes(scene.meshResources); meshes.HasError())
            return Result<std::optional<Render::RenderTargetHandle>>::Failure(meshes.ErrorValue());
        if (const Result<void> shadow = SynchronizeShadowResources(); shadow.HasError())
            return Result<std::optional<Render::RenderTargetHandle>>::Failure(shadow.ErrorValue());
        if (const Result<void> target = requestedExtent.IsValid() ? SynchronizeTarget(requestedExtent) : Result<void>::Success();
            target.HasError())
            return Result<std::optional<Render::RenderTargetHandle>>::Failure(target.ErrorValue());
        if (viewportTarget_.target.IsValid()) {
            if (const auto ready = frontend_->ResourceState(viewportTarget_.target);
                ready.HasValue() && ready.Value() == Render::RenderResourceState::Ready)
                return Result<std::optional<Render::RenderTargetHandle>>::Success(viewportTarget_.target);
        }
        return Result<std::optional<Render::RenderTargetHandle>>::Success(std::nullopt);
    }

    void EditorViewportResources::Shutdown() noexcept {
        ReleaseShadowResources();
        ReleaseViewportTarget(pendingViewportTarget_);
        ReleaseViewportTarget(viewportTarget_);
        for (auto &mesh : meshes_ | std::views::values)
            ReleaseMesh(mesh);
        for (auto &mesh : pendingMeshes_ | std::views::values)
            ReleaseMesh(mesh);
        meshes_.clear();
        pendingMeshes_.clear();
        allocatedExtent_ = {};
        meshesReady_ = false;
    }

    std::optional<EditorViewportResources::MeshBinding> EditorViewportResources::FindMesh(const std::uint64_t sourceId) const noexcept {
        if (const auto mesh = meshes_.find(sourceId); mesh != meshes_.end())
            return MeshBinding{mesh->second.mesh, mesh->second.indexCount};
        return std::nullopt;
    }

    Render::RenderTargetHandle EditorViewportResources::Target() const noexcept {
        return viewportTarget_.target;
    }

    Render::RenderTargetHandle EditorViewportResources::ShadowTarget() const noexcept {
        return shadowTarget_;
    }

    Render::RenderTextureViewHandle EditorViewportResources::ShadowTextureView() const noexcept {
        return shadowDepthTextureView_;
    }

    EditorViewportExtent EditorViewportResources::AllocatedExtent() const noexcept {
        return allocatedExtent_;
    }

    std::uintptr_t EditorViewportResources::ImageIdentity() const noexcept {
        return viewportTarget_.imageIdentity;
    }

    bool EditorViewportResources::IsReady() const noexcept {
        return meshesReady_ && viewportTarget_.imageIdentity != 0 && viewportTarget_.target.IsValid() && allocatedExtent_.IsValid();
    }

    Result<void> EditorViewportResources::SynchronizeMeshes(const std::span<const EditorViewportMeshResourceView> resources) {
        meshesReady_ = true;
        for (const EditorViewportMeshResourceView &resource : resources) {
            const auto ready = SynchronizeMesh(resource);
            if (ready.HasError())
                return Result<void>::Failure(ready.ErrorValue());
            meshesReady_ = meshesReady_ && ready.Value();
        }
        RetireMissingMeshes(resources);
        return Result<void>::Success();
    }

    Result<bool> EditorViewportResources::SynchronizeMesh(const EditorViewportMeshResourceView &resource) {
        const std::uint64_t id = resource.handle.id.value;
        auto [activePosition, inserted] = meshes_.try_emplace(id);
        ResidentMesh &active = activePosition->second;
        if (inserted)
            return AdvanceResidentMesh(resource, active);
        if (active.sourceGeneration == resource.handle.generation) {
            if (auto pending = pendingMeshes_.find(id); pending != pendingMeshes_.end()) {
                ReleaseMesh(pending->second);
                pendingMeshes_.erase(pending);
            }
            return AdvanceResidentMesh(resource, active);
        }
        if (!IsResidentMeshReady(active)) {
            ReleaseMesh(active);
            return AdvanceResidentMesh(resource, active);
        }
        ResidentMesh &pending = pendingMeshes_[id];
        if (pending.sourceGeneration != 0 && pending.sourceGeneration != resource.handle.generation)
            ReleaseMesh(pending);
        const auto ready = AdvanceResidentMesh(resource, pending);
        if (ready.HasError())
            return Result<bool>::Failure(ready.ErrorValue());
        if (ready.Value()) {
            ReleaseMesh(active);
            active = std::exchange(pending, {});
            pendingMeshes_.erase(id);
        }
        return Result<bool>::Success(true);
    }

    Result<bool> EditorViewportResources::AdvanceResidentMesh(const EditorViewportMeshResourceView &resource, ResidentMesh &resident) {
        if (resident.sourceGeneration == 0) {
            if (const Result<void> created = CreateResidentMeshBuffers(resource, resident); created.HasError())
                return Result<bool>::Failure(created.ErrorValue());
        }
        const auto vertexReady = ResourceReady(*frontend_, resident.vertexBuffer, resident.vertexOperation);
        if (vertexReady.HasError())
            return Result<bool>::Failure(vertexReady.ErrorValue());
        const auto indexReady = ResourceReady(*frontend_, resident.indexBuffer, resident.indexOperation);
        if (indexReady.HasError())
            return Result<bool>::Failure(indexReady.ErrorValue());
        if (!resident.mesh.IsValid() && vertexReady.Value() && indexReady.Value()) {
            auto mesh = frontend_->CreateMesh({.vertexBuffer = resident.vertexBuffer,
                                               .indexBuffer = resident.indexBuffer,
                                               .vertexStride = sizeof(Render::MeshVertex),
                                               .vertexCount = static_cast<std::uint32_t>(resource.vertices.size()),
                                               .indexFormat = Render::RenderIndexFormat::UInt32,
                                               .indexCount = resident.indexCount,
                                               .topology = Render::RenderPrimitiveTopology::Triangles,
                                               .localBounds = resource.localBounds});
            if (mesh.HasError())
                return Result<bool>::Failure(mesh.ErrorValue());
            resident.mesh = mesh.Value().handle;
            resident.meshOperation = mesh.Value().operation;
        }
        return ResourceReady(*frontend_, resident.mesh, resident.meshOperation);
    }

    Result<void> EditorViewportResources::CreateResidentMeshBuffers(const EditorViewportMeshResourceView &resource,
                                                                    ResidentMesh &resident) {
        if (resource.vertices.size() > std::numeric_limits<std::uint32_t>::max() ||
            resource.indices.size() > std::numeric_limits<std::uint32_t>::max()) {
            return Result<void>::Failure(
                MakeViewportError(RendererErrors::ViewportGeometryCreationFailed, "Viewport mesh exceeds generic resource count limits."));
        }
        auto vertex = frontend_->CreateBuffer(config_.memoryScope,
                                              {.byteSize = resource.vertices.size_bytes(),
                                               .usage = Render::RenderBufferUsage::Vertex,
                                               .access = Render::RenderBufferAccess::DeviceLocal},
                                              std::as_bytes(resource.vertices));
        if (vertex.HasError())
            return Result<void>::Failure(vertex.ErrorValue());
        resident.vertexBuffer = vertex.Value().handle;
        resident.vertexOperation = vertex.Value().operation;
        auto index = frontend_->CreateBuffer(config_.memoryScope,
                                             {.byteSize = resource.indices.size_bytes(),
                                              .usage = Render::RenderBufferUsage::Index,
                                              .access = Render::RenderBufferAccess::DeviceLocal},
                                             std::as_bytes(resource.indices));
        if (index.HasError()) {
            ReleaseMesh(resident);
            return Result<void>::Failure(index.ErrorValue());
        }
        resident.indexBuffer = index.Value().handle;
        resident.indexOperation = index.Value().operation;
        resident.indexCount = static_cast<std::uint32_t>(resource.indices.size());
        resident.sourceGeneration = resource.handle.generation;
        return Result<void>::Success();
    }

    bool EditorViewportResources::IsResidentMeshReady(const ResidentMesh &resident) const {
        if (!resident.mesh.IsValid())
            return false;
        if (const auto state = frontend_->ResourceState(resident.mesh); state.HasValue())
            return state.Value() == Render::RenderResourceState::Ready;
        return false;
    }

    void EditorViewportResources::RetireMissingMeshes(const std::span<const EditorViewportMeshResourceView> resources) noexcept {
        for (auto mesh = meshes_.begin(); mesh != meshes_.end();) {
            const bool present = std::ranges::any_of(resources, [&](const EditorViewportMeshResourceView &resource) {
                return resource.handle.id.value == mesh->first;
            });
            if (!present) {
                ReleaseMesh(mesh->second);
                if (auto pending = pendingMeshes_.find(mesh->first); pending != pendingMeshes_.end()) {
                    ReleaseMesh(pending->second);
                    pendingMeshes_.erase(pending);
                }
                mesh = meshes_.erase(mesh);
            } else {
                ++mesh;
            }
        }
    }

    Result<void> EditorViewportResources::SynchronizeTarget(const EditorViewportExtent extent) {
        if (allocatedExtent_ == extent) {
            if (pendingViewportTarget_.extent.IsValid())
                ReleaseViewportTarget(pendingViewportTarget_);
            return Result<void>::Success();
        }
        if (pendingViewportTarget_.extent.IsValid() && pendingViewportTarget_.extent != extent)
            ReleaseViewportTarget(pendingViewportTarget_);
        pendingViewportTarget_.extent = extent;
        const auto targetReady = AdvanceViewportTarget(pendingViewportTarget_);
        if (targetReady.HasError())
            return Result<void>::Failure(targetReady.ErrorValue());
        if (!targetReady.Value())
            return Result<void>::Success();
        if (config_.resolveImage == nullptr) {
            return Result<void>::Failure(
                MakeViewportError(RendererErrors::ViewportGeometryCreationFailed, "Viewport image resolver is unavailable."));
        }
        auto image = config_.resolveImage(*frontend_, pendingViewportTarget_.colorView);
        if (image.HasError())
            return Result<void>::Failure(image.ErrorValue());
        pendingViewportTarget_.imageIdentity = image.Value();
        ReleaseViewportTarget(viewportTarget_);
        viewportTarget_ = std::exchange(pendingViewportTarget_, {});
        allocatedExtent_ = extent;
        return Result<void>::Success();
    }

    Result<bool> EditorViewportResources::AdvanceViewportTarget(ViewportTargetResources &resources) {
        if (const auto texturesReady = AdvanceViewportTextures(resources); texturesReady.HasError() || !texturesReady.Value())
            return texturesReady;
        if (const auto viewsReady = AdvanceViewportViews(resources); viewsReady.HasError() || !viewsReady.Value())
            return viewsReady;
        const Render::FramebufferExtent extent{resources.extent.width, resources.extent.height};
        return AdvanceRenderTarget(*frontend_,
                                   {.colorAttachment = resources.colorView, .depthAttachment = resources.depthView, .extent = extent},
                                   resources.target, resources.targetOperation);
    }

    Result<bool> EditorViewportResources::AdvanceViewportTextures(ViewportTargetResources &resources) {
        using enum Render::RenderTextureUsage;
        const Render::FramebufferExtent extent{resources.extent.width, resources.extent.height};
        const auto color =
            AdvanceTexture(*frontend_, config_.memoryScope,
                           {.extent = extent, .format = Render::RenderTextureFormat::Rgba8Unorm, .usage = Sampled | RenderAttachment},
                           resources.colorTexture, resources.colorTextureOperation);
        if (color.HasError())
            return Result<bool>::Failure(color.ErrorValue());
        const auto depth =
            AdvanceTexture(*frontend_, config_.memoryScope, {.extent = extent, .format = config_.depthFormat, .usage = RenderAttachment},
                           resources.depthTexture, resources.depthTextureOperation);
        if (depth.HasError())
            return Result<bool>::Failure(depth.ErrorValue());
        return Result<bool>::Success(color.Value() && depth.Value());
    }

    Result<bool> EditorViewportResources::AdvanceViewportViews(ViewportTargetResources &resources) {
        const auto color = AdvanceTextureView(*frontend_,
                                              {.texture = resources.colorTexture,
                                               .format = Render::RenderTextureFormat::Rgba8Unorm,
                                               .aspect = Render::RenderTextureAspect::Color},
                                              resources.colorView, resources.colorViewOperation);
        if (color.HasError())
            return Result<bool>::Failure(color.ErrorValue());
        const auto depth =
            AdvanceTextureView(*frontend_,
                               {.texture = resources.depthTexture, .format = config_.depthFormat, .aspect = config_.depthAspect},
                               resources.depthView, resources.depthViewOperation);
        if (depth.HasError())
            return Result<bool>::Failure(depth.ErrorValue());
        return Result<bool>::Success(color.Value() && depth.Value());
    }

    Result<void> EditorViewportResources::SynchronizeShadowResources() {
        const auto targetReady = AdvanceShadowTarget();
        if (targetReady.HasError())
            return Result<void>::Failure(targetReady.ErrorValue());
        meshesReady_ = meshesReady_ && targetReady.Value();
        return Result<void>::Success();
    }

    Result<bool> EditorViewportResources::AdvanceShadowTarget() {
        using enum Render::RenderTextureUsage;
        constexpr Render::FramebufferExtent shadowExtent{EditorViewportDirectionalShadowMapResolution,
                                                         EditorViewportDirectionalShadowMapResolution};
        if (const auto textureReady = AdvanceTexture(*frontend_, config_.memoryScope,
                                                     {.extent = shadowExtent,
                                                      .format = Render::RenderTextureFormat::Depth32Float,
                                                      .usage = Sampled | RenderAttachment},
                                                     shadowDepthTexture_, shadowDepthTextureOperation_);
            textureReady.HasError() || !textureReady.Value())
            return textureReady;
        if (const auto viewReady = AdvanceTextureView(*frontend_,
                                                      {.texture = shadowDepthTexture_,
                                                       .format = Render::RenderTextureFormat::Depth32Float,
                                                       .aspect = Render::RenderTextureAspect::Depth},
                                                      shadowDepthTextureView_, shadowDepthTextureViewOperation_);
            viewReady.HasError() || !viewReady.Value())
            return viewReady;
        return AdvanceRenderTarget(*frontend_, {.depthAttachment = shadowDepthTextureView_, .extent = shadowExtent}, shadowTarget_,
                                   shadowTargetOperation_);
    }

    void EditorViewportResources::ReleaseMesh(ResidentMesh &mesh) noexcept {
        if (mesh.mesh.IsValid())
            static_cast<void>(frontend_->ReleaseMesh(mesh.mesh));
        if (mesh.indexBuffer.IsValid())
            static_cast<void>(frontend_->ReleaseBuffer(mesh.indexBuffer));
        if (mesh.vertexBuffer.IsValid())
            static_cast<void>(frontend_->ReleaseBuffer(mesh.vertexBuffer));
        mesh = {};
    }

    void EditorViewportResources::ReleaseShadowResources() noexcept {
        if (shadowTarget_.IsValid())
            static_cast<void>(frontend_->ReleaseRenderTarget(shadowTarget_));
        if (shadowDepthTextureView_.IsValid())
            static_cast<void>(frontend_->ReleaseTextureView(shadowDepthTextureView_));
        if (shadowDepthTexture_.IsValid())
            static_cast<void>(frontend_->ReleaseTexture(shadowDepthTexture_));
        shadowDepthTexture_ = {};
        shadowDepthTextureView_ = {};
        shadowTarget_ = {};
        shadowDepthTextureOperation_ = {};
        shadowDepthTextureViewOperation_ = {};
        shadowTargetOperation_ = {};
    }

    void EditorViewportResources::ReleaseViewportTarget(ViewportTargetResources &resources) noexcept {
        if (resources.target.IsValid())
            static_cast<void>(frontend_->ReleaseRenderTarget(resources.target));
        if (resources.depthView.IsValid())
            static_cast<void>(frontend_->ReleaseTextureView(resources.depthView));
        if (resources.colorView.IsValid())
            static_cast<void>(frontend_->ReleaseTextureView(resources.colorView));
        if (resources.depthTexture.IsValid())
            static_cast<void>(frontend_->ReleaseTexture(resources.depthTexture));
        if (resources.colorTexture.IsValid())
            static_cast<void>(frontend_->ReleaseTexture(resources.colorTexture));
        resources = {};
    }
}  // namespace Horo::Editor
