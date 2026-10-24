#include "Horo/Runtime/Render/RenderAdapterErrors.h"
#include "MetalBackendInternal.h"
#include "MetalColorAttachmentEncoding.h"
#include "MetalCommandCompletion.h"
#include "MetalDrawableAcquisition.h"
#include "MetalNativeDeviceFacts.h"
#include "MetalParallelRecording.h"
#include "MetalPresentationFeedback.h"
#include "MetalPresentationOutput.h"
#include "MetalRenderBackendErrors.h"
#include "MetalResourceRuntime.h"
#include "MetalSubmittedGraphQueue.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAAnimation.h>
#import <QuartzCore/CAMetalLayer.h>
#include <algorithm>
#include <array>
#include <cstdlib>
#include <exception>
#include <format>
#include <limits>
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
                const auto begun = BeginShutdown();
                const auto drained = PollDrain();
                if (begun.HasError() || drained.HasError() || drained.Value() != RenderBackendDrainProgress::Drained)
                    std::terminate();
            }

            Result<MetalDeviceCapabilities> Initialize(const MetalPresentationDescriptor &descriptor,
                                                       const MetalDeviceAdmissionRequest &request) override {
                if (ownerThread_ != std::thread::id{} && std::this_thread::get_id() != ownerThread_)
                    return Result<MetalDeviceCapabilities>::Failure(MakeError(MetalBackendErrors::WrongThread));
                if (device_ != nil || surfaceCreated_ || (closing_ && !drained_)) {
                    return Result<MetalDeviceCapabilities>::Failure(
                        MakeMetalRuntimeError("render.metal.presentation_exists",
                                              "Metal runtime presentation resources are already retained."));
                }
                const Result<void> validRequest = ValidateInitializationRequest(descriptor);
                if (validRequest.HasError()) {
                    return Result<MetalDeviceCapabilities>::Failure(validRequest.ErrorValue());
                }
                ownerThread_ = std::this_thread::get_id();
                closing_ = false;
                drained_ = false;
                drainError_.reset();
                drainErrorReported_ = false;
                drawableExtent_ = {};

                auto admitted = AcquireAdmittedDevice(request);
                if (admitted.HasError()) {
                    CloseUnsubmittedInitialization();
                    return Result<MetalDeviceCapabilities>::Failure(admitted.ErrorValue());
                }
                if (const auto prepared = PreparePresentation(descriptor, request.requirePresentation); prepared.HasError()) {
                    CloseUnsubmittedInitialization();
                    return Result<MetalDeviceCapabilities>::Failure(prepared.ErrorValue());
                }

                maxFramesInFlight_ = descriptor.maxFramesInFlight;
                feedback_ = std::make_shared<MetalPresentationFeedback>();
                ownerThread_ = std::this_thread::get_id();
                submitted_.Initialize(maxFramesInFlight_);
                MetalEditorGraphicsAccess::PublishPersistent(*editorGraphicsBridge_, (__bridge void *)device_,
                                                             (__bridge void *)commandQueue_, this, &WaitUntilIdleThunk);
                resources_.Initialize((__bridge void *)device_, (__bridge void *)commandQueue_);
                auto capabilities = std::move(admitted).Value();
                const auto incarnation = AcquireRenderDeviceIncarnation();
                if (incarnation.HasError()) {
                    CloseUnsubmittedInitialization();
                    return Result<MetalDeviceCapabilities>::Failure(incarnation.ErrorValue());
                }
                capabilities.deviceIncarnation = incarnation.Value();
                capabilities.implemented.support.deviceIncarnation = incarnation.Value();
                capabilities.implemented.support.features.Enable(RenderCapability::LightCulling);
                if (SupportsMetalUiPaint(capabilities))
                    capabilities.implemented.support.features.Enable(RenderCapability::UiPaint);
                auto acquisition = MetalDrawableAcquisition::Create(layer_, device_, MakeMetalDrawableSource());
                if (acquisition.HasError()) {
                    CloseUnsubmittedInitialization();
                    return Result<MetalDeviceCapabilities>::Failure(acquisition.ErrorValue());
                }
                acquisition_ = std::move(acquisition).Value();
                return Result<MetalDeviceCapabilities>::Success(std::move(capabilities));
            }

            Result<std::shared_ptr<IResidentLightCullingKernel>> RealizeLightCullingKernel(
                const CookedLightCullingKernel &kernel) override {
                using Preparation = Result<std::shared_ptr<IResidentLightCullingKernel>>;
                if (std::this_thread::get_id() != ownerThread_)
                    return Preparation::Failure(MakeError(MetalBackendErrors::WrongThread));
                if (commandBuffer_ != nil)
                    return Preparation::Failure(
                        MakeError(LightCullingErrors::InvalidInput, "Kernel realization requires a preparation safe point."));
                return resources_.RealizeLightCullingKernel(kernel);
            }

            Result<std::shared_ptr<IResidentUiPaintKernel>> RealizeUiPaintKernel(const CookedUiPaintKernel &kernel) override {
                using Preparation = Result<std::shared_ptr<IResidentUiPaintKernel>>;
                if (std::this_thread::get_id() != ownerThread_)
                    return Preparation::Failure(MakeError(MetalBackendErrors::WrongThread));
                if (commandBuffer_ != nil)
                    return Preparation::Failure(
                        MakeError(MetalBackendErrors::InvalidExecutionPlan, "UI pipeline realization requires a preparation safe point."));
                return resources_.RealizeUiPaintKernel(kernel);
            }

            Result<void> UpdateUiPaintFrame(const NativeUiPaintFrameUpdate &update) override {
                if (std::this_thread::get_id() != ownerThread_)
                    return WrongThread();
                if (commandBuffer_ != nil)
                    return Result<void>::Failure(
                        MakeError(MetalBackendErrors::InvalidExecutionPlan, "UI slots update before native frame acquisition."));
                return resources_.UpdateUiPaintFrame(update);
            }

            Result<void> UpdateLightFrame(const NativeLightFrameUpdate &update) override {
                if (std::this_thread::get_id() != ownerThread_)
                    return WrongThread();
                if (commandBuffer_ != nil)
                    return Result<void>::Failure(
                        MakeError(LightCullingErrors::InvalidInput, "Light slots update only outside an active frame."));
                return resources_.UpdateLightFrame(update);
            }

            /** @copydoc IRenderResourceBackend::QueryPipelineDeviceIdentity */
            Result<RenderPipelineDeviceIdentity> QueryPipelineDeviceIdentity() const override {
                if (const auto access = ValidatePreparationAccess(); access.HasError())
                    return Result<RenderPipelineDeviceIdentity>::Failure(access.ErrorValue());
                return resources_.QueryPipelineDeviceIdentity();
            }

            /** @copydoc IRenderResourceBackend::QueryShaderModuleMemoryCost */
            Result<RenderMemoryCostPlan> QueryShaderModuleMemoryCost(const RenderShaderModuleDescriptor &descriptor,
                                                                     std::size_t cookedBytes) const override {
                if (const auto access = ValidatePreparationAccess(); access.HasError())
                    return Result<RenderMemoryCostPlan>::Failure(access.ErrorValue());
                return resources_.QueryShaderModuleMemoryCost(descriptor, cookedBytes);
            }

            /** @copydoc IRenderResourceBackend::QueryGraphicsPipelineMemoryCost */
            Result<RenderMemoryCostPlan> QueryGraphicsPipelineMemoryCost(const RenderGraphicsPipelineDescriptor &descriptor,
                                                                         std::uint64_t vertex, std::uint64_t fragment) const override {
                if (const auto access = ValidatePreparationAccess(); access.HasError())
                    return Result<RenderMemoryCostPlan>::Failure(access.ErrorValue());
                return resources_.QueryGraphicsPipelineMemoryCost(descriptor, vertex, fragment);
            }

            /** @copydoc IRenderResourceBackend::QuerySamplerMemoryCost */
            Result<RenderMemoryCostPlan> QuerySamplerMemoryCost(const RenderSamplerDescriptor &descriptor) const override {
                if (const auto access = ValidatePreparationAccess(); access.HasError())
                    return Result<RenderMemoryCostPlan>::Failure(access.ErrorValue());
                return resources_.QuerySamplerMemoryCost(descriptor);
            }

            /** @copydoc IRenderResourceBackend::CreateShaderModule */
            Result<std::uint64_t> CreateShaderModule(const RenderShaderModuleDescriptor &descriptor,
                                                     std::span<const std::uint8_t> cookedCode,
                                                     const RenderMemoryPlacement &placement) override {
                if (const auto access = ValidatePreparationAccess(); access.HasError())
                    return Result<std::uint64_t>::Failure(access.ErrorValue());
                return resources_.CreateShaderModule(descriptor, cookedCode, placement);
            }

            /** @copydoc IRenderResourceBackend::CreateGraphicsPipeline */
            Result<std::uint64_t> CreateGraphicsPipeline(const RenderGraphicsPipelineDescriptor &descriptor, std::uint64_t vertex,
                                                         std::uint64_t fragment, const RenderMemoryPlacement &placement) override {
                if (const auto access = ValidatePreparationAccess(); access.HasError())
                    return Result<std::uint64_t>::Failure(access.ErrorValue());
                return resources_.CreateGraphicsPipeline(descriptor, vertex, fragment, placement);
            }

            /** @copydoc IRenderResourceBackend::CreateSampler */
            Result<std::uint64_t> CreateSampler(const RenderSamplerDescriptor &descriptor,
                                                const RenderMemoryPlacement &placement) override {
                if (const auto access = ValidatePreparationAccess(); access.HasError())
                    return Result<std::uint64_t>::Failure(access.ErrorValue());
                return resources_.CreateSampler(descriptor, placement);
            }

            /** @copydoc IMaterialBindingBackend::QueryMaterialParameterBufferSize */
            Result<std::size_t> QueryMaterialParameterBufferSize(std::uint64_t pipeline, const MaterialBindingDescriptor &descriptor,
                                                                 ShaderBindingId binding) const override {
                if (const auto access = ValidatePreparationAccess(); access.HasError())
                    return Result<std::size_t>::Failure(access.ErrorValue());
                return resources_.QueryMaterialParameterBufferSize(pipeline, descriptor, binding);
            }

            /** @copydoc IMaterialBindingBackend::RealizeResolved */
            Result<std::unique_ptr<IResidentMaterialBinding>> RealizeResolved(
                const MaterialBindingDescriptor &descriptor, std::uint64_t pipeline,
                std::span<const MaterialResidentResource> resources) override {
                if (const auto access = ValidatePreparationAccess(); access.HasError())
                    return Result<std::unique_ptr<IResidentMaterialBinding>>::Failure(access.ErrorValue());
                return resources_.RealizeResolved(descriptor, pipeline, resources);
            }

            /** @copydoc IRenderResourceBackend::DestroyShaderModule */
            void DestroyShaderModule(std::uint64_t instance) noexcept override {
                if (std::this_thread::get_id() == ownerThread_)
                    resources_.DestroyShaderModule(instance);
            }

            /** @copydoc IRenderResourceBackend::DestroyGraphicsPipeline */
            void DestroyGraphicsPipeline(std::uint64_t instance) noexcept override {
                if (std::this_thread::get_id() == ownerThread_)
                    resources_.DestroyGraphicsPipeline(instance);
            }

            /** @copydoc IRenderResourceBackend::DestroySampler */
            void DestroySampler(std::uint64_t instance) noexcept override {
                if (std::this_thread::get_id() == ownerThread_)
                    resources_.DestroySampler(instance);
            }

            Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const override {
                return resources_.QueryBufferMemoryCost(descriptor);
            }

            Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const override {
                return resources_.QueryTextureMemoryCost(descriptor);
            }

            ResourceInstanceResult CreateBuffer(const RenderBufferDescriptor &descriptor, const std::span<const std::byte> initialData,
                                                const RenderMemoryPlacement &placement, RenderResourceNativeLifetime lifetime) override {
                return Resources().CreateBuffer(descriptor, initialData, placement, std::move(lifetime));
            }

            ResourceInstanceResult CreateMesh(const RenderMeshDescriptor &descriptor, const std::uint64_t vertexBuffer,
                                              const std::uint64_t indexBuffer, RenderResourceNativeLifetime lifetime) override {
                return Resources().CreateMesh(descriptor, vertexBuffer, indexBuffer, std::move(lifetime));
            }

            ResourceInstanceResult CreateTexture(const RenderTextureDescriptor &descriptor, const std::span<const std::byte> initialData,
                                                 const RenderMemoryPlacement &placement, RenderResourceNativeLifetime lifetime) override {
                return Resources().CreateTexture(descriptor, initialData, placement, std::move(lifetime));
            }

            ResourceInstanceResult CreateTextureView(const RenderTextureViewDescriptor &descriptor, const std::uint64_t texture,
                                                     RenderResourceNativeLifetime lifetime) override {
                return Resources().CreateTextureView(descriptor, texture, std::move(lifetime));
            }

            ResourceInstanceResult CreateRenderTarget(const RenderTargetDescriptor &descriptor, const std::uint64_t colorAttachment,
                                                      const std::uint64_t depthAttachment, RenderResourceNativeLifetime lifetime) override {
                return Resources().CreateRenderTarget(descriptor, colorAttachment, depthAttachment, std::move(lifetime));
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

            Result<RenderPresentationOutput> ConfigurePresentationOutput(const RenderPresentationOutputRequest &request) override {
                if (const auto access = ValidatePreparationAccess(); access.HasError())
                    return Result<RenderPresentationOutput>::Failure(access.ErrorValue());
                const auto display = presentationPort_->OutputDisplayFacts();
                if (display.HasError())
                    return Result<RenderPresentationOutput>::Failure(display.ErrorValue());
                if (const auto candidate = NegotiatePresentationOutput(request, outputCapabilities_, display.Value(), outputRevision_ + 1);
                    candidate.HasError())
                    return candidate;
                if (outputRevision_ == std::numeric_limits<std::uint64_t>::max() ||
                    drawableGeneration_ == std::numeric_limits<std::uint64_t>::max())
                    return Result<RenderPresentationOutput>::Failure(MakeError(RenderPresentationErrors::InvalidOutput));
                if (const auto capacity = PollSubmissionAvailability(); capacity.HasError())
                    return Result<RenderPresentationOutput>::Failure(capacity.ErrorValue());
                readyDrawable_ = nil;
                const auto quiescent = QuiesceAcquisition();
                if (quiescent.HasError())
                    return Result<RenderPresentationOutput>::Failure(quiescent.ErrorValue());
                if (!quiescent.Value() || submitted_.Count() != 0 || recordingBudget_->live.load(std::memory_order_acquire) != 0)
                    return Result<RenderPresentationOutput>::Failure(MakeError(RenderPresentationErrors::AcquisitionPending));
                auto configured =
                    ConfigureMetalPresentationOutput(layer_, device_, request, outputCapabilities_, display.Value(), outputRevision_ + 1);
                if (configured.HasError()) {
                    if (configured.ErrorValue().code.Value() == RenderPresentationErrors::OutputUnavailable.code.Value())
                        output_.reset();
                    return configured;
                }
                output_ = configured.Value();
                ++outputRevision_;
                ++drawableGeneration_;
                return configured;
            }

            Result<RenderPresentationOutput> PresentationOutput() const override {
                if (std::this_thread::get_id() != ownerThread_)
                    return Result<RenderPresentationOutput>::Failure(MakeError(MetalBackendErrors::WrongThread));
                if (closing_ || !output_)
                    return Result<RenderPresentationOutput>::Failure(MakeError(RenderPresentationErrors::OutputUnavailable));
                const auto facts = presentationPort_->OutputDisplayFacts();
                if (facts.HasError())
                    return Result<RenderPresentationOutput>::Failure(facts.ErrorValue());
                auto result = *output_;
                result.display = facts.Value();
                const auto admitted = NegotiatePresentationOutput(result.requested, outputCapabilities_, result.display, result.revision);
                return admitted.HasError() ? Result<RenderPresentationOutput>::Failure(admitted.ErrorValue())
                                           : Result<RenderPresentationOutput>::Success(result);
            }

            Result<bool> PollPresentationAvailability(FramebufferExtent extent) override {
                if (const auto access = ValidatePreparationAccess(); access.HasError())
                    return Result<bool>::Failure(access.ErrorValue());
                if (!extent.IsValid())
                    return Result<bool>::Failure(MakeError(MetalBackendErrors::InvalidFrameDescriptor));
                if (!output_)
                    return Result<bool>::Failure(MakeError(RenderPresentationErrors::OutputUnavailable));
                if (const auto capacity = PollSubmissionAvailability(); capacity.HasError() || !capacity.Value())
                    return capacity;
                if (const auto facts = AdmitCurrentDisplay(); facts.HasError())
                    return Result<bool>::Failure(facts.ErrorValue());
                if (extent.width != drawableExtent_.width || extent.height != drawableExtent_.height) {
                    if (const auto resized = TryResize(extent); resized.HasError() || !resized.Value())
                        return resized;
                }
                if (readyDrawable_ != nil)
                    return Result<bool>::Success(true);
                auto result = acquisition_->Poll(drawableGeneration_);
                switch (result.progress) {
                    case MetalDrawablePoll::Pending:
                    case MetalDrawablePoll::Stale:
                        static_cast<void>(acquisition_->Request(drawableGeneration_, extent));
                        return Result<bool>::Success(false);
                    case MetalDrawablePoll::Unavailable:
                        return Result<bool>::Failure(MakeError(RenderPresentationErrors::DrawableUnavailable));
                    case MetalDrawablePoll::WrongThread:
                    case MetalDrawablePoll::Failed:
                        return Result<bool>::Failure(DrawableFailure(result));
                    case MetalDrawablePoll::Ready:
                        break;
                }
                id<MTLTexture> texture = result.drawable.texture;
                const auto format =
                    output_->active.format == RenderTextureFormat::Bgra8Unorm ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatRGBA16Float;
                if (texture == nil || texture.device != device_ || texture.width != extent.width || texture.height != extent.height ||
                    texture.pixelFormat != format || result.extent.width != extent.width || result.extent.height != extent.height)
                    return Result<bool>::Failure(MakeError(RenderPresentationErrors::OutputMismatch));
                readyDrawable_ = result.drawable;
                return Result<bool>::Success(true);
            }

            Result<void> BeginFrame(const FramebufferExtent extent) override {
                if (closing_)
                    return Result<void>::Failure(MakeError(MetalBackendErrors::NotInitialized));
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
                const auto ready = PollPresentationAvailability(extent);
                if (ready.HasError())
                    return Result<void>::Failure(ready.ErrorValue());
                if (!ready.Value())
                    return Result<void>::Failure(MakeError(RenderPresentationErrors::AcquisitionPending));
                resources_.DrainGraphRetirements();

                @autoreleasepool {
                    drawable_ = readyDrawable_;
                    readyDrawable_ = nil;
                    commandBuffer_ = [commandQueue_ commandBuffer];
                    if (commandBuffer_ == nil) {
                        drawable_ = nil;
                        return Result<void>::Failure(MakeError(MetalBackendErrors::CommandSubmissionFailed));
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
                if (const auto encoding = ValidatePrimaryOutput(attachment); encoding.HasError())
                    return encoding;
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
                if (const auto *primary = std::get_if<PrimaryOutputAttachment>(&workload)) {
                    if (const auto encoding = ValidatePrimaryOutput(*primary); encoding.HasError())
                        return encoding;
                }
                if (const auto *paint = std::get_if<RenderGraphUiPaint>(&workload))
                    return resources_.ValidateUiPaint(*paint, resources, (__bridge void *)drawable_.texture);
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
                if (const auto *paint = std::get_if<RenderGraphUiPaint>(&workload))
                    return resources_.ExecuteUiPaint((__bridge void *)commandBuffer_, *paint, resources,
                                                     (__bridge void *)drawable_.texture);
                return resources_.ExecuteGraphWorkload((__bridge void *)commandBuffer_, workload, resources);
            }

            Result<void> RetainGraphResources(IRenderGraphResourceLease &lease) override {
                if (std::this_thread::get_id() != ownerThread_) {
                    return WrongThread();
                }
                if (commandBuffer_ == nil || (activeGraphLease_ != nullptr && activeGraphLease_ != &lease)) {
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
                const auto resized = TryResize(extent);
                if (resized.HasError())
                    return Result<void>::Failure(resized.ErrorValue());
                return resized.Value() ? Result<void>::Success()
                                       : Result<void>::Failure(MakeError(RenderPresentationErrors::AcquisitionPending));
            }

            /** @brief Coalesces resize without allocating an error for ordinary outstanding acquisition. */
            [[nodiscard]] Result<bool> TryResize(const FramebufferExtent extent) {
                if (const auto access = ValidatePreparationAccess(); access.HasError())
                    return Result<bool>::Failure(access.ErrorValue());
                if (!extent.IsValid())
                    return Result<bool>::Failure(MakeError(MetalBackendErrors::InvalidFrameDescriptor));
                if (extent.width == drawableExtent_.width && extent.height == drawableExtent_.height)
                    return Result<bool>::Success(true);
                readyDrawable_ = nil;
                if (const auto quiescent = QuiesceAcquisition(); quiescent.HasError() || !quiescent.Value())
                    return quiescent;
                if (drawableGeneration_ == std::numeric_limits<std::uint64_t>::max())
                    return Result<bool>::Failure(MakeError(RenderPresentationErrors::InvalidOutput));
                @try {
                    layer_.drawableSize = CGSizeMake(static_cast<CGFloat>(extent.width), static_cast<CGFloat>(extent.height));
                    if (layer_.bounds.size.height > 0)
                        layer_.contentsScale = static_cast<CGFloat>(extent.height) / layer_.bounds.size.height;
                    if (layer_.drawableSize.width != extent.width || layer_.drawableSize.height != extent.height) {
                        output_.reset();
                        return Result<bool>::Failure(MakeError(RenderPresentationErrors::OutputMismatch));
                    }
                } @catch (NSException *exception) {
                    (void)exception;
                    output_.reset();
                    return Result<bool>::Failure(MakeError(RenderPresentationErrors::OutputUnavailable));
                }
                drawableExtent_ = extent;
                ++drawableGeneration_;
                return Result<bool>::Success(true);
            }

            /** @copydoc IMetalRuntime::BeginShutdown */
            Result<void> BeginShutdown() override {
                if (ownerThread_ != std::thread::id{} && std::this_thread::get_id() != ownerThread_)
                    return WrongThread();
                if (closing_)
                    return Result<void>::Success();
                closing_ = true;
                feedback_->Close();
                readyDrawable_ = nil;
                if (acquisition_)
                    static_cast<void>(acquisition_->Close());
                AbortFrame();
                for (auto &retired : cancelledRecordings_) {
                    if (retired.recording)
                        retired.recording->Cancel();
                }
                return Result<void>::Success();
            }

            /** @copydoc IMetalRuntime::PollDrain */
            Result<RenderBackendDrainProgress> PollDrain() override {
                if (ownerThread_ != std::thread::id{} && std::this_thread::get_id() != ownerThread_)
                    return Result<RenderBackendDrainProgress>::Failure(MakeError(MetalBackendErrors::WrongThread));
                if (!closing_)
                    return Result<RenderBackendDrainProgress>::Failure(
                        MakeError(MetalBackendErrors::InvalidConfig, "Begin Metal shutdown before polling retirement."));
                if (drained_)
                    return Result<RenderBackendDrainProgress>::Success(RenderBackendDrainProgress::Drained);
                DrainCancelledRecordings();
                if (const auto submitted = submitted_.DrainTerminals(); submitted.HasError() && !drainError_)
                    drainError_ = submitted.ErrorValue();
                const auto uploads = resources_.PollUploadDrain();
                if (uploads.HasError() && !drainError_)
                    drainError_ = uploads.ErrorValue();
                const bool pendingUploads = uploads.HasValue() && !uploads.Value();
                const bool pendingAcquisition = acquisition_ && (!acquisition_->Close() || !acquisition_->IsDrained());
                if (acquisition_ && !pendingAcquisition) {
                    const auto result = acquisition_->Poll(drawableGeneration_);
                    if ((result.progress == MetalDrawablePoll::Failed || result.progress == MetalDrawablePoll::WrongThread) && !drainError_)
                        drainError_ = DrawableFailure(result);
                }
                if (submitted_.Count() == 0 && recordingBudget_->live.load(std::memory_order_acquire) == 0 && !pendingUploads &&
                    !pendingAcquisition) {
                    ReleaseDrainedResources();
                    drained_ = true;
                }
                if (drainError_ && !drainErrorReported_) {
                    drainErrorReported_ = true;
                    return Result<RenderBackendDrainProgress>::Failure(*drainError_);
                }
                return Result<RenderBackendDrainProgress>::Success(drained_ ? RenderBackendDrainProgress::Drained
                                                                            : RenderBackendDrainProgress::Pending);
            }

        private:
            /** @brief Acquires only the admitted native device/queue; the initializer owns every failure cleanup. */
            [[nodiscard]] Result<MetalDeviceCapabilities> AcquireAdmittedDevice(const MetalDeviceAdmissionRequest &request) {
                std::uint64_t discoveryRevision = 0;
                device_ = FindMetalDevice(request.adapter, discoveryRevision);
                if (device_ == nil)
                    return Result<MetalDeviceCapabilities>::Failure(MakeError(MetalBackendErrors::AdapterNotFound));
                // Eight recording leases x sixteen buffers, four owner buffers and sixty-four uploads stay below this cap.
                commandQueue_ = [device_ newCommandQueueWithMaxCommandBufferCount:MetalRecordingBudget::NativeQueueCapacity];
                return AdmitMetalDevice(QueryMetalDeviceFacts(device_, discoveryRevision, commandQueue_ != nil), request);
            }

            /** @brief Attaches the explicitly supplied host surface and probes its real native output combinations. */
            [[nodiscard]] Result<void> PreparePresentation(const MetalPresentationDescriptor &descriptor, bool required) {
                if (const auto surface = presentationPort_->CreateSurface(); surface.HasError())
                    return surface;
                surfaceCreated_ = true;
                layer_ = (__bridge CAMetalLayer *)presentationPort_->Layer();
                if (layer_ == nil)
                    return Result<void>::Failure(MakeError(RenderPresentationErrors::OutputUnavailable));
                if (const auto layer = ConfigurePresentationLayer(descriptor.maxFramesInFlight, descriptor.presentMode); layer.HasError())
                    return layer;
                const auto capabilities = QueryMetalOutputCapabilities(device_);
                if (capabilities.HasError())
                    return Result<void>::Failure(capabilities.ErrorValue());
                outputCapabilities_ = capabilities.Value();
                return AdmitInitialOutput(descriptor, required);
            }

            /** @brief Keeps nonpresenting preparation genuinely output-less; required or explicit output never falls back. */
            [[nodiscard]] Result<void> AdmitInitialOutput(const MetalPresentationDescriptor &descriptor, bool required) {
                const auto display = presentationPort_->OutputDisplayFacts();
                if (display.HasError()) {
                    if (!required && !descriptor.output &&
                        display.ErrorValue().code.Value() == RenderPresentationErrors::OutputUnavailable.code.Value())
                        return Result<void>::Success();
                    return Result<void>::Failure(display.ErrorValue());
                }
                const auto output = ConfigurePresentationOutput(descriptor.output.value_or(RenderPresentationOutputRequest{}));
                return output.HasError() ? Result<void>::Failure(output.ErrorValue()) : Result<void>::Success();
            }

            /** @brief Republishes validated current platform facts without native layer mutation or color-space allocation. */
            [[nodiscard]] Result<void> AdmitCurrentDisplay() {
                const auto facts = presentationPort_->OutputDisplayFacts();
                if (facts.HasError())
                    return Result<void>::Failure(facts.ErrorValue());
                if (facts.Value().revision < output_->display.revision ||
                    (facts.Value().revision == output_->display.revision && facts.Value() != output_->display))
                    return Result<void>::Failure(MakeError(RenderPresentationErrors::InvalidOutput));
                if (facts.Value().revision != output_->display.revision) {
                    const auto admitted =
                        NegotiatePresentationOutput(output_->requested, outputCapabilities_, facts.Value(), output_->revision);
                    if (admitted.HasError())
                        return Result<void>::Failure(admitted.ErrorValue());
                    // Headroom/display evidence can change without changing the exact layer encoding. Publish its value
                    // projection without rebuilding a color space or mutating a layer observed by the acquisition worker.
                    output_->display = facts.Value();
                }
                return Result<void>::Success();
            }

            /** @brief Consumes any old generation before the owner mutates layer properties; never waits. */
            [[nodiscard]] Result<bool> QuiesceAcquisition() {
                if (!acquisition_)
                    return Result<bool>::Success(true);
                const auto result = acquisition_->Poll(drawableGeneration_);
                if (result.progress == MetalDrawablePoll::Failed || result.progress == MetalDrawablePoll::WrongThread)
                    return Result<bool>::Failure(DrawableFailure(result));
                return Result<bool>::Success(acquisition_->Idle());
            }

            /** @brief Preserves owned bounded worker failure context while translating at the native boundary. */
            [[nodiscard]] static Error DrawableFailure(const MetalDrawableResult &result) {
                if (result.progress == MetalDrawablePoll::WrongThread)
                    return MakeError(RenderPresentationErrors::ThreadAffinityViolation);
                return MakeError(RenderPresentationErrors::OutputUnavailable,
                                 result.failure[0] == '\0' ? "Metal drawable acquisition worker could not start or complete."
                                                           : std::string{result.failure.data()});
            }

            /** @brief Checks that graph-final pixels declare exactly the encoding admitted by native presentation. */
            [[nodiscard]] Result<void> ValidatePrimaryOutput(const PrimaryOutputAttachment &attachment) const override {
                if (!output_)
                    return Result<void>::Failure(MakeError(RenderPresentationErrors::OutputUnavailable));
                if (attachment.outputColor && *attachment.outputColor != output_->active)
                    return Result<void>::Failure(MakeError(RenderPresentationErrors::OutputMismatch));
                if (!attachment.outputColor && output_->active != RenderOutputColor{})
                    return Result<void>::Failure(MakeError(RenderPresentationErrors::OutputMismatch));
                return Result<void>::Success();
            }

            /** @brief Releases native parents only after every submitted command and external CPU capsule is terminal. */
            void ReleaseDrainedResources() noexcept {
                acquisition_.reset();
                readyDrawable_ = nil;
                output_.reset();

                resources_.Shutdown();
                submitted_.Clear();
                MetalEditorGraphicsAccess::Clear(*editorGraphicsBridge_);
                if (layer_ != nil) {
                    layer_.device = nil;
                    layer_ = nil;
                }
                lastSubmittedCommandBuffer_ = nil;
                submissionError_.reset();
                commandQueue_ = nil;
                device_ = nil;
                if (surfaceCreated_) {
                    presentationPort_->DestroySurface();
                    surfaceCreated_ = false;
                }
            }

            /** @brief Failed initialization has admitted no GPU work and must close synchronously without a hidden wait. */
            void CloseUnsubmittedInitialization() noexcept {
                const auto begun = BeginShutdown();
                const auto drained = PollDrain();
                if (begun.HasError() || drained.HasError() || drained.Value() != RenderBackendDrainProgress::Drained)
                    std::terminate();
            }

            /**
             * @brief Admits preparation without reading native state from a foreign thread.
             * Initialization and shutdown are externally serialized owner lifecycle operations.
             * Foreign preparation queries may run only while that lifecycle remains stable;
             * callers must join them before owner shutdown. This is not concurrent-shutdown admission.
             */
            [[nodiscard]] Result<void> ValidatePreparationAccess() const {
                if (closing_ || ownerThread_ == std::thread::id{})
                    return Result<void>::Failure(MakeError(MetalBackendErrors::NotInitialized));
                if (std::this_thread::get_id() != ownerThread_)
                    return Result<void>::Failure(MakeError(MetalBackendErrors::WrongThread));
                if (device_ == nil)
                    return Result<void>::Failure(MakeError(MetalBackendErrors::NotInitialized));
                if (commandBuffer_ != nil)
                    return Result<void>::Failure(MakeError(MetalBackendErrors::FrameActive));
                return Result<void>::Success();
            }

            /** @brief Configures the admitted native presentation layer before publishing graphics access. */
            [[nodiscard]] Result<void> ConfigurePresentationLayer(const std::uint32_t maxFramesInFlight, PresentMode mode) {
                @try {
                    layer_.device = device_;
                    layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
                    layer_.framebufferOnly = YES;
                    layer_.opaque = YES;
                    layer_.maximumDrawableCount = maxFramesInFlight;
                    layer_.allowsNextDrawableTimeout = YES;
                    layer_.displaySyncEnabled = mode == PresentMode::Fifo;
                    if (layer_.device != device_ || layer_.maximumDrawableCount != maxFramesInFlight || !layer_.allowsNextDrawableTimeout ||
                        layer_.displaySyncEnabled != (mode == PresentMode::Fifo))
                        return Result<void>::Failure(MakeError(RenderPresentationErrors::OutputMismatch));
                } @catch (NSException *exception) {
                    (void)exception;
                    return Result<void>::Failure(MakeError(RenderPresentationErrors::OutputUnavailable));
                }
                return Result<void>::Success();
            }

            /** @brief Checks native capture state and all command-count bounds before any buffer allocation. */
            [[nodiscard]] Result<void> ValidateParallelCapture(const RenderGraphExecutionRequest &request) const {
                if (!request.submissions.empty())
                    return Result<void>::Failure(
                        MakeError(MetalBackendErrors::UnsupportedGraphExecution, "Metal explicit graph submissions are not implemented."));
                if (commandBuffer_ == nil || drawable_ == nil || renderEncoder_ != nil || activeGraphLease_ != nullptr || activeRecording_)
                    return Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
                if (request.workloads.size() > MetalRecordingBudget::MaximumPasses)
                    return Result<void>::Failure(
                        MakeError(MetalBackendErrors::UnsupportedGraphExecution,
                                  "Metal parallel graphs admit at most sixteen independent command buffers per frame."));
                for (const auto &binding : request.workloads) {
                    if (const auto *primary = std::get_if<PrimaryOutputAttachment>(&binding.workload)) {
                        if (const auto encoding = ValidatePrimaryOutput(*primary); encoding.HasError())
                            return encoding;
                    }
                }
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
            [[nodiscard]] Result<bool> PollSubmissionAvailability() {
                if (std::this_thread::get_id() != ownerThread_) {
                    return Result<bool>::Failure(MakeError(MetalBackendErrors::WrongThread));
                }
                if (submissionError_) {
                    return Result<bool>::Failure(*submissionError_);
                }
                DrainCancelledRecordings();
                if (const auto polled = submitted_.Poll(); polled.HasError()) {
                    submissionError_ = polled.ErrorValue();
                    return Result<bool>::Failure(polled.ErrorValue());
                }
                if (submitted_.Count() >= maxFramesInFlight_)
                    return Result<bool>::Success(false);
                const std::size_t gpuRecordings = submitted_.RecordingCount();
                if (recordingBudget_->live.load(std::memory_order_acquire) > static_cast<std::size_t>(gpuRecordings)) {
                    // A cancelled callback may still retain its CAMetalDrawable parent. Do
                    // not enter nextDrawable and accidentally wait for that CPU lease to drain.
                    return Result<bool>::Success(false);
                }
                return Result<bool>::Success(true);
            }

            /** @brief Keeps direct BeginFrame failure typed while ordinary host readiness uses a nonallocating pending value. */
            [[nodiscard]] Result<void> CheckSubmissionCapacity() {
                const auto available = PollSubmissionAvailability();
                if (available.HasError())
                    return Result<void>::Failure(available.ErrorValue());
                return available.Value() ? Result<void>::Success() : Result<void>::Failure(MakeError(MetalBackendErrors::SubmissionBusy));
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
            __strong id<CAMetalDrawable> readyDrawable_{nil};
            std::unique_ptr<MetalDrawableAcquisition> acquisition_;
            RenderOutputCapabilities outputCapabilities_;
            std::optional<RenderPresentationOutput> output_;
            std::uint64_t outputRevision_{};
            std::uint64_t drawableGeneration_{1};
            FramebufferExtent drawableExtent_;
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
            bool closing_{false};
            bool drained_{false};
            std::optional<Error> drainError_;
            bool drainErrorReported_{false};
        };
    }  // namespace

    Result<std::unique_ptr<IMetalRuntime>> CreateMetalRuntime(IMetalPresentationPort &presentationPort,
                                                              MetalEditorGraphicsBridge &editorGraphicsBridge) {
        return Result<std::unique_ptr<IMetalRuntime>>::Success(std::make_unique<MetalRuntime>(presentationPort, editorGraphicsBridge));
    }
}  // namespace Horo::Render::Detail
