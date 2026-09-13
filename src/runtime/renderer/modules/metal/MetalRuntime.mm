#include "Horo/Runtime/Render/RenderAdapterErrors.h"
#include "MetalBackendInternal.h"
#include "MetalRenderBackendErrors.h"
#include "MetalResourceRuntime.h"

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <algorithm>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <string_view>
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

        [[nodiscard]] MetalHostArchitecture HostArchitecture() noexcept {
#if defined(__aarch64__) || defined(__arm64__)
            return MetalHostArchitecture::Arm64;
#elif defined(__x86_64__)
            return MetalHostArchitecture::X86_64;
#else
            return MetalHostArchitecture::Unsupported;
#endif
        }

        [[nodiscard]] RenderAdapterId AdapterId(id<MTLDevice> device) {
            return RenderAdapterId{std::format("metal:{:016x}", static_cast<std::uint64_t>(device.registryID))};
        }

        [[nodiscard]] std::uint64_t DiscoveryRevisionForDevices(NSArray<id<MTLDevice>> *devices) {
            std::vector<std::string> identities;
            identities.reserve(devices.count);
            for (id<MTLDevice> device in devices) {
                identities.push_back(AdapterId(device).Value());
            }
            std::ranges::sort(identities);

            constexpr std::uint64_t offsetBasis = 14'695'981'039'346'656'037ULL;
            constexpr std::uint64_t prime = 1'099'511'628'211ULL;
            std::uint64_t revision = offsetBasis;
            for (const std::string &identity : identities) {
                for (const unsigned char byte : identity) {
                    revision = (revision ^ byte) * prime;
                }
                revision = (revision ^ 0xffU) * prime;
            }
            return revision == 0 ? 1 : revision;
        }

        [[nodiscard]] MetalFormatCapabilities QueryFormats(id<MTLDevice> device) noexcept {
            MetalFormatCapabilities formats;
            const auto sampledAttachment = RenderTextureUsage::Sampled | RenderTextureUsage::RenderAttachment;
            const auto enableUsage = [&formats](const RenderTextureFormat format, const RenderTextureUsage usage) {
                formats.usages[static_cast<std::size_t>(format)] = usage;
            };
            enableUsage(RenderTextureFormat::Rgba8Unorm, sampledAttachment);
            enableUsage(RenderTextureFormat::Bgra8Unorm, sampledAttachment);
            enableUsage(RenderTextureFormat::Depth32Float, RenderTextureUsage::RenderAttachment);
            if (device.depth24Stencil8PixelFormatSupported) {
                enableUsage(RenderTextureFormat::Depth24Stencil8, RenderTextureUsage::RenderAttachment);
            }
            constexpr std::array sampleCounts{1U, 2U, 4U, 8U};
            for (const std::uint32_t sampleCount : sampleCounts) {
                if ([device supportsTextureSampleCount:sampleCount]) {
                    formats.sampleCountMask |= std::uint64_t{1} << sampleCount;
                }
            }
            return formats;
        }

        [[nodiscard]] RenderAdapterProperties AdapterProperties(id<MTLDevice> device) {
            const char *name = device.name.UTF8String;
            std::string displayName = name == nullptr ? "Unnamed Metal device" : std::string{name};
            constexpr std::size_t maximumDisplayNameLength = 256;
            if (displayName.size() > maximumDisplayNameLength) {
                std::size_t truncatedLength = maximumDisplayNameLength;
                while (truncatedLength > 0 && (static_cast<unsigned char>(displayName[truncatedLength]) & 0xc0U) == 0x80U) {
                    --truncatedLength;
                }
                displayName.resize(truncatedLength);
            }
            const RenderAdapterKind kind = device.lowPower ? RenderAdapterKind::Integrated
                                                           : (device.removable ? RenderAdapterKind::Discrete : RenderAdapterKind::Unknown);
            return {
                .id = AdapterId(device),
                .displayName = std::move(displayName),
                .kind = kind,
                .availability = RenderAdapterAvailability::Available,
                .dedicatedVideoMemoryBytes = 0,
                .supportsPresentation = !device.headless,
            };
        }

        [[nodiscard]] MetalDeviceFacts QueryDeviceFacts(id<MTLDevice> device, const std::uint64_t discoveryRevision,
                                                        const bool commandQueueAvailable) {
            const NSOperatingSystemVersion version = NSProcessInfo.processInfo.operatingSystemVersion;
            return {
                .adapter = AdapterProperties(device),
                .discoveryRevision = discoveryRevision,
                .operatingSystemMajor = static_cast<std::uint32_t>(version.majorVersion),
                .operatingSystemMinor = static_cast<std::uint32_t>(version.minorVersion),
                .architecture = HostArchitecture(),
                .supportsApple7 = [device supportsFamily:MTLGPUFamilyApple7],
                .supportsMac2 = [device supportsFamily:MTLGPUFamilyMac2],
                .commandQueueAvailable = commandQueueAvailable,
                .maxBufferLength = static_cast<std::uint64_t>(device.maxBufferLength),
                .maxTextureDimension2D = 16'384,
                .formats = QueryFormats(device),
            };
        }

        [[nodiscard]] bool IsDeviceAvailable(const MetalDeviceFacts &facts) noexcept {
            const bool hostSupported = facts.operatingSystemMajor >= 14;
            const bool familySupported = (facts.architecture == MetalHostArchitecture::Arm64 && facts.supportsApple7) ||
                                         (facts.architecture == MetalHostArchitecture::X86_64 && facts.supportsMac2);
            return hostSupported && familySupported;
        }

        [[nodiscard]] std::vector<RenderAdapterProperties> DiscoverAdapters(NSArray<id<MTLDevice>> *devices, const std::uint64_t revision) {
            std::vector<RenderAdapterProperties> adapters;
            adapters.reserve(devices.count);
            for (id<MTLDevice> device in devices) {
                MetalDeviceFacts facts = QueryDeviceFacts(device, revision, false);
                facts.adapter.availability =
                    IsDeviceAvailable(facts) ? RenderAdapterAvailability::Available : RenderAdapterAvailability::Unavailable;
                adapters.push_back(std::move(facts.adapter));
            }
            std::ranges::sort(adapters, {}, [](const RenderAdapterProperties &adapter) {
                return adapter.id.Value();
            });
            return adapters;
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

        [[nodiscard]] id<MTLDevice> FindDevice(const std::optional<RenderAdapterId> &requested, std::uint64_t &discoveryRevision) {
            NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
            discoveryRevision = DiscoveryRevisionForDevices(devices);
            if (!requested) {
                return MTLCreateSystemDefaultDevice();
            }
            for (id<MTLDevice> candidate in devices) {
                if (AdapterId(candidate) == *requested) {
                    return candidate;
                }
            }
            return nil;
        }

        class MetalAdapterDiscovery final : public IRenderAdapterDiscovery {
        public:
            Result<RenderAdapterSnapshot> Discover(const RenderAdapterDiscoveryRequest &request) override {
                if (const std::optional<Error> invalidState = ValidateState(request)) {
                    return Result<RenderAdapterSnapshot>::Failure(*invalidState);
                }

                NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
                const std::uint64_t revision = DiscoveryRevisionForDevices(devices);
                std::vector<RenderAdapterProperties> adapters = DiscoverAdapters(devices, revision);
                if (adapters.size() > request.maxAdapters) {
                    adapters.resize(request.maxAdapters);
                }
                RenderAdapterSnapshot snapshot{revision, std::move(adapters)};
                if (!snapshot.IsValid()) {
                    return Result<RenderAdapterSnapshot>::Failure(
                        MakeError(MetalBackendErrors::InvalidDeviceFacts,
                                  "Metal discovery returned duplicate or malformed adapter facts."));
                }
                return Result<RenderAdapterSnapshot>::Success(std::move(snapshot));
            }

            void Stop() noexcept override {
                stopped_ = true;
            }

        private:
            [[nodiscard]] std::optional<Error> ValidateState(const RenderAdapterDiscoveryRequest &request) const {
                if (stopped_) {
                    return MakeError(RenderAdapterErrors::DiscoveryStopped);
                }
                if (!request.IsValid()) {
                    return MakeError(RenderAdapterErrors::InvalidDiscoveryRequest);
                }
                return std::nullopt;
            }

            bool stopped_{false};
        };

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
                device_ = FindDevice(request.adapter, discoveryRevision);
                if (device_ == nil) {
                    return Result<MetalDeviceCapabilities>::Failure(
                        MakeError(MetalBackendErrors::AdapterNotFound, "The requested Metal adapter is unavailable."));
                }
                commandQueue_ = [device_ newCommandQueue];
                const Result<MetalDeviceCapabilities> admitted =
                    AdmitMetalDevice(QueryDeviceFacts(device_, discoveryRevision, commandQueue_ != nil), request);
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
            [[nodiscard]] MetalResourceRuntime &Resources() noexcept {
                return resources_;
            }

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

namespace Horo::Render {
    /** @copydoc CreateMetalAdapterDiscovery */
    Result<std::unique_ptr<IRenderAdapterDiscovery>> CreateMetalAdapterDiscovery() {
        return Result<std::unique_ptr<IRenderAdapterDiscovery>>::Success(std::make_unique<Detail::MetalAdapterDiscovery>());
    }
}  // namespace Horo::Render
