#pragma once

#include "MetalRenderBackendErrors.h"
#include "MetalResourceInstances.h"
#include "MetalResourcePolicy.h"
#include "MetalResourceRuntime.h"

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
    [[nodiscard]] inline Error ResourceError(const ErrorCodeDescriptor &descriptor, std::string message) {
        return MakeError(descriptor, std::move(message));
    }

    [[nodiscard]] inline MTLStorageMode NativeStorageMode(const MetalResourceStorage storage) noexcept {
        return storage == MetalResourceStorage::Shared ? MTLStorageModeShared : MTLStorageModePrivate;
    }

    [[nodiscard]] inline MTLResourceOptions NativeResourceOptions(const MetalResourceStorage storage) noexcept {
        return (storage == MetalResourceStorage::Shared ? MTLResourceStorageModeShared : MTLResourceStorageModePrivate) |
               MTLResourceHazardTrackingModeTracked;
    }

    template <typename Instance> [[nodiscard]] Instance *Decode(const std::uint64_t identity) noexcept {
        if (identity == 0 || identity > std::numeric_limits<std::uintptr_t>::max())
            return nullptr;
        return reinterpret_cast<Instance *>(static_cast<std::uintptr_t>(identity));
    }

    template <typename Instance> [[nodiscard]] std::uint64_t Identity(Instance *instance) noexcept {
        return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(instance));
    }

    /** @brief Single render-owner native heap/identity/retirement store shared by materialization and graph operations. */
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
        __strong NSMutableArray<id<MTLCommandBuffer>> *submittedUploads{
            [[NSMutableArray alloc] initWithCapacity:MetalRecordingBudget::MaximumUploads]};
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
            while (submittedUploads.count != 0 && Complete(submittedUploads.firstObject))
                [submittedUploads removeObjectAtIndex:0];
            if (submittedUploads.count >= MetalRecordingBudget::MaximumUploads)
                return Result<StagingBlit>::Failure(
                    MakeError(MetalBackendErrors::SubmissionBusy,
                              "Metal staging uploads occupy their finite command-buffer budget; retry at an owner safe point."));
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
            [submittedUploads addObject:transfer.commands];
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

}  // namespace Horo::Render::Detail
