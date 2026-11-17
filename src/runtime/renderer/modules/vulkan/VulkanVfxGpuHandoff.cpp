#include "VulkanGraphRuntimeInternal.h"

#include <algorithm>

namespace Horo::Render::Detail {
    namespace {
        bool LaterUse(const CompiledRenderGraphExecution &graph, std::size_t pass, RenderGraphResourceId resource) {
            for (std::size_t i = pass + 1; i < graph.Passes().size(); ++i) {
                const auto &entry = graph.Passes()[i];
                for (const auto &usage : graph.Usages().subspan(entry.usages.offset, entry.usages.count))
                    if (usage.resource == resource)
                        return true;
            }
            return false;
        }
    }  // namespace

    Result<void> VulkanGraphRuntime::Impl::FinishParticleStates(const RenderGraphExecutionRequest &request, std::size_t pass) {
        const auto &entry = request.graph.Passes()[pass];
        for (const auto &usage : request.graph.Usages().subspan(entry.usages.offset, entry.usages.count)) {
            const auto index = usage.resource.value - 1;
            if (!vfxGpu.Owns(bindings[index].buffer) || LaterUse(request.graph, pass, usage.resource))
                continue;
            const auto after =
                RenderGraphLogicalState{RenderGraphSynchronizationAccess::Read, RenderGraphSynchronizationOperation::Storage,
                                        RenderGraphPipelineScope::External, RenderGraphTextureLayout::NotApplicable, entry.queue};
            const auto *queue = Queue(entry.queue);
            const auto barrier =
                TranslateVulkanGraphBarrier(bindings[index], states[index].logical, after,
                                            {.computeAvailable = queue && queue->supportsCompute, .particleHandoff = true});
            if (barrier.HasError())
                return Result<void>::Failure(barrier.ErrorValue());
            if (barriers.size() == barriers.capacity())
                return Result<void>::Failure(Invalid("Particle final-state barrier capacity exhausted."));
            barriers.push_back(barrier.Value());
            states[index].logical = after;
            const auto producer = std::ranges::find_if(updates.rbegin(), updates.rend(), [&](const auto &update) {
                return update.target == bindings[index].state;
            });
            if (producer == updates.rend())
                return Result<void>::Failure(Invalid("Particle handoff has no exact current producer state update."));
            producer->value = states[index];
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Render::Detail
