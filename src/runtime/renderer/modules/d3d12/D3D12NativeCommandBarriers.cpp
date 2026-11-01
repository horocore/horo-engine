#include "D3D12NativeCommandBarriers.h"

#if defined(_WIN32) && defined(_M_X64)
namespace Horo::Render::D3D12Detail {
    namespace {
        using Operation = RenderGraphSynchronizationOperation;

        /** @brief Sampled layout is shared, but compute queues cannot name pixel-shader access. */
        D3D12_RESOURCE_STATES LegacySampledState(const D3D12QueueKind queue) {
            return queue == D3D12QueueKind::Compute
                       ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                       : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }

        bool CopyOperation(const Operation operation) noexcept {
            return operation == Operation::None || operation == Operation::CopySource || operation == Operation::CopyDestination;
        }

        bool GraphicsOnlyOperation(const Operation operation) noexcept {
            return operation == Operation::ColorAttachment || operation == Operation::DepthStencilAttachment ||
                   operation == Operation::GraphicsInputBuffer;
        }

        /** @brief Legacy state policy retains compute queue restrictions and exact authored operation. */
        D3D12_RESOURCE_STATES LegacyState(const RenderGraphLogicalState &state, const D3D12QueueKind queue) {
            switch (state.operation) {
                case Operation::None:
                    return D3D12_RESOURCE_STATE_COMMON;
                case Operation::Sampled:
                    return LegacySampledState(queue);
                case Operation::Storage:
                    return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                case Operation::ColorAttachment:
                    return D3D12_RESOURCE_STATE_RENDER_TARGET;
                case Operation::DepthStencilAttachment:
                    return state.access == RenderGraphSynchronizationAccess::Read ? D3D12_RESOURCE_STATE_DEPTH_READ
                                                                                  : D3D12_RESOURCE_STATE_DEPTH_WRITE;
                case Operation::CopySource:
                    return D3D12_RESOURCE_STATE_COPY_SOURCE;
                case Operation::CopyDestination:
                    return D3D12_RESOURCE_STATE_COPY_DEST;
                case Operation::GraphicsInputBuffer:
                    return D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER;
            }
            HORO_INVARIANT(false);
            return D3D12_RESOURCE_STATE_COMMON;
        }

        /** @brief Enhanced memory-access scopes; None denotes no access across the fixed COMMON boundary. */
        D3D12_BARRIER_ACCESS EnhancedAccess(const RenderGraphLogicalState &state) {
            switch (state.operation) {
                case Operation::None:
                    return D3D12_BARRIER_ACCESS_NO_ACCESS;
                case Operation::Sampled:
                    return D3D12_BARRIER_ACCESS_SHADER_RESOURCE;
                case Operation::Storage:
                    return D3D12_BARRIER_ACCESS_UNORDERED_ACCESS;
                case Operation::ColorAttachment:
                    return D3D12_BARRIER_ACCESS_RENDER_TARGET;
                case Operation::DepthStencilAttachment:
                    return state.access == RenderGraphSynchronizationAccess::Read ? D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ
                                                                                  : D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE;
                case Operation::CopySource:
                    return D3D12_BARRIER_ACCESS_COPY_SOURCE;
                case Operation::CopyDestination:
                    return D3D12_BARRIER_ACCESS_COPY_DEST;
                case Operation::GraphicsInputBuffer:
                    return D3D12_BARRIER_ACCESS_VERTEX_BUFFER | D3D12_BARRIER_ACCESS_CONSTANT_BUFFER | D3D12_BARRIER_ACCESS_INDEX_BUFFER;
            }
            HORO_INVARIANT(false);
            return D3D12_BARRIER_ACCESS_NO_ACCESS;
        }

        /** @brief Synchronizes only actual logical operation classes; queue compatibility was checked before recording. */
        D3D12_BARRIER_SYNC EnhancedSync(const RenderGraphLogicalState &state) {
            switch (state.operation) {
                case Operation::None:
                    return D3D12_BARRIER_SYNC_NONE;
                case Operation::CopySource:
                case Operation::CopyDestination:
                    return D3D12_BARRIER_SYNC_COPY;
                case Operation::ColorAttachment:
                    return D3D12_BARRIER_SYNC_RENDER_TARGET;
                case Operation::DepthStencilAttachment:
                    return D3D12_BARRIER_SYNC_DEPTH_STENCIL;
                case Operation::GraphicsInputBuffer:
                    return D3D12_BARRIER_SYNC_ALL;
                default:
                    return state.scope == RenderGraphPipelineScope::Compute ? D3D12_BARRIER_SYNC_COMPUTE_SHADING
                                                                            : D3D12_BARRIER_SYNC_ALL_SHADING;
            }
        }

        /** @brief Copy queues keep texture layout COMMON, as required by the enhanced barrier contract. */
        D3D12_BARRIER_LAYOUT EnhancedLayout(const RenderGraphLogicalState &state, const D3D12QueueKind queue) {
            if (queue == D3D12QueueKind::Copy)
                return D3D12_BARRIER_LAYOUT_COMMON;
            switch (state.operation) {
                case Operation::None:
                    return D3D12_BARRIER_LAYOUT_COMMON;
                case Operation::Sampled:
                    return D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
                case Operation::Storage:
                    return D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS;
                case Operation::ColorAttachment:
                    return D3D12_BARRIER_LAYOUT_RENDER_TARGET;
                case Operation::DepthStencilAttachment:
                    return state.access == RenderGraphSynchronizationAccess::Read ? D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ
                                                                                  : D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE;
                case Operation::CopySource:
                    return D3D12_BARRIER_LAYOUT_COPY_SOURCE;
                case Operation::CopyDestination:
                    return D3D12_BARRIER_LAYOUT_COPY_DEST;
                default:
                    return D3D12_BARRIER_LAYOUT_COMMON;
            }
        }

        /** @brief Encodes exact legacy whole-resource transitions and UAV ordering without synthetic subresources. */
        void LegacyBarrier(ID3D12GraphicsCommandList &commands, Resource &resource, const RenderGraphLogicalState &before,
                           const RenderGraphLogicalState &after, const D3D12QueueKind queue) {
            const auto first = LegacyState(before, queue);
            const auto second = LegacyState(after, queue);
            D3D12_RESOURCE_BARRIER barrier{};
            if (first != second) {
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Transition = {resource.native.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, first, second};
            } else {
                if (after.operation != Operation::Storage)
                    return;
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                barrier.UAV.pResource = resource.native.Get();
            }
            commands.ResourceBarrier(1, &barrier);
        }

        /** @brief Buffers have no layout; the barrier still finishes and flushes preceding access before COMMON interoperability. */
        void EnhancedBufferBarrier(ID3D12GraphicsCommandList7 &commands, Resource &resource, const RenderGraphLogicalState &before,
                                   const RenderGraphLogicalState &after) {
            const D3D12_BUFFER_BARRIER barrier{EnhancedSync(before),
                                               EnhancedSync(after),
                                               EnhancedAccess(before),
                                               EnhancedAccess(after),
                                               resource.native.Get(),
                                               0,
                                               UINT64_MAX};
            D3D12_BARRIER_GROUP group{};
            group.Type = D3D12_BARRIER_TYPE_BUFFER;
            group.NumBarriers = 1;
            group.pBufferBarriers = &barrier;
            commands.Barrier(1, &group);
        }

        /** @brief Texture layout remains explicit and whole-resource; no discard or content replacement is inferred. */
        void EnhancedTextureBarrier(ID3D12GraphicsCommandList7 &commands, Resource &resource, const RenderGraphLogicalState &before,
                                    const RenderGraphLogicalState &after, const D3D12QueueKind queue) {
            D3D12_TEXTURE_BARRIER barrier{};
            barrier.SyncBefore = EnhancedSync(before);
            barrier.SyncAfter = EnhancedSync(after);
            barrier.AccessBefore = EnhancedAccess(before);
            barrier.AccessAfter = EnhancedAccess(after);
            barrier.LayoutBefore = EnhancedLayout(before, queue);
            barrier.LayoutAfter = EnhancedLayout(after, queue);
            barrier.pResource = resource.native.Get();
            barrier.Subresources.IndexOrFirstMipLevel = D3D12_BARRIER_ALL_SUBRESOURCES;
            D3D12_BARRIER_GROUP group{};
            group.Type = D3D12_BARRIER_TYPE_TEXTURE;
            group.NumBarriers = 1;
            group.pTextureBarriers = &barrier;
            commands.Barrier(1, &group);
        }
    }  // namespace

    Result<void> ValidateCommandState(const RenderGraphLogicalState &state, const D3D12QueueKind queue) {
        if (state.operation > Operation::GraphicsInputBuffer)
            return Result<void>::Failure(ResourceError("render.d3d12.command_state", "Use a declared logical synchronization operation."));
        if (queue == D3D12QueueKind::Copy && !CopyOperation(state.operation))
            return Result<void>::Failure(ResourceError("render.d3d12.command_state", "Copy queues admit only COMMON and copy access."));
        if (queue == D3D12QueueKind::Compute && GraphicsOnlyOperation(state.operation))
            return Result<void>::Failure(
                ResourceError("render.d3d12.command_state", "Graphics-only state requires the admitted direct queue."));
        return Result<void>::Success();
    }

    void RecordCommandBarrier(ID3D12GraphicsCommandList &commands, ID3D12GraphicsCommandList7 *enhanced, Resource &resource,
                              const RenderGraphLogicalState &before, const RenderGraphLogicalState &after, const D3D12QueueKind queue) {
        if (resource.kind == Resource::Kind::Buffer && resource.buffer.access == RenderBufferAccess::HostVisible)
            return;  // Upload heaps remain GENERIC_READ; admission permits only read-compatible operations.
        if (!enhanced)
            return LegacyBarrier(commands, resource, before, after, queue);
        if (resource.kind == Resource::Kind::Buffer)
            return EnhancedBufferBarrier(*enhanced, resource, before, after);
        EnhancedTextureBarrier(*enhanced, resource, before, after, queue);
    }

    void RecordCommandActivation(ID3D12GraphicsCommandList &commands, Resource &resource) {
        if (!resource.activationPending)
            return;
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
        barrier.Aliasing.pResourceAfter = resource.native.Get();
        commands.ResourceBarrier(1, &barrier);
    }
}  // namespace Horo::Render::D3D12Detail
#endif
