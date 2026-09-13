#include "MetalResourceRuntime.h"

#include "MetalRenderBackendErrors.h"
#include "MetalResourceFormats.h"
#include "MetalResourceInstances.h"
#include "MetalResourcePolicy.h"

#import <Metal/Metal.h>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Horo::Render::Detail {
    namespace {
        [[nodiscard]] Error ResourceError(const ErrorCodeDescriptor &descriptor, std::string message) {
            return MakeError(descriptor, std::move(message));
        }

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

        [[nodiscard]] MTLStorageMode NativeStorageMode(const MetalResourceStorage storage) noexcept {
            return storage == MetalResourceStorage::Shared ? MTLStorageModeShared : MTLStorageModePrivate;
        }

        [[nodiscard]] MTLResourceOptions NativeResourceOptions(const MetalResourceStorage storage) noexcept {
            return storage == MetalResourceStorage::Shared ? MTLResourceStorageModeShared : MTLResourceStorageModePrivate;
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
            return native;
        }

        template <typename Instance> [[nodiscard]] Instance *Decode(const std::uint64_t identity) noexcept {
            if (identity == 0 || identity > std::numeric_limits<std::uintptr_t>::max())
                return nullptr;
            return reinterpret_cast<Instance *>(static_cast<std::uintptr_t>(identity));
        }

        template <typename Instance> [[nodiscard]] std::uint64_t Identity(Instance *instance) noexcept {
            return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(instance));
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

    struct MetalResourceRuntime::Impl {
        struct PoolKey {
            std::uint64_t renderer{0};
            std::uint64_t pool{0};

            [[nodiscard]] bool operator==(const PoolKey &) const noexcept = default;
        };

        struct PoolKeyHash {
            [[nodiscard]] std::size_t operator()(const PoolKey &key) const noexcept {
                return std::hash<std::uint64_t>{}(key.renderer) ^ (std::hash<std::uint64_t>{}(key.pool) << 1U);
            }
        };

        struct HeapRecord {
            __strong id<MTLHeap> heap{nil};
            MetalHeapState state;
        };

        struct StagingBlit {
            __strong id<MTLBuffer> staging{nil};
            __strong id<MTLCommandBuffer> commands{nil};
            __strong id<MTLBlitCommandEncoder> blit{nil};
        };

        __strong id<MTLDevice> device{nil};
        __strong id<MTLCommandQueue> commandQueue{nil};
        __strong id<MTLCommandBuffer> lastSubmittedUpload{nil}; /**< Final staging submission drained during teardown. */
        std::unordered_map<PoolKey, HeapRecord, PoolKeyHash> heaps;
        std::unordered_set<MetalBufferInstance *> buffers;
        std::unordered_set<MetalMeshInstance *> meshes;
        std::unordered_set<MetalTextureInstance *> textures;
        std::unordered_set<MetalTextureViewInstance *> textureViews;
        std::unordered_set<MetalRenderTargetInstance *> renderTargets;

        [[nodiscard]] PoolKey Key(const RenderMemoryPoolId pool) const noexcept {
            return {.renderer = pool.renderer.value, .pool = pool.value};
        }

        [[nodiscard]] PoolKey Key(const RenderMemoryPlacement &placement) const noexcept {
            return Key(placement.pool);
        }

        [[nodiscard]] Result<HeapRecord *> CreateHeap(const PoolKey key, const RenderMemoryPlacement &placement,
                                                      const MetalResourceStorage storage) {
            MTLHeapDescriptor *descriptor = [[MTLHeapDescriptor alloc] init];
            descriptor.size = placement.backingBytes;
            descriptor.storageMode = NativeStorageMode(storage);
            descriptor.type = MTLHeapTypePlacement;
            id<MTLHeap> heap = [device newHeapWithDescriptor:descriptor];
            if (heap == nil)
                return Result<HeapRecord *>::Failure(
                    ResourceError(MetalBackendErrors::ResourceCreationFailed, "Metal failed to allocate a resource backing heap."));
            auto inserted = heaps.emplace(key, HeapRecord{.heap = heap,
                                                          .state = {.storage = storage,
                                                                    .compatibility = placement.compatibility,
                                                                    .backingBytes = placement.backingBytes}});
            return Result<HeapRecord *>::Success(&inserted.first->second);
        }

        [[nodiscard]] Result<HeapRecord *> AcquireHeap(const RenderMemoryPlacement &placement, const MetalResourceStorage storage) {
            const PoolKey key = Key(placement);
            auto found = heaps.find(key);
            Result<HeapRecord *> acquired =
                found == heaps.end() ? CreateHeap(key, placement, storage) : Result<HeapRecord *>::Success(&found->second);
            if (acquired.HasError())
                return acquired;
            if (ValidateMetalHeapReuse(acquired.Value()->state, placement, storage).HasError()) {
                if (acquired.Value()->state.liveResources == 0)
                    heaps.erase(key);
                return Result<HeapRecord *>::Failure(
                    ResourceError(MetalBackendErrors::InvalidConfig, "Metal resource placement conflicts with its existing backing heap."));
            }
            return acquired;
        }

        void DiscardEmptyHeap(const RenderMemoryPlacement &placement) {
            const auto found = heaps.find(Key(placement));
            if (found != heaps.end() && found->second.state.liveResources == 0)
                heaps.erase(found);
        }

        [[nodiscard]] Result<StagingBlit> BeginStagingBlit(const NSUInteger bytes) const {
            StagingBlit transfer{.staging = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared],
                                 .commands = [commandQueue commandBuffer]};
            transfer.blit = [transfer.commands blitCommandEncoder];
            if (transfer.staging == nil || transfer.commands == nil || transfer.blit == nil)
                return Result<StagingBlit>::Failure(ResourceError(MetalBackendErrors::ResourceCreationFailed,
                                                                  "Metal failed to create a private-resource staging command."));
            return Result<StagingBlit>::Success(std::move(transfer));
        }

        [[nodiscard]] Result<MetalTextureUploadLayout> PlanTextureUpload(const RenderTextureDescriptor &descriptor) const {
            const auto texelBytes = RenderTextureTexelBytes(descriptor.format);
            return PlanMetalTextureUpload(descriptor, texelBytes.value_or(0));
        }

        /** @pre All fallible upload setup has completed; callers return success after this terminal submission step. */
        void CommitStagingBlit(const StagingBlit &transfer) {
            [transfer.blit endEncoding];
            [transfer.commands commit];
            lastSubmittedUpload = transfer.commands;
        }

        [[nodiscard]] Result<void> UploadBuffer(id<MTLBuffer> buffer, const std::span<const std::byte> data,
                                                const MetalResourceStorage storage) {
            if (data.empty())
                return Result<void>::Success();
            if (storage == MetalResourceStorage::Shared) {
                std::memcpy(buffer.contents, data.data(), data.size());
                return Result<void>::Success();
            }
            auto transfer = BeginStagingBlit(data.size());
            if (!transfer.HasValue()) {
                Error error = std::move(transfer).ErrorValue();
                return Result<void>::Failure(std::move(error));
            }
            std::memcpy(transfer.Value().staging.contents, data.data(), data.size());
            [transfer.Value().blit copyFromBuffer:transfer.Value().staging
                                     sourceOffset:0
                                         toBuffer:buffer
                                destinationOffset:0
                                             size:data.size()];
            CommitStagingBlit(transfer.Value());
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> UploadTexture(id<MTLTexture> texture, const RenderTextureDescriptor &descriptor,
                                                 const std::span<const std::byte> data) {
            if (data.empty())
                return Result<void>::Success();
            auto layout = PlanTextureUpload(descriptor);
            if (layout.HasError())
                return Result<void>::Failure(std::move(layout).ErrorValue());
            auto transfer = BeginStagingBlit(static_cast<NSUInteger>(layout.Value().stagingBytes));
            if (transfer.HasError())
                return Result<void>::Failure(std::move(transfer).ErrorValue());
            auto *destination = static_cast<std::byte *>(transfer.Value().staging.contents);
            for (std::size_t layer = 0; layer < descriptor.layerCount; ++layer) {
                for (std::size_t row = 0; row < descriptor.extent.height; ++row) {
                    std::memcpy(destination + layer * layout.Value().paddedImageBytes + row * layout.Value().paddedRowBytes,
                                data.data() + layer * layout.Value().tightImageBytes + row * layout.Value().tightRowBytes,
                                layout.Value().tightRowBytes);
                }
            }
            for (std::size_t layer = 0; layer < descriptor.layerCount; ++layer) {
                [transfer.Value().blit copyFromBuffer:transfer.Value().staging
                                         sourceOffset:static_cast<NSUInteger>(layer * layout.Value().paddedImageBytes)
                                    sourceBytesPerRow:static_cast<NSUInteger>(layout.Value().paddedRowBytes)
                                  sourceBytesPerImage:static_cast<NSUInteger>(layout.Value().paddedImageBytes)
                                           sourceSize:MTLSizeMake(descriptor.extent.width, descriptor.extent.height, 1)
                                            toTexture:texture
                                     destinationSlice:static_cast<NSUInteger>(layer)
                                     destinationLevel:0
                                    destinationOrigin:MTLOriginMake(0, 0, 0)];
            }
            CommitStagingBlit(transfer.Value());
            return Result<void>::Success();
        }

        /** @brief Removes a partially realized placed resource and restores its heap accounting. */
        template <typename Instance, typename Resource>
        void RollBackResident(Instance *instance, HeapRecord &heap, const RenderMemoryPlacement &placement,
                              std::unordered_set<Instance *> &instances, Resource Instance::*resource, const bool retained) noexcept {
            instances.erase(instance);
            [(instance->*resource) makeAliasable];
            delete instance;
            if (retained)
                static_cast<void>(ReleaseMetalHeapResource(heap.state));
            DiscardEmptyHeap(placement);
        }

        /** @brief Publishes one placed resource only after tracking, accounting, and initial upload succeed. */
        template <typename Instance, typename Resource, typename Upload>
        [[nodiscard]] Result<std::uint64_t> FinishResident(Instance *instance, HeapRecord &heap, const RenderMemoryPlacement &placement,
                                                           std::unordered_set<Instance *> &instances, Resource Instance::*resource,
                                                           Upload &&upload, const char *trackingError) {
            try {
                instances.insert(instance);
            } catch (...) {  // NOSONAR(cpp:S2738) Translate backend-private tracking allocation failures.
                auto failure = Result<std::uint64_t>::Failure(ResourceError(MetalBackendErrors::ResourceCreationFailed, trackingError));
                RollBackResident(instance, heap, placement, instances, resource, false);
                return failure;
            }
            if (RetainMetalHeapResource(heap.state).HasError()) {
                RollBackResident(instance, heap, placement, instances, resource, false);
                return Result<std::uint64_t>::Failure(ResourceError(MetalBackendErrors::ResourceCreationFailed, trackingError));
            }
            try {
                Result<void> uploaded = std::forward<Upload>(upload)();
                if (uploaded.HasValue())
                    return Result<std::uint64_t>::Success(Identity(instance));
                RollBackResident(instance, heap, placement, instances, resource, true);
                return Result<std::uint64_t>::Failure(std::move(uploaded).ErrorValue());
            } catch (...) {
                RollBackResident(instance, heap, placement, instances, resource, true);
                return Result<std::uint64_t>::Failure(
                    ResourceError(MetalBackendErrors::ResourceCreationFailed, "Metal initial resource upload threw an exception."));
            }
        }

        /** @brief Publishes one non-placed native identity with typed allocation-failure translation. */
        template <typename Instance>
        [[nodiscard]] Result<std::uint64_t> TrackInstance(Instance *instance, std::unordered_set<Instance *> &instances,
                                                          const char *trackingError) {
            if (instance == nullptr)
                return Result<std::uint64_t>::Failure(ResourceError(MetalBackendErrors::ResourceCreationFailed, trackingError));
            try {
                instances.insert(instance);
            } catch (...) {  // NOSONAR(cpp:S2738) Translate backend-private tracking allocation failures.
                delete instance;
                return Result<std::uint64_t>::Failure(ResourceError(MetalBackendErrors::ResourceCreationFailed, trackingError));
            }
            return Result<std::uint64_t>::Success(Identity(instance));
        }

        template <typename Instance, typename Resource>
        void DestroyResident(std::unordered_set<Instance *> &instances, const std::uint64_t identity,
                             Resource Instance::*resource) noexcept {
            Instance *instance = Decode<Instance>(identity);
            if (instance == nullptr || !instances.contains(instance))
                return;
            const PoolKey key = Key(instance->pool);
            instances.erase(instance);
            [(instance->*resource) makeAliasable];
            delete instance;
            auto heap = heaps.find(key);
            if (heap != heaps.end() && ReleaseMetalHeapResource(heap->second.state))
                heaps.erase(heap);
        }
    };

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
        if (vertex == nullptr || index == nullptr || !impl_->buffers.contains(vertex) || !impl_->buffers.contains(index)) {
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
        if (!descriptor.IsValid() || source == nullptr || !impl_->textures.contains(source)) {
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
        if (impl_->lastSubmittedUpload != nil)
            [impl_->lastSubmittedUpload waitUntilCompleted];
        DestroyAll(impl_->renderTargets);
        DestroyAll(impl_->textureViews);
        DestroyAll(impl_->meshes);
        DestroyAll(impl_->textures);
        DestroyAll(impl_->buffers);
        impl_->heaps.clear();
        impl_->lastSubmittedUpload = nil;
        impl_->commandQueue = nil;
        impl_->device = nil;
    }
}  // namespace Horo::Render::Detail
