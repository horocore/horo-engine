#include "RenderFrontendResourceAccess.h"

#include "Horo/Runtime/Render/RenderFrontend.h"
#include "RenderResourceRegistry.h"

namespace Horo::Render {
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
