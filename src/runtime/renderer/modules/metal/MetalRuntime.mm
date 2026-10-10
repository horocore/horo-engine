#include "Horo/Runtime/Render/RenderAdapterErrors.h"
#include "MetalBackendInternal.h"
#include "MetalColorAttachmentEncoding.h"
#include "MetalCommandCompletion.h"
#include "MetalNativeDeviceFacts.h"
#include "MetalParallelRecording.h"
#include "MetalPresentationFeedback.h"
#include "MetalRenderBackendErrors.h"
#include "MetalResourceRuntime.h"
#include "MetalSubmittedGraphQueue.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAAnimation.h>
#import <QuartzCore/CAMetalLayer.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Render::Detail {
    namespace {
        [[nodiscard]] Error MakeMetalRuntimeError(const char *code, std::string message) {
            return Error{.code = ErrorCode{code},
                         .domain = ErrorDomainId{"horo.render.metal"},
                         .severity = ErrorSeverity::Error,
                         .message = std::move(message)};
        }

        [[nodiscard]] Result<void> ValidateInitializationRequest(const MetalPresentationDescriptor &descriptor) {
            if (!descriptor.enableValidation) {
                return Result<void>::Success();
            }
            const char *debugLayer = std::getenv("MTL_DEBUG_LAYER");
            if (debugLayer != nullptr && std::string_view{debugLayer} == "1") {
                return Result<void>::Success();
            }
            return Result<void>::Failure(
                MakeMetalRuntimeError("render.metal.validation_unavailable",
                                      "Metal validation must be enabled through MTL_DEBUG_LAYER=1 before device creation."));
        }

        class MetalRuntime final : public IMetalRuntime {
        public:
            using ResourceInstanceResult = Result<std::uint64_t>;

            MetalRuntime(IMetalPresentationPort &presentationPort, MetalEditorGraphicsBridge &editorGraphicsBridge) noexcept
                : presentationPort_(&presentationPort), editorGraphicsBridge_(&editorGraphicsBridge) {}

            ~MetalRuntime() override {
                Shutdown();
            }

            Result<MetalDeviceCapabilities> Initialize(const MetalPresentationDescriptor &descriptor,
                                                       const MetalDeviceAdmissionRequest &request) override {
                if (device_ != nil || surfaceCreated_) {
                    return Result<MetalDeviceCapabilities>::Failure(
                        MakeMetalRuntimeError("render.metal.presentation_exists",
                                              "Metal runtime presentation resources are already retained."));
                }
                const Result<void> validRequest = ValidateInitializationRequest(descriptor);
                if (validRequest.HasError()) {
                    return Result<MetalDeviceCapabilities>::Failure(validRequest.ErrorValue());
                }

                std::uint64_t discoveryRevision = 0;
                device_ = FindMetalDevice(request.adapter, discoveryRevision);
                if (device_ == nil) {
                    return Result<MetalDeviceCapabilities>::Failure(
                        MakeError(MetalBackendErrors::AdapterNotFound, "The requested Metal adapter is unavailable."));
                }
                // Eight live recording leases x sixteen buffers, at most four owner
                // buffers and sixty-four retained uploads stay below this native cap.
                // The editor bridge borrows our current buffer; it must not allocate its own.
                commandQueue_ = [device_ newCommandQueueWithMaxCommandBufferCount:MetalRecordingBudget::NativeQueueCapacity];
                const Result<MetalDeviceCapabilities> admitted =
                    AdmitMetalDevice(QueryMetalDeviceFacts(device_, discoveryRevision, commandQueue_ != nil), request);
                if (admitted.HasError()) {
                    Shutdown();
                    return Result<MetalDeviceCapabilities>::Failure(admitted.ErrorValue());
                }

                const Result<void> surface = presentationPort_->CreateSurface();
                if (surface.HasError()) {
                    Shutdown();
                    return Result<MetalDeviceCapabilities>::Failure(surface.ErrorValue());
                }
                surfaceCreated_ = true;
                layer_ = (__bridge CAMetalLayer *)presentationPort_->Layer();
                if (layer_ == nil) {
                    Shutdown();
                    return Result<MetalDeviceCapabilities>::Failure(
                        MakeMetalRuntimeError("render.metal.layer_unavailable",
                                              "The platform presentation port did not expose a CAMetalLayer."));
                }

                layer_.device = device_;
                layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
                layer_.framebufferOnly = YES;
                layer_.opaque = YES;
                layer_.maximumDrawableCount = descriptor.maxFramesInFlight;
                maxFramesInFlight_ = descriptor.maxFramesInFlight;
                feedback_ = std::make_shared<MetalPresentationFeedback>();
                ownerThread_ = std::this_thread::get_id();
                submitted_.Initialize(maxFramesInFlight_);
                layer_.displaySyncEnabled = descriptor.presentMode == PresentMode::Fifo;
                MetalEditorGraphicsAccess::PublishPersistent(*editorGraphicsBridge_, (__bridge void *)device_,
                                                             (__bridge void *)commandQueue_, this, &WaitUntilIdleThunk);
                resources_.Initialize((__bridge void *)device_, (__bridge void *)commandQueue_);
                return admitted;
            }

            Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const override {
                return resources_.QueryBufferMemoryCost(descriptor);
            }

            Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const override {
                return resources_.QueryTextureMemoryCost(descriptor);
            }

            ResourceInstanceResult CreateBuffer(const RenderBufferDescriptor &descriptor, const std::span<const std::byte> initialData,
                                                const RenderMemoryPlacement &placement) override {
                return Resources().CreateBuffer(descriptor, initialData, placement);
            }

            ResourceInstanceResult CreateMesh(const RenderMeshDescriptor &descriptor, const std::uint64_t vertexBuffer,
                                              const std::uint64_t indexBuffer) override {
                return Resources().CreateMesh(descriptor, vertexBuffer, indexBuffer);
            }

            ResourceInstanceResult CreateTexture(const RenderTextureDescriptor &descriptor, const std::span<const std::byte> initialData,
                                                 const RenderMemoryPlacement &placement) override {
                return Resources().CreateTexture(descriptor, initialData, placement);
            }

            ResourceInstanceResult CreateTextureView(const RenderTextureViewDescriptor &descriptor, const std::uint64_t texture) override {
                return Resources().CreateTextureView(descriptor, texture);
            }

            ResourceInstanceResult CreateRenderTarget(const RenderTargetDescriptor &descriptor, const std::uint64_t colorAttachment,
                                                      const std::uint64_t depthAttachment) override {
                return Resources().CreateRenderTarget(descriptor, colorAttachment, depthAttachment);
            }

            void DestroyBuffer(const std::uint64_t backendInstance) noexcept override {
                resources_.DestroyBuffer(backendInstance);
            }

            void DestroyMesh(const std::uint64_t backendInstance) noexcept override {
                resources_.DestroyMesh(backendInstance);
            }

            void DestroyTexture(const std::uint64_t backendInstance) noexcept override {
                resources_.DestroyTexture(backendInstance);
            }

            void DestroyTextureView(const std::uint64_t backendInstance) noexcept override {
                resources_.DestroyTextureView(backendInstance);
            }

            void DestroyRenderTarget(const std::uint64_t backendInstance) noexcept override {
                resources_.DestroyRenderTarget(backendInstance);
            }

            Result<void> BeginFrame(const FramebufferExtent extent) override {
                if (device_ == nil || commandQueue_ == nil || layer_ == nil) {
                    return Result<void>::Failure(
                        MakeMetalRuntimeError("render.metal.not_initialized", "Metal runtime presentation resources are not initialized."));
                }
                if (commandBuffer_ != nil || drawable_ != nil) {
                    return Result<void>::Failure(
                        MakeMetalRuntimeError("render.metal.frame_already_active", "A Metal presentation frame is already active."));
                }
                if (const Result<void> available = CheckSubmissionCapacity(); available.HasError()) {
                    return available;
                }
                resources_.DrainGraphRetirements();
                if (const Result<void> resized = Resize(extent); resized.HasError()) {
                    return resized;
                }

                @autoreleasepool {
                    drawable_ = [layer_ nextDrawable];
                    commandBuffer_ = [commandQueue_ commandBuffer];
                    if (drawable_ == nil || commandBuffer_ == nil) {
                        drawable_ = nil;
                        commandBuffer_ = nil;
                        return Result<void>::Failure(MakeMetalRuntimeError("render.metal.frame_acquisition_failed",
                                                                           "Failed to acquire a Metal drawable or command buffer."));
                    }
                    renderPassDescriptor_ = [MTLRenderPassDescriptor renderPassDescriptor];
                    renderPassDescriptor_.colorAttachments[0].texture = drawable_.texture;
                    renderPassDescriptor_.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                    renderPassDescriptor_.colorAttachments[0].storeAction = MTLStoreActionStore;
                }
                PublishFrame();
                return Result<void>::Success();
            }

            Result<void> ExecutePrimaryOutput(const PrimaryOutputAttachment &attachment) override {
                if (std::this_thread::get_id() != ownerThread_) {
                    return WrongThread();
                }
                if (commandBuffer_ == nil || renderPassDescriptor_ == nil) {
                    return Result<void>::Failure(
                        MakeMetalRuntimeError("render.metal.no_active_frame", "No Metal presentation frame is active."));
                }
                EndPrimaryEncoder();

                MTLRenderPassColorAttachmentDescriptor *color = renderPassDescriptor_.colorAttachments[0];
                ConfigureMetalColorAttachment(color, attachment);
                renderEncoder_ = [commandBuffer_ renderCommandEncoderWithDescriptor:renderPassDescriptor_];
                if (renderEncoder_ == nil) {
                    return Result<void>::Failure(MakeMetalRuntimeError("render.metal.encoder_creation_failed",
                                                                       "Failed to create the primary Metal render encoder."));
                }
                [renderEncoder_ pushDebugGroup:@"Horo Primary Output"];
                PublishFrame();
                return Result<void>::Success();
            }

            Result<void> ValidateGraphWorkload(const RenderGraphWorkload &workload,
                                               const std::span<const RenderGraphResourceInstance> resources) const override {
                if (std::this_thread::get_id() != ownerThread_) {
                    return WrongThread();
                }
                if (commandBuffer_ == nil) {
                    return Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
                }
                return resources_.ValidateGraphWorkload(workload, resources);
            }

            Result<void> ExecuteGraphWorkload(const RenderGraphWorkload &workload,
                                              const std::span<const RenderGraphResourceInstance> resources) override {
                if (const auto valid = ValidateGraphWorkload(workload, resources); valid.HasError()) {
                    return valid;
                }
                if (const auto *primary = std::get_if<PrimaryOutputAttachment>(&workload)) {
                    return ExecutePrimaryOutput(*primary);
                }
                EndPrimaryEncoder();
                return resources_.ExecuteGraphWorkload((__bridge void *)commandBuffer_, workload, resources);
            }

            Result<void> RetainGraphResources(IRenderGraphResourceLease &lease) override {
                if (std::this_thread::get_id() != ownerThread_) {
                    return WrongThread();
                }
                if (commandBuffer_ == nil || activeGraphLease_ != nullptr) {
                    return Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
                }
                activeGraphLease_ = &lease;
                return Result<void>::Success();
            }

            RenderParallelRecordingCapabilities ParallelRecordingCapabilities() const noexcept override {
                return {MetalRecordingBudget::MaximumPasses, MetalRecordingBudget::MaximumLiveRecordings};
            }

            Result<std::shared_ptr<IRenderParallelGraphRecording>> PrepareParallelGraph(
                const RenderGraphExecutionRequest &request) override {
                using RecordingResult = Result<std::shared_ptr<IRenderParallelGraphRecording>>;
                if (std::this_thread::get_id() != ownerThread_)
                    return RecordingResult::Failure(MakeError(MetalBackendErrors::WrongThread));
                DrainCancelledRecordings();
                if (const auto admitted = ValidateParallelCapture(request); admitted.HasError())
                    return RecordingResult::Failure(admitted.ErrorValue());
                try {
                    auto operations = CaptureNativeOperations(request);
                    if (operations.HasError())
                        return RecordingResult::Failure(operations.ErrorValue());
                    std::vector<RenderGraphResourceInstance> residents(request.resources.begin(), request.resources.end());
                    auto recording =
                        std::make_shared<MetalParallelRecording>(request.frame, std::move(operations).Value(), recordingBudget_);
                    activeRecording_ = recording;
                    activeRecordingResources_ = std::move(residents);
                    for (const auto &binding : request.workloads) {
                        if (const auto *primary = std::get_if<PrimaryOutputAttachment>(&binding.workload))
                            activeParallelPrimary_ = *primary;
                    }
                    activeGraphLease_ = request.lease;
                    return RecordingResult::Success(std::move(recording));
                } catch (const std::bad_alloc &) {
                    return RecordingResult::Failure(MakeError(MetalBackendErrors::ResourceCreationFailed));
                }
            }

            Result<void> AcceptParallelGraph(const std::shared_ptr<IRenderParallelGraphRecording> &recording) override {
                if (std::this_thread::get_id() != ownerThread_)
                    return WrongThread();
                if (commandBuffer_ == nil || !activeRecording_ || recording.get() != activeRecording_.get())
                    return Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
                if (const auto accepted = activeRecording_->Accept(); accepted.HasError())
                    return accepted;
                // Native workers end their independent primary encoder. The existing GUI
                // bridge continues on the owner buffer after all graph buffers, preserving
                // stored primary contents instead of clearing them again or using a worker context.
                if (activeParallelPrimary_ && activeParallelPrimary_->storeOperation == AttachmentStoreOperation::Store) {
                    const auto continuation = ExecutePrimaryOutput(
                        {.loadOperation = AttachmentLoadOperation::Load, .storeOperation = AttachmentStoreOperation::Store});
                    if (continuation.HasError())
                        return continuation;
                }
                resources_.TrackParallelGraphUse((__bridge void *)commandBuffer_, activeRecordingResources_);
                recordingAccepted_ = true;
                return Result<void>::Success();
            }

            /** @copydoc IMetalRuntime::PresentWithTiming */
            Result<void> PresentWithTiming(const PresentationTimingRequest &request) override {
                if (std::this_thread::get_id() != ownerThread_)
                    return WrongThread();
                if (!request.surface.IsAttachedGeneration() || request.frameNumber == 0 || drawable_ == nil || commandBuffer_ == nil)
                    return Result<void>::Failure(MakeError(FramePacingErrors::InvalidSurface));
                const Duration before = request.clock.MonotonicNow();
                const double nativeNow = CACurrentMediaTime();
                const Duration after = request.clock.MonotonicNow();
                const MetalPresentationCalibration calibration{nativeNow, before, after};
                const auto feedback = feedback_;
                const auto surface = request.surface;
                const auto frame = request.frameNumber;
                if (feedback->SelectSurface(surface)) {
                    [drawable_ addPresentedHandler:^(id<MTLDrawable> displayed) {
                      if (const auto timing = calibration.Translate(surface, frame, displayed.presentedTime))
                          feedback->Publish(*timing);
                      else
                          feedback->Discard();
                    }];
                }
                return Present();
            }

            /** @copydoc IMetalRuntime::PollNativePresentTiming */
            Result<std::optional<NativePresentTiming>> PollNativePresentTiming() override {
                if (std::this_thread::get_id() != ownerThread_)
                    return Result<std::optional<NativePresentTiming>>::Failure(MakeError(MetalBackendErrors::WrongThread));
                auto timing = feedback_->Poll();
                if (timing)
                    timing->discardedObservations = feedback_->DroppedCount();
                return Result<std::optional<NativePresentTiming>>::Success(timing);
            }

            Result<void> Present() override {
                if (std::this_thread::get_id() != ownerThread_) {
                    return WrongThread();
                }
                if (commandBuffer_ == nil || drawable_ == nil) {
                    return Result<void>::Failure(
                        MakeMetalRuntimeError("render.metal.no_active_frame", "No Metal presentation frame is active."));
                }
                EndPrimaryEncoder();
                [commandBuffer_ presentDrawable:drawable_];
                if (activeRecording_ && !recordingAccepted_)
                    return Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
                if (activeRecording_)
                    activeRecording_->Commit();
                lastSubmittedCommandBuffer_ = commandBuffer_;
                submitted_.Remember(commandBuffer_, activeGraphLease_, activeRecording_);
                resources_.FinishGraphCommands((__bridge void *)commandBuffer_, true);
                [commandBuffer_ commit];
                ClearActiveFrame();
                return Result<void>::Success();
            }

            void AbortFrame() noexcept override {
                EndPrimaryEncoder();
                if (activeRecording_)
                    activeRecording_->Cancel();
                resources_.FinishGraphCommands((__bridge void *)commandBuffer_, false);
                if (activeRecording_ && !activeRecording_->Idle()) {
                    for (auto &retired : cancelledRecordings_) {
                        if (!retired.recording) {
                            retired = {std::move(activeRecording_), activeGraphLease_};
                            activeGraphLease_ = nullptr;
                            break;
                        }
                    }
                }
                if (activeGraphLease_ != nullptr) {
                    activeGraphLease_->Release();
                    activeGraphLease_ = nullptr;
                }
                activeRecording_.reset();
                ClearActiveFrame();
            }

            Result<void> Resize(const FramebufferExtent extent) override {
                if (!extent.IsValid()) {
                    return Result<void>::Failure(
                        MakeMetalRuntimeError("render.metal.invalid_extent", "Metal drawable extent must be non-zero."));
                }
                if (layer_ == nil) {
                    return Result<void>::Failure(
                        MakeMetalRuntimeError("render.metal.not_initialized", "Metal runtime presentation resources are not initialized."));
                }
                if (std::this_thread::get_id() != ownerThread_) {
                    return WrongThread();
                }
                layer_.drawableSize = CGSizeMake(static_cast<CGFloat>(extent.width), static_cast<CGFloat>(extent.height));
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {
                feedback_->Close();
                AbortFrame();
                WaitUntilIdle();
                // Closing the domain prevents submission/reuse. Outstanding CPU capsules own
                // their native resources AND heaps and can finish cancellation after this runtime.
                for (auto &retired : cancelledRecordings_) {
                    if (retired.recording)
                        retired.recording->Cancel();
                    if (retired.lease != nullptr)
                        retired.lease->Release();
                    retired = {};
                }
                submitted_.ReleaseLeases();
                resources_.Shutdown();
                submitted_.Clear();
                MetalEditorGraphicsAccess::Clear(*editorGraphicsBridge_);
                if (layer_ != nil) {
                    layer_.device = nil;
                    layer_ = nil;
                }
                lastSubmittedCommandBuffer_ = nil;
                submissionError_.reset();
                ownerThread_ = {};
                commandQueue_ = nil;
                device_ = nil;
                if (surfaceCreated_) {
                    presentationPort_->DestroySurface();
                    surfaceCreated_ = false;
                }
            }

        private:
            /** @brief Checks native capture state and all command-count bounds before any buffer allocation. */
            [[nodiscard]] Result<void> ValidateParallelCapture(const RenderGraphExecutionRequest &request) const {
                if (commandBuffer_ == nil || drawable_ == nil || renderEncoder_ != nil || activeGraphLease_ != nullptr || activeRecording_)
                    return Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
                if (request.workloads.size() > MetalRecordingBudget::MaximumPasses)
                    return Result<void>::Failure(
                        MakeError(MetalBackendErrors::UnsupportedGraphExecution,
                                  "Metal parallel graphs admit at most sixteen independent command buffers per frame."));
                if (recordingBudget_->live.load(std::memory_order_acquire) >= MetalRecordingBudget::MaximumLiveRecordings)
                    return Result<void>::Failure(
                        MakeError(MetalBackendErrors::SubmissionBusy,
                                  "Cancelled worker or submitted GPU recording leases still occupy the bounded native budget."));
                return Result<void>::Success();
            }

            /** @brief Freezes every worker buffer/resource parent before publishing any native session or owner lease. */
            [[nodiscard]] Result<std::vector<MetalRecordedOperation>> CaptureNativeOperations(const RenderGraphExecutionRequest &request) {
                using CaptureResult = Result<std::vector<MetalRecordedOperation>>;
                std::vector<MetalRecordedOperation> operations;
                operations.reserve(request.workloads.size());
                for (const auto &binding : request.workloads) {
                    id<MTLCommandBuffer> commands = [commandQueue_ commandBuffer];
                    if (commands == nil)
                        return CaptureResult::Failure(MakeError(MetalBackendErrors::CommandSubmissionFailed));
                    auto captured = resources_.CaptureGraphOperation(binding.workload, request.resources, commands, drawable_.texture);
                    if (captured.HasError())
                        return CaptureResult::Failure(captured.ErrorValue());
                    auto operation = std::move(captured).Value();
                    if (std::holds_alternative<PrimaryOutputAttachment>(binding.workload))
                        operation.presentationDrawable = drawable_;
                    operations.push_back(std::move(operation));
                }
                return CaptureResult::Success(std::move(operations));
            }

            /** @brief Rejects command access outside the initialized render thread without mutating native state. */
            [[nodiscard]] static Result<void> WrongThread() {
                return Result<void>::Failure(MakeError(MetalBackendErrors::WrongThread));
            }

            /** @brief Polls bounded retained submissions; ordinary frames never wait for GPU completion. */
            [[nodiscard]] Result<void> CheckSubmissionCapacity() {
                if (std::this_thread::get_id() != ownerThread_) {
                    return WrongThread();
                }
                if (submissionError_) {
                    return Result<void>::Failure(*submissionError_);
                }
                DrainCancelledRecordings();
                if (const auto polled = submitted_.Poll(); polled.HasError()) {
                    submissionError_ = polled.ErrorValue();
                    return polled;
                }
                if (submitted_.Count() >= maxFramesInFlight_)
                    return Result<void>::Failure(MakeError(MetalBackendErrors::SubmissionBusy));
                const std::size_t gpuRecordings = submitted_.RecordingCount();
                if (recordingBudget_->live.load(std::memory_order_acquire) > static_cast<std::size_t>(gpuRecordings)) {
                    // A cancelled callback may still retain its CAMetalDrawable parent. Do
                    // not enter nextDrawable and accidentally wait for that CPU lease to drain.
                    return Result<void>::Failure(
                        MakeError(MetalBackendErrors::SubmissionBusy,
                                  "Cancelled CPU recording leases must drain before acquiring another presentation drawable."));
                }
                return Result<void>::Success();
            }

            [[nodiscard]] MetalResourceRuntime &Resources() noexcept {
                return resources_;
            }

            /** @brief Owner safe-point polling; closed workers never release registry pins themselves. */
            void DrainCancelledRecordings() noexcept {
                for (auto &retired : cancelledRecordings_) {
                    if (retired.recording && retired.recording->Idle()) {
                        if (retired.lease != nullptr)
                            retired.lease->Release();
                        retired = {};
                    }
                }
            }

            static void WaitUntilIdleThunk(void *context) noexcept {
                static_cast<MetalRuntime *>(context)->WaitUntilIdle();
            }

            void WaitUntilIdle() noexcept {
                if (ObserveMetalCommandForTeardown(lastSubmittedCommandBuffer_) != MetalCommandCompletion::Completed) {
                    submissionError_ = MakeError(MetalBackendErrors::CommandSubmissionFailed,
                                                 "Metal explicit idle/teardown did not observe successful completion within its finite "
                                                 "budget; close or restart the backend.");
                }
            }

            void EndPrimaryEncoder() noexcept {
                if (renderEncoder_ != nil) {
                    [renderEncoder_ popDebugGroup];
                    [renderEncoder_ endEncoding];
                    renderEncoder_ = nil;
                    PublishFrame();
                }
            }

            void PublishFrame() noexcept {
                MetalEditorGraphicsAccess::PublishFrame(*editorGraphicsBridge_, (__bridge void *)commandBuffer_,
                                                        (__bridge void *)renderPassDescriptor_, (__bridge void *)renderEncoder_);
            }

            void ClearActiveFrame() noexcept {
                recordingAccepted_ = false;
                activeRecordingResources_.clear();
                activeParallelPrimary_.reset();
                renderPassDescriptor_ = nil;
                commandBuffer_ = nil;
                drawable_ = nil;
                MetalEditorGraphicsAccess::ClearFrame(*editorGraphicsBridge_);
            }

            IMetalPresentationPort *presentationPort_{nullptr};
            MetalEditorGraphicsBridge *editorGraphicsBridge_{nullptr};
            __strong CAMetalLayer *layer_{nil};
            __strong id<MTLDevice> device_{nil};
            __strong id<MTLCommandQueue> commandQueue_{nil};
            __strong id<CAMetalDrawable> drawable_{nil};
            __strong id<MTLCommandBuffer> commandBuffer_{nil};
            __strong id<MTLRenderCommandEncoder> renderEncoder_{nil};
            __strong MTLRenderPassDescriptor *renderPassDescriptor_{nil};
            __strong id<MTLCommandBuffer> lastSubmittedCommandBuffer_{nil};
            MetalSubmittedGraphQueue submitted_;
            std::shared_ptr<MetalPresentationFeedback> feedback_{std::make_shared<MetalPresentationFeedback>()};
            IRenderGraphResourceLease *activeGraphLease_{nullptr};

            struct CancelledRecording {
                std::shared_ptr<MetalParallelRecording> recording;
                IRenderGraphResourceLease *lease{nullptr};
            };

            std::array<CancelledRecording, MetalRecordingBudget::MaximumLiveRecordings> cancelledRecordings_{};
            std::shared_ptr<MetalParallelRecording> activeRecording_;
            std::vector<RenderGraphResourceInstance> activeRecordingResources_;
            std::optional<PrimaryOutputAttachment> activeParallelPrimary_;
            std::shared_ptr<MetalRecordingBudget> recordingBudget_{std::make_shared<MetalRecordingBudget>()};
            bool recordingAccepted_{false};
            std::optional<Error> submissionError_;
            std::thread::id ownerThread_;
            std::uint32_t maxFramesInFlight_{0};
            MetalResourceRuntime resources_;
            bool surfaceCreated_{false};
        };
    }  // namespace

    Result<std::unique_ptr<IMetalRuntime>> CreateMetalRuntime(IMetalPresentationPort &presentationPort,
                                                              MetalEditorGraphicsBridge &editorGraphicsBridge) {
        return Result<std::unique_ptr<IMetalRuntime>>::Success(std::make_unique<MetalRuntime>(presentationPort, editorGraphicsBridge));
    }
}  // namespace Horo::Render::Detail
