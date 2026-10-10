#include "MetalResourceRuntime.h"

#include "MetalCommandCompletion.h"
#include "MetalRenderBackendErrors.h"
#include "MetalResourceFormats.h"
#include "MetalResourceInstances.h"
#include "MetalResourcePolicy.h"
#include "MetalResourceRuntimeInternal.h"

#import <Metal/Metal.h>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace Horo::Render::Detail {
    namespace {
        [[nodiscard]] MTLPixelFormat PixelFormat(const RenderTextureFormat format) noexcept {
            return static_cast<MTLPixelFormat>(MetalPixelFormatValue(format));
        }

        [[nodiscard]] MTLTextureUsage TextureUsage(const RenderTextureUsage usage) noexcept {
            MTLTextureUsage nativeUsage = MTLTextureUsageUnknown;
            if (HasTextureUsage(usage, RenderTextureUsage::Sampled))
                nativeUsage |= MTLTextureUsageShaderRead;
            if (HasTextureUsage(usage, RenderTextureUsage::RenderAttachment))
                nativeUsage |= MTLTextureUsageRenderTarget;
            if (HasTextureUsage(usage, RenderTextureUsage::Storage))
                nativeUsage |= MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
            return nativeUsage;
        }

        [[nodiscard]] MTLTextureDescriptor *NativeTextureDescriptor(const RenderTextureDescriptor &descriptor) noexcept {
            MTLTextureDescriptor *native = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:PixelFormat(descriptor.format)
                                                                                              width:descriptor.extent.width
                                                                                             height:descriptor.extent.height
                                                                                          mipmapped:descriptor.mipCount > 1];
            native.textureType = descriptor.layerCount > 1 ? MTLTextureType2DArray : MTLTextureType2D;
            if (descriptor.sampleCount > 1)
                native.textureType = descriptor.layerCount > 1 ? MTLTextureType2DMultisampleArray : MTLTextureType2DMultisample;
            native.mipmapLevelCount = descriptor.mipCount;
            native.arrayLength = descriptor.layerCount;
            native.sampleCount = descriptor.sampleCount;
            native.usage = TextureUsage(descriptor.usage);
            native.storageMode = NativeStorageMode(MetalTextureStorage(descriptor));
            native.hazardTrackingMode = MTLHazardTrackingModeTracked;
            return native;
        }

        template <typename Instance> void DestroyTracked(std::unordered_set<Instance *> &instances, const std::uint64_t identity) noexcept {
            Instance *instance = Decode<Instance>(identity);
            if (instance != nullptr && instances.erase(instance) > 0)
                delete instance;
        }

        template <typename Instance> void DestroyAll(std::unordered_set<Instance *> &instances) noexcept {
            for (Instance *instance : instances)
                delete instance;
            instances.clear();
        }

        /** @brief Reports whether Metal can bind a requested view over its source texture. */
        [[nodiscard]] bool SupportsTextureView(const RenderTextureDescriptor &source, const RenderTextureViewDescriptor &view) {
            const bool supportedDimension =
                view.dimension == RenderTextureViewDimension::TwoD || view.dimension == RenderTextureViewDimension::TwoDArray;
            return supportedDimension && ValidateRenderTextureViewCompatibility(source, view).HasValue() &&
                   MetalTextureAspectMatches(view.format, view.aspect);
        }

        enum class AttachmentKind : std::uint8_t {
            Color,
            Depth,
        };

        [[nodiscard]] Result<MetalTextureViewInstance *> ResolveAttachment(const std::unordered_set<MetalTextureViewInstance *> &instances,
                                                                           const std::uint64_t identity, const AttachmentKind kind) {
            if (identity == 0)
                return Result<MetalTextureViewInstance *>::Success(nullptr);
            MetalTextureViewInstance *instance = Decode<MetalTextureViewInstance>(identity);
            if (instance == nullptr || !instances.contains(instance)) {
                return Result<MetalTextureViewInstance *>::Failure(
                    ResourceError(MetalBackendErrors::ResourceIdentityInvalid, "Metal render target attachment is invalid."));
            }
            const bool aspectMatches = kind == AttachmentKind::Color ? instance->aspect == RenderTextureAspect::Color
                                                                     : instance->aspect != RenderTextureAspect::Color;
            if (!aspectMatches || !HasTextureUsage(instance->usage, RenderTextureUsage::RenderAttachment))
                return Result<MetalTextureViewInstance *>::Failure(
                    ResourceError(MetalBackendErrors::ResourceIdentityInvalid,
                                  "Metal render target attachment lacks a compatible render-attachment binding."));
            return Result<MetalTextureViewInstance *>::Success(instance);
        }

        [[nodiscard]] bool ExtentMatches(const MetalTextureViewInstance *attachment, const FramebufferExtent extent) noexcept {
            return attachment == nullptr || (attachment->texture.width == extent.width && attachment->texture.height == extent.height);
        }

        /** @brief Validates one Metal buffer creation request against its queried plan. */
        [[nodiscard]] bool ValidBufferRequest(const bool deviceAvailable, const RenderBufferDescriptor &descriptor,
                                              const std::span<const std::byte> initialData, const RenderMemoryPlacement &placement,
                                              const Result<RenderMemoryCostPlan> &cost) {
            return deviceAvailable && descriptor.IsValid() && (initialData.empty() || initialData.size() == descriptor.byteSize) &&
                   cost.HasValue() && ValidateMetalResourcePlacement(cost.Value(), placement).HasValue();
        }

        /** @brief Validates one Metal texture creation request against its queried plan. */
        [[nodiscard]] bool ValidTextureRequest(const bool deviceAvailable, const RenderTextureDescriptor &descriptor,
                                               const std::span<const std::byte> initialData, const RenderMemoryPlacement &placement,
                                               const Result<RenderMemoryCostPlan> &cost) {
            const auto payload = RenderTextureBaseLevelByteSize(descriptor);
            const bool initialDataValid = payload.has_value() && (initialData.empty() || initialData.size() == *payload) &&
                                          (initialData.empty() || descriptor.sampleCount == 1);
            if (!deviceAvailable || !descriptor.IsValid() || !initialDataValid || cost.HasError())
                return false;
            return ValidateMetalResourcePlacement(cost.Value(), placement).HasValue();
        }
    }  // namespace

    MetalResourceRuntime::MetalResourceRuntime() : impl_(std::make_unique<Impl>()) {}

    MetalResourceRuntime::~MetalResourceRuntime() {
        Shutdown();
    }

    void MetalResourceRuntime::Initialize(void *device, void *commandQueue) noexcept {
        impl_->device = (__bridge id<MTLDevice>)device;
        impl_->commandQueue = (__bridge id<MTLCommandQueue>)commandQueue;
    }

    Result<RenderMemoryCostPlan> MetalResourceRuntime::QueryBufferMemoryCost(const RenderBufferDescriptor &descriptor) const {
        if (impl_->device == nil || !descriptor.IsValid())
            return Result<RenderMemoryCostPlan>::Failure(
                ResourceError(MetalBackendErrors::InvalidConfig, "Metal buffer memory requirement request is invalid."));
        const MTLSizeAndAlign native =
            [impl_->device heapBufferSizeAndAlignWithLength:descriptor.byteSize
                                                    options:NativeResourceOptions(MetalBufferStorage(descriptor))];
        return PlanMetalBufferMemory(descriptor, {.size = native.size, .alignment = native.align});
    }

    Result<RenderMemoryCostPlan> MetalResourceRuntime::QueryTextureMemoryCost(const RenderTextureDescriptor &descriptor) const {
        const auto payload = RenderTextureBaseLevelByteSize(descriptor);
        if (impl_->device == nil || !payload.has_value() || descriptor.dimension != RenderTextureDimension::TwoD ||
            PixelFormat(descriptor.format) == MTLPixelFormatInvalid)
            return Result<RenderMemoryCostPlan>::Failure(
                ResourceError(MetalBackendErrors::InvalidConfig, "Metal texture memory requirement request is invalid."));
        MTLTextureDescriptor *nativeDescriptor = NativeTextureDescriptor(descriptor);
        const MTLSizeAndAlign native = [impl_->device heapTextureSizeAndAlignWithDescriptor:nativeDescriptor];
        return PlanMetalTextureMemory(descriptor, {.size = native.size, .alignment = native.align});
    }

    Result<std::uint64_t> MetalResourceRuntime::CreateBuffer(const RenderBufferDescriptor &descriptor,
                                                             const std::span<const std::byte> initialData,
                                                             const RenderMemoryPlacement &placement) {
        const auto cost = QueryBufferMemoryCost(descriptor);
        if (!ValidBufferRequest(impl_->device != nil, descriptor, initialData, placement, cost))
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::InvalidConfig, "Metal buffer creation request is invalid."));
        if (!impl_->PlacementAvailable(placement)) {
            return Result<std::uint64_t>::Failure(
                MakeError(MetalBackendErrors::ResourceCreationFailed,
                          "Metal placement remains owned by an outstanding retired resource command; retry after completion."));
        }
        const MetalResourceStorage storage = MetalBufferStorage(descriptor);
        auto heap = impl_->AcquireHeap(placement, storage);
        if (heap.HasError())
            return Result<std::uint64_t>::Failure(std::move(heap).ErrorValue());
        id<MTLBuffer> buffer = [heap.Value()->heap newBufferWithLength:descriptor.byteSize
                                                               options:NativeResourceOptions(storage)
                                                                offset:placement.offsetBytes];
        if (buffer == nil) {
            impl_->DiscardEmptyHeap(placement);
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::ResourceCreationFailed, "Metal failed to allocate a resident buffer."));
        }
        auto *instance = new (std::nothrow) MetalBufferInstance{.buffer = buffer, .usage = descriptor.usage, .pool = placement.pool};
        if (instance == nullptr) {
            [buffer makeAliasable];
            impl_->DiscardEmptyHeap(placement);
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::ResourceCreationFailed, "Metal buffer identity allocation failed."));
        }
        return impl_->FinishResident(instance, *heap.Value(), placement, impl_->buffers, &MetalBufferInstance::buffer, [&] {
            return impl_->UploadBuffer(buffer, initialData, storage);
        }, "Metal buffer heap tracking capacity is exhausted.");
    }

    Result<std::uint64_t> MetalResourceRuntime::CreateMesh(const RenderMeshDescriptor &descriptor, const std::uint64_t vertexBuffer,
                                                           const std::uint64_t indexBuffer) {
        MetalBufferInstance *vertex = Decode<MetalBufferInstance>(vertexBuffer);
        MetalBufferInstance *index = Decode<MetalBufferInstance>(indexBuffer);
        if (vertex == nullptr || index == nullptr || !impl_->buffers.contains(vertex) || !impl_->buffers.contains(index) ||
            vertex->use.retired || index->use.retired) {
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::ResourceIdentityInvalid, "Metal mesh references unknown buffer instances."));
        }
        const Result<void> validBindings = ValidateMetalMeshBindings(descriptor, vertex->usage, index->usage);
        if (validBindings.HasError())
            return Result<std::uint64_t>::Failure(validBindings.ErrorValue());
        auto *instance = new (std::nothrow) MetalMeshInstance{.vertexBuffer = vertex->buffer, .indexBuffer = index->buffer};
        return impl_->TrackInstance(instance, impl_->meshes, "Metal mesh identity allocation failed.");
    }

    Result<std::uint64_t> MetalResourceRuntime::CreateTexture(const RenderTextureDescriptor &descriptor,
                                                              const std::span<const std::byte> initialData,
                                                              const RenderMemoryPlacement &placement) {
        const auto cost = QueryTextureMemoryCost(descriptor);
        if (!ValidTextureRequest(impl_->device != nil, descriptor, initialData, placement, cost))
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::InvalidConfig, "Metal texture creation request is invalid."));
        if (!impl_->PlacementAvailable(placement)) {
            return Result<std::uint64_t>::Failure(
                MakeError(MetalBackendErrors::ResourceCreationFailed,
                          "Metal placement remains owned by an outstanding retired resource command; retry after completion."));
        }
        MTLTextureDescriptor *native = NativeTextureDescriptor(descriptor);
        auto heap = impl_->AcquireHeap(placement, MetalTextureStorage(descriptor));
        if (heap.HasError())
            return Result<std::uint64_t>::Failure(std::move(heap).ErrorValue());
        id<MTLTexture> texture = [heap.Value()->heap newTextureWithDescriptor:native offset:placement.offsetBytes];
        if (texture == nil) {
            impl_->DiscardEmptyHeap(placement);
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::ResourceCreationFailed, "Metal failed to allocate a resident texture."));
        }
        auto *instance = new (std::nothrow) MetalTextureInstance{.texture = texture, .descriptor = descriptor, .pool = placement.pool};
        if (instance == nullptr) {
            [texture makeAliasable];
            impl_->DiscardEmptyHeap(placement);
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::ResourceCreationFailed, "Metal texture identity allocation failed."));
        }
        return impl_->FinishResident(instance, *heap.Value(), placement, impl_->textures, &MetalTextureInstance::texture, [&] {
            return impl_->UploadTexture(texture, descriptor, initialData);
        }, "Metal texture heap tracking capacity is exhausted.");
    }

    Result<std::uint64_t> MetalResourceRuntime::CreateTextureView(const RenderTextureViewDescriptor &descriptor,
                                                                  const std::uint64_t texture) {
        MetalTextureInstance *source = Decode<MetalTextureInstance>(texture);
        if (!descriptor.IsValid() || source == nullptr || !impl_->textures.contains(source) || source->use.retired) {
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::ResourceIdentityInvalid, "Metal texture view source identity is invalid."));
        }
        if (!SupportsTextureView(source->descriptor, descriptor))
            return Result<std::uint64_t>::Failure(ResourceError(MetalBackendErrors::UnsupportedResourceOperation,
                                                                "Metal texture view binding is incompatible with its source."));
        MTLTextureType viewType = descriptor.dimension == RenderTextureViewDimension::TwoDArray ? MTLTextureType2DArray : MTLTextureType2D;
        if (source->descriptor.sampleCount > 1)
            viewType = descriptor.dimension == RenderTextureViewDimension::TwoDArray ? MTLTextureType2DMultisampleArray
                                                                                     : MTLTextureType2DMultisample;
        id<MTLTexture> view = [source->texture newTextureViewWithPixelFormat:PixelFormat(descriptor.format)
                                                                 textureType:viewType
                                                                      levels:NSMakeRange(descriptor.baseMip, descriptor.mipCount)
                                                                      slices:NSMakeRange(descriptor.baseLayer, descriptor.layerCount)];
        if (view == nil)
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::ResourceCreationFailed, "Metal failed to create a resident texture view."));
        auto *instance = new (std::nothrow) MetalTextureViewInstance{.texture = view,
                                                                     .format = descriptor.format,
                                                                     .aspect = descriptor.aspect,
                                                                     .usage = source->descriptor.usage};
        return impl_->TrackInstance(instance, impl_->textureViews, "Metal texture-view identity allocation failed.");
    }

    Result<std::uint64_t> MetalResourceRuntime::CreateRenderTarget(const RenderTargetDescriptor &descriptor,
                                                                   const std::uint64_t colorAttachment,
                                                                   const std::uint64_t depthAttachment) {
        if (!descriptor.IsValid())
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::InvalidConfig, "Metal render target descriptor is invalid."));
        const auto color = ResolveAttachment(impl_->textureViews, colorAttachment, AttachmentKind::Color);
        if (color.HasError())
            return Result<std::uint64_t>::Failure(color.ErrorValue());
        const auto depth = ResolveAttachment(impl_->textureViews, depthAttachment, AttachmentKind::Depth);
        if (depth.HasError())
            return Result<std::uint64_t>::Failure(depth.ErrorValue());
        if (!ExtentMatches(color.Value(), descriptor.extent) || !ExtentMatches(depth.Value(), descriptor.extent))
            return Result<std::uint64_t>::Failure(
                ResourceError(MetalBackendErrors::InvalidExtent, "Metal render target attachment extent does not match its descriptor."));
        auto *instance = new (std::nothrow) MetalRenderTargetInstance{
            .colorTexture = color.Value() == nullptr ? nil : color.Value()->texture,
            .depthTexture = depth.Value() == nullptr ? nil : depth.Value()->texture,
        };
        return impl_->TrackInstance(instance, impl_->renderTargets, "Metal render-target identity allocation failed.");
    }

    void MetalResourceRuntime::DestroyBuffer(const std::uint64_t backendInstance) noexcept {
        impl_->DestroyResident(impl_->buffers, backendInstance, &MetalBufferInstance::buffer);
    }

    void MetalResourceRuntime::DestroyMesh(const std::uint64_t backendInstance) noexcept {
        DestroyTracked(impl_->meshes, backendInstance);
    }

    void MetalResourceRuntime::DestroyTexture(const std::uint64_t backendInstance) noexcept {
        impl_->DestroyResident(impl_->textures, backendInstance, &MetalTextureInstance::texture);
    }

    void MetalResourceRuntime::DestroyTextureView(const std::uint64_t backendInstance) noexcept {
        DestroyTracked(impl_->textureViews, backendInstance);
    }

    void MetalResourceRuntime::DestroyRenderTarget(const std::uint64_t backendInstance) noexcept {
        DestroyTracked(impl_->renderTargets, backendInstance);
    }

    void MetalResourceRuntime::Shutdown() noexcept {
        // Timeout closes this native ownership domain; it does not establish reusable completion evidence.
        static_cast<void>(ObserveMetalCommandForTeardown(impl_->lastSubmittedUpload));
        DestroyAll(impl_->renderTargets);
        DestroyAll(impl_->textureViews);
        DestroyAll(impl_->meshes);
        DestroyAll(impl_->textures);
        DestroyAll(impl_->buffers);
        impl_->heaps.clear();
        impl_->retired.clear();
        impl_->retirementCursor = 0;
        impl_->lastSubmittedUpload = nil;
        [impl_->submittedUploads removeAllObjects];
        impl_->commandQueue = nil;
        impl_->device = nil;
    }
}  // namespace Horo::Render::Detail
