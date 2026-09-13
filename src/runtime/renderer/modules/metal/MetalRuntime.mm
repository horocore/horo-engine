#include "MetalBackendInternal.h"
#include "MetalResourceRuntime.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace Horo::Render::Detail {
    namespace {
        [[nodiscard]] Error MakeMetalRuntimeError(const char *code, std::string message) {
            return Error{.code = ErrorCode{code},
                         .domain = ErrorDomainId{"horo.render.metal"},
                         .severity = ErrorSeverity::Error,
                         .message = std::move(message)};
        }

        [[nodiscard]] MTLLoadAction ToMetalLoadAction(const AttachmentLoadOperation operation) {
            switch (operation) {
                case AttachmentLoadOperation::Load:
                    return MTLLoadActionLoad;
                case AttachmentLoadOperation::Clear:
                    return MTLLoadActionClear;
                case AttachmentLoadOperation::DontCare:
                    return MTLLoadActionDontCare;
            }
            return MTLLoadActionDontCare;
        }

        [[nodiscard]] MTLStoreAction ToMetalStoreAction(const AttachmentStoreOperation operation) {
            switch (operation) {
                case AttachmentStoreOperation::Store:
                    return MTLStoreActionStore;
                case AttachmentStoreOperation::DontCare:
                    return MTLStoreActionDontCare;
            }
            return MTLStoreActionDontCare;
        }

        class MetalRuntime final : public IMetalRuntime {
        public:
            MetalRuntime(IMetalPresentationPort &presentationPort, MetalEditorGraphicsBridge &editorGraphicsBridge) noexcept
                : presentationPort_(&presentationPort), editorGraphicsBridge_(&editorGraphicsBridge) {}

            ~MetalRuntime() override {
                Shutdown();
            }

            Result<void> Initialize(const MetalPresentationDescriptor &descriptor) override {
                if (device_ != nil || surfaceCreated_) {
                    return Result<void>::Failure(MakeMetalRuntimeError("render.metal.presentation_exists",
                                                                       "Metal runtime presentation resources are already retained."));
                }
                if (descriptor.enableValidation) {
                    const char *debugLayer = std::getenv("MTL_DEBUG_LAYER");
                    if (debugLayer == nullptr || std::string_view{debugLayer} != "1") {
                        return Result<void>::Failure(
                            MakeMetalRuntimeError("render.metal.validation_unavailable",
                                                  "Metal validation must be enabled through MTL_DEBUG_LAYER=1 before device creation."));
                    }
                }

                const Result<void> surface = presentationPort_->CreateSurface();
                if (surface.HasError()) {
                    return surface;
                }
                surfaceCreated_ = true;
                layer_ = (__bridge CAMetalLayer *)presentationPort_->Layer();
                if (layer_ == nil) {
                    Shutdown();
                    return Result<void>::Failure(MakeMetalRuntimeError("render.metal.layer_unavailable",
                                                                       "The platform presentation port did not expose a CAMetalLayer."));
                }

                device_ = MTLCreateSystemDefaultDevice();
                if (device_ == nil) {
                    Shutdown();
                    return Result<void>::Failure(
                        MakeMetalRuntimeError("render.metal.device_unavailable", "The host did not provide a compatible Metal device."));
                }
                commandQueue_ = [device_ newCommandQueue];
                if (commandQueue_ == nil) {
                    Shutdown();
                    return Result<void>::Failure(
                        MakeMetalRuntimeError("render.metal.command_queue_creation_failed", "Failed to create the Metal command queue."));
                }

                layer_.device = device_;
                layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
                layer_.framebufferOnly = YES;
                layer_.opaque = YES;
                layer_.maximumDrawableCount = descriptor.maxFramesInFlight;
                layer_.displaySyncEnabled = descriptor.presentMode == PresentMode::Fifo;
                MetalEditorGraphicsAccess::PublishPersistent(*editorGraphicsBridge_, (__bridge void *)device_,
                                                             (__bridge void *)commandQueue_, this, &WaitUntilIdleThunk);
                resources_.Initialize((__bridge void *)device_);
                return Result<void>::Success();
            }

            Result<RenderMemoryCostPlan> QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const override {
                return resources_.QueryBufferMemoryCost(descriptor);
            }

            Result<RenderMemoryCostPlan> QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const override {
                return resources_.QueryTextureMemoryCost(descriptor);
            }

            Result<std::uint64_t> CreateBuffer(const RenderBufferDescriptor &descriptor, const std::span<const std::byte> initialData,
                                               const RenderMemoryPlacement &placement) override {
                return resources_.CreateBuffer(descriptor, initialData, placement);
            }

            Result<std::uint64_t> CreateMesh(const RenderMeshDescriptor &descriptor, const std::uint64_t vertexBuffer,
                                             const std::uint64_t indexBuffer) override {
                return resources_.CreateMesh(descriptor, vertexBuffer, indexBuffer);
            }

            Result<std::uint64_t> CreateTexture(const RenderTextureDescriptor &descriptor, const std::span<const std::byte> initialData,
                                                const RenderMemoryPlacement &placement) override {
                return resources_.CreateTexture(descriptor, initialData, placement);
            }

            Result<std::uint64_t> CreateTextureView(const RenderTextureViewDescriptor &descriptor, const std::uint64_t texture) override {
                return resources_.CreateTextureView(descriptor, texture);
            }

            Result<std::uint64_t> CreateRenderTarget(const RenderTargetDescriptor &descriptor, const std::uint64_t colorAttachment,
                                                     const std::uint64_t depthAttachment) override {
                return resources_.CreateRenderTarget(descriptor, colorAttachment, depthAttachment);
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
                if (commandBuffer_ == nil || renderPassDescriptor_ == nil) {
                    return Result<void>::Failure(
                        MakeMetalRuntimeError("render.metal.no_active_frame", "No Metal presentation frame is active."));
                }
                EndPrimaryEncoder();

                MTLRenderPassColorAttachmentDescriptor *color = renderPassDescriptor_.colorAttachments[0];
                color.loadAction = ToMetalLoadAction(attachment.loadOperation);
                color.storeAction = ToMetalStoreAction(attachment.storeOperation);
                color.clearColor = MTLClearColorMake(attachment.clearColor.red, attachment.clearColor.green, attachment.clearColor.blue,
                                                     attachment.clearColor.alpha);
                renderEncoder_ = [commandBuffer_ renderCommandEncoderWithDescriptor:renderPassDescriptor_];
                if (renderEncoder_ == nil) {
                    return Result<void>::Failure(MakeMetalRuntimeError("render.metal.encoder_creation_failed",
                                                                       "Failed to create the primary Metal render encoder."));
                }
                [renderEncoder_ pushDebugGroup:@"Horo Primary Output"];
                PublishFrame();
                return Result<void>::Success();
            }

            Result<void> Present() override {
                if (commandBuffer_ == nil || drawable_ == nil) {
                    return Result<void>::Failure(
                        MakeMetalRuntimeError("render.metal.no_active_frame", "No Metal presentation frame is active."));
                }
                EndPrimaryEncoder();
                [commandBuffer_ presentDrawable:drawable_];
                lastSubmittedCommandBuffer_ = commandBuffer_;
                [commandBuffer_ commit];
                ClearActiveFrame();
                return Result<void>::Success();
            }

            void AbortFrame() noexcept override {
                EndPrimaryEncoder();
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
                layer_.drawableSize = CGSizeMake(static_cast<CGFloat>(extent.width), static_cast<CGFloat>(extent.height));
                return Result<void>::Success();
            }

            void Shutdown() noexcept override {
                AbortFrame();
                WaitUntilIdle();
                resources_.Shutdown();
                MetalEditorGraphicsAccess::Clear(*editorGraphicsBridge_);
                if (layer_ != nil) {
                    layer_.device = nil;
                    layer_ = nil;
                }
                lastSubmittedCommandBuffer_ = nil;
                commandQueue_ = nil;
                device_ = nil;
                if (surfaceCreated_) {
                    presentationPort_->DestroySurface();
                    surfaceCreated_ = false;
                }
            }

        private:
            static void WaitUntilIdleThunk(void *context) noexcept {
                static_cast<MetalRuntime *>(context)->WaitUntilIdle();
            }

            void WaitUntilIdle() noexcept {
                if (lastSubmittedCommandBuffer_ != nil) {
                    [lastSubmittedCommandBuffer_ waitUntilCompleted];
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
            MetalResourceRuntime resources_;
            bool surfaceCreated_{false};
        };
    }  // namespace

    Result<std::unique_ptr<IMetalRuntime>> CreateMetalRuntime(IMetalPresentationPort &presentationPort,
                                                              MetalEditorGraphicsBridge &editorGraphicsBridge) {
        return Result<std::unique_ptr<IMetalRuntime>>::Success(std::make_unique<MetalRuntime>(presentationPort, editorGraphicsBridge));
    }
}  // namespace Horo::Render::Detail
