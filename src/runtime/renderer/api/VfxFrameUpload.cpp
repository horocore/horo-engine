#include "Horo/Runtime/Render/VfxFrameUpload.h"

#include "Horo/Runtime/Render/RenderBackend.h"

namespace Horo::Render {
    namespace VfxFrameErrors {
        const ErrorCodeDescriptor InvalidInput{ErrorDomainId{"horo.render"}, ErrorCode{"render.vfx.frame.invalid"}, ErrorSeverity::Error,
                                               "The VFX uniform slot publication is invalid or stale.",
                                               "Supply exact prepared uniform ranges and a newer complete slot revision."};
        const ErrorCodeDescriptor Pending{ErrorDomainId{"horo.render"}, ErrorCode{"render.vfx.frame.pending"}, ErrorSeverity::Info,
                                          "Native commands still retain the VFX uniform slot.",
                                          "Keep the accepted publication and select another admitted free slot without waiting."};
        const ErrorCodeDescriptor Cancelled{ErrorDomainId{"horo.render"}, ErrorCode{"render.vfx.frame.cancelled"}, ErrorSeverity::Error,
                                            "The unpublished VFX frame was cancelled.",
                                            "Discard the candidate; the prior slot publication remains unchanged."};
        const ErrorCodeDescriptor Unavailable{ErrorDomainId{"horo.render"}, ErrorCode{"render.vfx.frame.unavailable"}, ErrorSeverity::Error,
                                              "The selected renderer cannot publish native VFX uniform slots.",
                                              "Select an explicitly qualified renderer; no fallback is performed."};
    }  // namespace VfxFrameErrors

    /** @copydoc PreparedVfxFrameSlot::CaptureDraw */
    void PreparedVfxFrameSlot::CaptureDraw(const RenderGraphVfx &workload, const std::uint32_t firstOrder, const std::uint32_t batch,
                                           const RenderMeshHandle mesh) {
        capture_ = workload.snapshot.data_;
        order_ = workload.order;
        stage_ = workload.stage;
        firstOrder_ = firstOrder;
        batch_ = batch;
        mesh_ = mesh;
    }

    /** @copydoc PreparedVfxFrameSlot::ValidateDraw */
    Result<void> PreparedVfxFrameSlot::ValidateDraw(const RenderGraphVfx &workload, const VfxPreparedDraw &draw) const {
        const auto order = order_.lock();
        if (!published_ || updating_ || owner_.expired() || !workload.order || !workload.order->nativePrepared || !order ||
            order.get() != workload.order.get() || order.owner_before(workload.order) || workload.order.owner_before(order) ||
            !workload.snapshot.data_ || capture_.lock() != workload.snapshot.data_ || draw.slot.get() != this ||
            draw.revision != revision_ || draw.instanceCount != instanceCount_ || draw.instanceCount == 0 ||
            draw.instanceCount > MaximumVfxDrawInstances || draw.firstOrder != firstOrder_ || draw.mesh != mesh_ ||
            workload.stage != stage_ || firstOrder_ >= order->instances.size() || instanceCount_ > order->instances.size() - firstOrder_ ||
            batch_ >= workload.snapshot.Batches().size())
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput, "VFX draw publication is absent, stale or foreign."));
        const auto &batch = workload.snapshot.Batches()[batch_];
        if (batch.material != material_->Descriptor().material ||
            (batch.topology == VfxPrimitiveTopology::InstancedMesh && mesh_ != batch.mesh))
            return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        for (std::size_t index = firstOrder_; index < firstOrder_ + instanceCount_; ++index)
            if (order->instances[index].batch != batch_ || order->instances[index].instance >= workload.snapshot.Instances().size() ||
                order->instances[index].instance < batch.firstInstance ||
                order->instances[index].instance - batch.firstInstance >= batch.instanceCount)
                return Result<void>::Failure(MakeError(VfxFrameErrors::InvalidInput));
        return Result<void>::Success();
    }

    /** @copydoc IRenderBackend::UpdateVfxFrame */
    Result<void> IRenderBackend::UpdateVfxFrame(const NativeVfxFrameUpdate &) {
        return Result<void>::Failure(MakeError(VfxFrameErrors::Unavailable));
    }
}  // namespace Horo::Render
