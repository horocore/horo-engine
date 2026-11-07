#pragma once
/** @file MetalVfxGpuGraphInternal.h @brief Exact issued-inventory matching shared by native compute and direct draw. */
#include "MetalRenderBackendErrors.h"
#include "MetalVfxGpuState.h"

#include <algorithm>

namespace Horo::Render::Detail {
    inline bool MetalVfxGpuComplete(id<MTLCommandBuffer> commands) noexcept {
        return commands == nil || commands.status == MTLCommandBufferStatusCompleted || commands.status == MTLCommandBufferStatusError;
    }

    inline bool MetalVfxGpuQuiescent(const MetalBufferInstance &buffer) noexcept {
        return MetalVfxGpuComplete(buffer.use.last) && MetalVfxGpuComplete(buffer.use.previous);
    }

    inline std::uint64_t MetalVfxGraphInstance(const RenderGraphResourceId id, const RenderGraphExecutionRequest &request) {
        const auto found = std::ranges::find(request.resources, id, &RenderGraphResourceInstance::resource);
        return found == request.resources.end() ? 0 : found->instance;
    }

    /** @brief Requires both the exact frontend generation and its resolved native instance. */
    inline bool MetalVfxBufferMatches(const MetalVfxGpuBuffer &buffer, const RenderGraphResourceId id,
                                      const RenderGraphExecutionRequest &request) {
        const auto resources = request.graph.Resources();
        const auto found = std::ranges::find(resources, id, &RenderGraphResource::id);
        if (found == resources.end())
            return false;
        const auto *handle = std::get_if<RenderBufferHandle>(&found->binding);
        return handle && *handle == buffer.issued.handle && MetalVfxGraphInstance(id, request) == buffer.issued.instance;
    }

    inline std::size_t MetalVfxStateIndex(const MetalVfxGpuSimulation &simulation, const RenderGraphResourceId id,
                                          const RenderGraphExecutionRequest &request) {
        for (std::size_t index = 0; index < simulation.states.size(); ++index)
            if (MetalVfxBufferMatches(simulation.states[index], id, request))
                return index;
        return simulation.states.size();
    }

    inline std::size_t MetalVfxObservationIndex(const MetalVfxGpuSimulation &simulation, const RenderGraphResourceId id,
                                                const RenderGraphExecutionRequest &request) {
        for (std::size_t index = 0; index < simulation.observations.size(); ++index)
            if (MetalVfxBufferMatches(simulation.observations[index], id, request))
                return index;
        return simulation.observations.size();
    }

    /** @brief Borrows only the exact existing frontend signal of the partition containing this compute pass. */
    inline RenderTimelinePoint MetalVfxStepSignal(const RenderGraphVfxGpuCompute &workload, const RenderGraphExecutionRequest &request) {
        for (std::size_t index = 0; index < request.workloads.size(); ++index) {
            if (std::get_if<RenderGraphVfxGpuCompute>(&request.workloads[index].workload) != &workload)
                continue;
            for (const auto &submission : request.submissions)
                if (index >= submission.passes.offset && index - submission.passes.offset < submission.passes.count)
                    return submission.submission.signal;
        }
        return {};
    }
}  // namespace Horo::Render::Detail
