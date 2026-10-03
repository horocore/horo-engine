#include "RenderFrontendResourceAccess.h"

#include "Horo/Runtime/Render/RenderFrontend.h"
#include "RenderResourceOperations.h"
#include "RenderResourceRegistry.h"
#include "RenderResourceUploadQueue.h"

namespace Horo::Render {
    /** @copydoc RenderFrontend::UploadSnapshot */
    RenderResourceUploadSnapshot RenderFrontend::UploadSnapshot() const noexcept {
        return resourceUploadQueue_->Snapshot();
    }

    /** @copydoc RenderFrontend::ResourceState(RenderBufferHandle) */
    Result<RenderResourceState> RenderFrontend::ResourceState(const RenderBufferHandle buffer) const {
        return resourceRegistry_->State(Detail::RenderResourceClass::Buffer, Identity(buffer));
    }

    /** @copydoc RenderFrontend::ResourceState(RenderMeshHandle) */
    Result<RenderResourceState> RenderFrontend::ResourceState(const RenderMeshHandle mesh) const {
        return resourceRegistry_->State(Detail::RenderResourceClass::Mesh, Identity(mesh));
    }

    /** @copydoc RenderFrontend::ResourceState(RenderTextureHandle) */
    Result<RenderResourceState> RenderFrontend::ResourceState(const RenderTextureHandle texture) const {
        return resourceRegistry_->State(Detail::RenderResourceClass::Texture, Identity(texture));
    }

    /** @copydoc RenderFrontend::ResourceState(RenderTextureViewHandle) */
    Result<RenderResourceState> RenderFrontend::ResourceState(const RenderTextureViewHandle view) const {
        return resourceRegistry_->State(Detail::RenderResourceClass::TextureView, Identity(view));
    }

    /** @copydoc RenderFrontend::ResourceState(RenderTargetHandle) */
    Result<RenderResourceState> RenderFrontend::ResourceState(const RenderTargetHandle target) const {
        return resourceRegistry_->State(Detail::RenderResourceClass::RenderTarget, Identity(target));
    }

    /** @copydoc RenderFrontend::ResourceOperationResult */
    Result<void> RenderFrontend::ResourceOperationResult(const ResourceOperationId operation) const {
        return resourceRegistry_->OperationResult(operation);
    }

    Result<std::uint64_t> RenderFrontend::BackendInstance(const RenderBufferHandle buffer) const {
        return resourceRegistry_->BackendInstance(Detail::RenderResourceClass::Buffer, Identity(buffer));
    }

    Result<std::uint64_t> RenderFrontend::BackendInstance(const RenderMeshHandle mesh) const {
        return resourceRegistry_->BackendInstance(Detail::RenderResourceClass::Mesh, Identity(mesh));
    }

    Result<std::uint64_t> RenderFrontend::BackendInstance(const RenderTextureViewHandle view) const {
        return resourceRegistry_->BackendInstance(Detail::RenderResourceClass::TextureView, Identity(view));
    }

    Result<std::uint64_t> RenderFrontend::BackendInstance(const RenderTargetHandle target) const {
        return resourceRegistry_->BackendInstance(Detail::RenderResourceClass::RenderTarget, Identity(target));
    }

    Result<std::uint64_t> Detail::RenderFrontendResourceAccess::BackendInstance(const RenderFrontend &frontend,
                                                                                const RenderBufferHandle buffer) {
        return frontend.BackendInstance(buffer);
    }

    Result<std::uint64_t> Detail::RenderFrontendResourceAccess::BackendInstance(const RenderFrontend &frontend,
                                                                                const RenderMeshHandle mesh) {
        return frontend.BackendInstance(mesh);
    }

    Result<std::uint64_t> Detail::RenderFrontendResourceAccess::BackendInstance(const RenderFrontend &frontend,
                                                                                const RenderTextureViewHandle view) {
        return frontend.BackendInstance(view);
    }

    Result<std::uint64_t> Detail::RenderFrontendResourceAccess::BackendInstance(const RenderFrontend &frontend,
                                                                                const RenderTargetHandle target) {
        return frontend.BackendInstance(target);
    }

    Result<void> Detail::RenderFrontendResourceAccess::TrackSubmission(RenderFrontend &frontend, const RenderBufferHandle buffer,
                                                                       const RenderTimelinePoint completion) {
        return frontend.resourceRegistry_->TrackSubmission(RenderResourceClass::Buffer, Identity(buffer), completion);
    }

    Result<void> Detail::RenderFrontendResourceAccess::TrackSubmission(RenderFrontend &frontend, const RenderMeshHandle mesh,
                                                                       const RenderTimelinePoint completion) {
        return frontend.resourceRegistry_->TrackSubmission(RenderResourceClass::Mesh, Identity(mesh), completion);
    }

    Result<void> Detail::RenderFrontendResourceAccess::TrackSubmission(RenderFrontend &frontend, const RenderTextureHandle texture,
                                                                       const RenderTimelinePoint completion) {
        return frontend.resourceRegistry_->TrackSubmission(RenderResourceClass::Texture, Identity(texture), completion);
    }

    Result<void> Detail::RenderFrontendResourceAccess::TrackSubmission(RenderFrontend &frontend, const RenderTextureViewHandle view,
                                                                       const RenderTimelinePoint completion) {
        return frontend.resourceRegistry_->TrackSubmission(RenderResourceClass::TextureView, Identity(view), completion);
    }

    Result<void> Detail::RenderFrontendResourceAccess::TrackSubmission(RenderFrontend &frontend, const RenderTargetHandle target,
                                                                       const RenderTimelinePoint completion) {
        return frontend.resourceRegistry_->TrackSubmission(RenderResourceClass::RenderTarget, Identity(target), completion);
    }

    Result<std::size_t> Detail::RenderFrontendResourceAccess::AcknowledgeCompletion(RenderFrontend &frontend,
                                                                                    const RenderTimelinePoint completion) {
        auto acknowledged = frontend.resourceRegistry_->AcknowledgeCompletion(completion);
        if (acknowledged.HasError())
            return acknowledged;
        static_cast<void>(frontend.resourceRegistry_->DrainRetirements());
        static_cast<void>(frontend.memoryBudget_->ReclaimEmptyBlocks(frontend.memoryConfig_.maximumEmptyBlocksReclaimedPerDrain));
        return acknowledged;
    }
}  // namespace Horo::Render
