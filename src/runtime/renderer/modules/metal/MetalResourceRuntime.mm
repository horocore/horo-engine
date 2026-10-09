#include "MetalResourceRuntime.h"

#include "MetalCommandCompletion.h"
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
#include <variant>
#include <vector>

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
            return (storage == MetalResourceStorage::Shared ? MTLResourceStorageModeShared : MTLResourceStorageModePrivate) |
                   MTLResourceHazardTrackingModeTracked;
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
        static constexpr std::size_t MaxResidentInstances = 65'535;
        using RetiredResident = std::variant<MetalBufferInstance *, MetalTextureInstance *>;

        Impl() {
            retired.reserve(MaxResidentInstances);
        }

        std::vector<RetiredResident> retired;
        std::size_t retirementCursor{0};

        /** @brief Commands are safe to retire only after native terminal completion or explicit abandonment. */
        [[nodiscard]] static bool Complete(id<MTLCommandBuffer> commands) noexcept {
            return commands == nil || commands.status == MTLCommandBufferStatusCompleted || commands.status == MTLCommandBufferStatusError;
        }

        /** @brief Preserves the previously submitted use so abort cannot clear an older GPU dependency. */
        static void TrackUse(MetalResidentUse &use, id<MTLCommandBuffer> commands) noexcept {
            if (use.last != commands) {
                use.previous = use.last;
                use.last = commands;
            }
        }

        /** @brief Resolves commit/abort ownership for a resource touched by the current command buffer. */
        static void FinishUse(MetalResidentUse &use, id<MTLCommandBuffer> commands, const bool committed) noexcept {
            if (use.last == commands) {
                if (!committed) {
                    use.last = use.previous;
                }
                use.previous = nil;
            }
        }

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
            // Placement-heap defaults are untracked; graph hazards require explicit tracking.
            descriptor.hazardTrackingMode = MTLHazardTrackingModeTracked;
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

        /** @brief Rejects reuse while a retired resource still owns storage on this placement heap. */
        [[nodiscard]] bool PlacementAvailable(const RenderMemoryPlacement &placement) {
            DrainRetired(MaxResidentInstances);
            for (const auto &entry : retired) {
                if (std::visit([&](const auto *instance) {
                    return instance->pool == placement.pool;
                }, entry)) {
                    return false;
                }
            }
            return buffers.size() + textures.size() < MaxResidentInstances;
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
                if (uploaded.HasValue()) {
                    instance->use.last = lastSubmittedUpload;
                    return Result<std::uint64_t>::Success(Identity(instance));
                }
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
        void DestroyResident(std::unordered_set<Instance *> &instances, const std::uint64_t identity, Resource Instance::*resource,
                             const bool force = false) noexcept {
            Instance *instance = Decode<Instance>(identity);
            if (instance == nullptr || !instances.contains(instance) || (instance->use.retired && !force))
                return;
            if (!force && !Complete(instance->use.last)) {
                instance->use.retired = true;
                retired.emplace_back(instance);  // Capacity is fixed before native initialization.
                return;
            }
            const PoolKey key = Key(instance->pool);
            instances.erase(instance);
            [(instance->*resource) makeAliasable];
            delete instance;
            auto heap = heaps.find(key);
            if (heap != heaps.end() && ReleaseMetalHeapResource(heap->second.state))
                heaps.erase(heap);
        }

        /** @brief Polls a finite retirement budget without any GPU wait or completion callback. */
        void DrainRetired(const std::size_t budget) noexcept {
            for (std::size_t examined = 0; examined < budget && !retired.empty(); ++examined) {
                retirementCursor %= retired.size();
                const auto entry = retired[retirementCursor];
                const bool complete = std::visit([](const auto *instance) {
                    return Complete(instance->use.last);
                }, entry);
                if (!complete) {
                    ++retirementCursor;
                    continue;
                }
                std::visit([&](auto *instance) {
                    using Instance = std::remove_pointer_t<decltype(instance)>;
                    if constexpr (std::is_same_v<Instance, MetalBufferInstance>) {
                        DestroyResident(buffers, Identity(instance), &MetalBufferInstance::buffer, true);
                    } else {
                        DestroyResident(textures, Identity(instance), &MetalTextureInstance::texture, true);
                    }
                }, entry);
                retired[retirementCursor] = retired.back();
                retired.pop_back();
            }
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

    namespace {
        /** @brief Applies the complete typed color load/store contract to the native attachment. */
        void ConfigureGraphColorAttachment(MTLRenderPassColorAttachmentDescriptor *attachment, const PrimaryOutputAttachment &operations) {
            switch (operations.loadOperation) {
                case AttachmentLoadOperation::Load:
                    attachment.loadAction = MTLLoadActionLoad;
                    break;
                case AttachmentLoadOperation::Clear:
                    attachment.loadAction = MTLLoadActionClear;
                    break;
                case AttachmentLoadOperation::DontCare:
                    attachment.loadAction = MTLLoadActionDontCare;
                    break;
            }
            attachment.storeAction =
                operations.storeOperation == AttachmentStoreOperation::Store ? MTLStoreActionStore : MTLStoreActionDontCare;
            const auto &clear = operations.clearColor;
            attachment.clearColor = MTLClearColorMake(clear.red, clear.green, clear.blue, clear.alpha);
        }

        /** @brief Finds only an exact graph-local resource binding; never guesses by slot or native address. */
        [[nodiscard]] std::uint64_t GraphInstance(const RenderGraphResourceId id,
                                                  const std::span<const RenderGraphResourceInstance> resources) {
            for (const auto &resource : resources) {
                if (resource.resource == id) {
                    return resource.instance;
                }
            }
            return 0;
        }
    }  // namespace

    /** @copydoc MetalResourceRuntime::ValidateGraphWorkload */
    Result<void> MetalResourceRuntime::ValidateGraphWorkload(const RenderGraphWorkload &workload,
                                                             const std::span<const RenderGraphResourceInstance> resources) const {
        if (const auto *color = std::get_if<RenderGraphColorAttachment>(&workload)) {
            auto *texture = Decode<MetalTextureInstance>(GraphInstance(color->texture, resources));
            if (texture == nullptr || !impl_->textures.contains(texture) || texture->use.retired) {
                return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
            }
            const auto &descriptor = texture->descriptor;
            if (!color->operations.IsValid() || !HasTextureUsage(descriptor.usage, RenderTextureUsage::RenderAttachment) ||
                descriptor.mipCount != 1 || descriptor.layerCount != 1 || descriptor.sampleCount != 1 ||
                !MetalTextureAspectMatches(descriptor.format, RenderTextureAspect::Color)) {
                return Result<void>::Failure(MakeError(MetalBackendErrors::UnsupportedGraphExecution,
                                                       "Metal graph color operations require a single color mip, layer and sample."));
            }
        } else if (const auto *copy = std::get_if<RenderGraphBufferCopy>(&workload)) {
            auto *source = Decode<MetalBufferInstance>(GraphInstance(copy->source, resources));
            auto *destination = Decode<MetalBufferInstance>(GraphInstance(copy->destination, resources));
            if (source == nullptr || destination == nullptr || !impl_->buffers.contains(source) || !impl_->buffers.contains(destination) ||
                source->use.retired || destination->use.retired) {
                return Result<void>::Failure(MakeError(MetalBackendErrors::ResourceIdentityInvalid));
            }
            if (!HasBufferUsage(source->usage, RenderBufferUsage::CopySource) ||
                !HasBufferUsage(destination->usage, RenderBufferUsage::CopyDestination) || source == destination || copy->byteCount == 0 ||
                copy->sourceOffset > source->buffer.length || copy->byteCount > source->buffer.length - copy->sourceOffset ||
                copy->destinationOffset > destination->buffer.length ||
                copy->byteCount > destination->buffer.length - copy->destinationOffset) {
                return Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan,
                                                       "Metal graph copy byte range exceeds its resolved resident buffer."));
            }
        }
        return Result<void>::Success();
    }

    /** @copydoc MetalResourceRuntime::ExecuteGraphWorkload */
    Result<void> MetalResourceRuntime::ExecuteGraphWorkload(void *commandBuffer, const RenderGraphWorkload &workload,
                                                            const std::span<const RenderGraphResourceInstance> resources) {
        if (const auto valid = ValidateGraphWorkload(workload, resources); valid.HasError()) {
            return valid;
        }
        id<MTLCommandBuffer> commands = (__bridge id<MTLCommandBuffer>)commandBuffer;
        if (commands == nil) {
            return Result<void>::Failure(MakeError(MetalBackendErrors::InvalidExecutionPlan));
        }
        if (const auto *color = std::get_if<RenderGraphColorAttachment>(&workload)) {
            auto *texture = Decode<MetalTextureInstance>(GraphInstance(color->texture, resources));
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            auto *attachment = pass.colorAttachments[0];
            attachment.texture = texture->texture;
            ConfigureGraphColorAttachment(attachment, color->operations);
            id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
            if (encoder == nil) {
                return Result<void>::Failure(
                    MakeError(MetalBackendErrors::CommandSubmissionFailed, "Metal graph color encoder creation failed."));
            }
            [encoder endEncoding];
            Impl::TrackUse(texture->use, commands);
        } else if (const auto *copy = std::get_if<RenderGraphBufferCopy>(&workload)) {
            auto *source = Decode<MetalBufferInstance>(GraphInstance(copy->source, resources));
            auto *destination = Decode<MetalBufferInstance>(GraphInstance(copy->destination, resources));
            id<MTLBlitCommandEncoder> encoder = [commands blitCommandEncoder];
            if (encoder == nil) {
                return Result<void>::Failure(
                    MakeError(MetalBackendErrors::CommandSubmissionFailed, "Metal graph copy encoder creation failed."));
            }
            [encoder copyFromBuffer:source->buffer
                       sourceOffset:copy->sourceOffset
                           toBuffer:destination->buffer
                  destinationOffset:copy->destinationOffset
                               size:copy->byteCount];
            [encoder endEncoding];
            Impl::TrackUse(source->use, commands);
            Impl::TrackUse(destination->use, commands);
        }
        return Result<void>::Success();
    }

    /** @copydoc MetalResourceRuntime::FinishGraphCommands */
    void MetalResourceRuntime::FinishGraphCommands(void *commandBuffer, const bool committed) noexcept {
        id<MTLCommandBuffer> commands = (__bridge id<MTLCommandBuffer>)commandBuffer;
        for (auto *buffer : impl_->buffers) {
            Impl::FinishUse(buffer->use, commands, committed);
        }
        for (auto *texture : impl_->textures) {
            Impl::FinishUse(texture->use, commands, committed);
        }
        impl_->DrainRetired(64);
    }

    /** @copydoc MetalResourceRuntime::DrainGraphRetirements */
    void MetalResourceRuntime::DrainGraphRetirements() noexcept {
        impl_->DrainRetired(64);
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
        impl_->commandQueue = nil;
        impl_->device = nil;
    }
}  // namespace Horo::Render::Detail
