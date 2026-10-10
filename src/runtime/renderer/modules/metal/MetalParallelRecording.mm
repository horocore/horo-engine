#include "MetalParallelRecording.h"

#include "Horo/Foundation/JobSystem.h"
#include "MetalColorAttachmentEncoding.h"
#include "MetalRenderBackendErrors.h"

namespace Horo::Render::Detail {
    namespace {
        /** @brief Encodes the admitted copy on its independent buffer, never on an ambient worker context. */
        [[nodiscard]] Result<void> EncodeCopy(MetalRecordedOperation &operation, const RenderGraphBufferCopy &copy) {
            id<MTLBlitCommandEncoder> encoder = [operation.commands blitCommandEncoder];
            if (encoder == nil)
                return Result<void>::Failure(MakeError(MetalBackendErrors::CommandSubmissionFailed));
            [encoder copyFromBuffer:operation.source
                       sourceOffset:copy.sourceOffset
                           toBuffer:operation.destination
                  destinationOffset:copy.destinationOffset
                               size:copy.byteCount];
            [encoder endEncoding];
            return Result<void>::Success();
        }

        /** @brief Encodes the full load/store contract against an owned native color attachment. */
        [[nodiscard]] Result<void> EncodeColor(MetalRecordedOperation &operation, const PrimaryOutputAttachment &operations) {
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            auto *attachment = pass.colorAttachments[0];
            attachment.texture = operation.texture;
            ConfigureMetalColorAttachment(attachment, operations);
            id<MTLRenderCommandEncoder> encoder = [operation.commands renderCommandEncoderWithDescriptor:pass];
            if (encoder == nil)
                return Result<void>::Failure(MakeError(MetalBackendErrors::CommandSubmissionFailed));
            [encoder endEncoding];
            return Result<void>::Success();
        }

        /** @brief Dispatches only the prevalidated independent payload; no frontend/registry access is permitted. */
        [[nodiscard]] Result<void> Encode(MetalRecordedOperation &operation) {
            if (const auto *copy = std::get_if<RenderGraphBufferCopy>(&operation.workload))
                return EncodeCopy(operation, *copy);
            if (const auto *primary = std::get_if<PrimaryOutputAttachment>(&operation.workload))
                return EncodeColor(operation, *primary);
            if (const auto *color = std::get_if<RenderGraphColorAttachment>(&operation.workload))
                return EncodeColor(operation, color->operations);
            return Result<void>::Success();
        }
    }  // namespace

    MetalParallelRecording::MetalParallelRecording(const FrameToken frame, std::vector<MetalRecordedOperation> operations,
                                                   std::shared_ptr<MetalRecordingBudget> budget) noexcept
        : frame_(frame), operations_(std::move(operations)), budget_(std::move(budget)) {
        budget_->live.fetch_add(1, std::memory_order_relaxed);
    }

    MetalParallelRecording::~MetalParallelRecording() {
        operations_.clear();
        budget_->live.fetch_sub(1, std::memory_order_release);
    }

    FrameToken MetalParallelRecording::Frame() const noexcept {
        return frame_;
    }

    std::size_t MetalParallelRecording::PassCount() const noexcept {
        return operations_.size();
    }

    void MetalParallelRecording::Cancel() noexcept {
        activity_.Close();
    }

    bool MetalParallelRecording::Idle() const noexcept {
        return activity_.Idle();
    }

    Result<void> MetalParallelRecording::Record(const std::size_t index, const CancellationToken &cancellation) {
        if (index >= operations_.size())
            return Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
        const MetalRecordingActivity::RecordScope guard{activity_};
        if (!guard.CanRecord() || cancellation.IsCancellationRequested())
            return JobCancelled();
        std::uint8_t expected = 0;
        if (!states_[index].compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
            return Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
        @autoreleasepool {
            const auto encoded = Encode(operations_[index]);
            if (encoded.HasError())
                return encoded;
        }
        if (!guard.CanRecord() || cancellation.IsCancellationRequested())
            return JobCancelled();
        states_[index].store(2, std::memory_order_release);
        return Result<void>::Success();
    }

    Result<void> MetalParallelRecording::Accept() {
        if (accepted_ || activity_.Closed() || !Idle())
            return Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
        for (std::size_t index = 0; index < operations_.size(); ++index) {
            if (states_[index].load(std::memory_order_acquire) != 2)
                return Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
        }
        Cancel();
        accepted_ = true;
        return Result<void>::Success();
    }

    void MetalParallelRecording::Commit() noexcept {
        if (!accepted_)
            return;
        for (const auto &operation : operations_) {
            // These native-only callbacks preserve placement heaps even when explicit
            // teardown times out and closes the runtime. No callback touches owner pins.
            id<MTLHeap> sourceHeap = operation.sourceHeap;
            id<MTLHeap> destinationHeap = operation.destinationHeap;
            [operation.commands addCompletedHandler:^(id<MTLCommandBuffer>) {
              // Precise lifetimes make this a retention contract rather than an
              // otherwise optimizable discarded read of the captured native parents.
              __attribute__((objc_precise_lifetime)) id<MTLHeap> retainedSource = sourceHeap;
              __attribute__((objc_precise_lifetime)) id<MTLHeap> retainedDestination = destinationHeap;
              (void)retainedSource;
              (void)retainedDestination;
            }];
            [operation.commands commit];
        }
        accepted_ = false;
    }

    Result<bool> MetalParallelRecording::NativeComplete() const {
        bool complete = true;
        for (const auto &operation : operations_) {
            if (operation.commands.status == MTLCommandBufferStatusError)
                return Result<bool>::Failure(MakeError(MetalBackendErrors::CommandSubmissionFailed,
                                                       "A worker-recorded Metal command buffer failed on the GPU; restart the backend."));
            complete &= operation.commands.status == MTLCommandBufferStatusCompleted;
        }
        return Result<bool>::Success(complete);
    }
}  // namespace Horo::Render::Detail
