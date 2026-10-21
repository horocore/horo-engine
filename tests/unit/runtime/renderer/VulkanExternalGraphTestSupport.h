#pragma once

#include "VulkanGraphTestSupport.h"

namespace Horo::Render::VulkanExternalTest {
    struct ExternalGraphPacket {
        CompiledRenderGraphExecution graph;
        std::array<RenderGraphPassWorkload, 1> workloads;
        std::array<RenderGraphResourceInstance, 1> instances;
        const std::array<RenderGraphQueueSubmission, 1> submissions{RenderGraphQueueSubmission{{{1}, {1}, {}, {{1}, 1}}, {0, 1}}};
    };

    inline ExternalGraphPacket ExternalGraph(const RenderTextureHandle handle, const RenderTextureDescriptor &descriptor,
                                             const RenderGraphLogicalState &requiredFinal, const std::uint64_t instance = 0,
                                             const bool includeFinal = true) {
        auto builder =
            SubmissionTest::Take(RenderGraphBuilder::Create({.maxPasses = 1, .maxResources = 1, .maxUsages = 1, .maxDependencies = 1}));
        const auto pass = SubmissionTest::Take(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
        const auto resource = SubmissionTest::Take(builder.ImportTexture(handle, RenderGraphResourceClass::External));
        const bool depth = descriptor.format == RenderTextureFormat::Depth32Float;
        REQUIRE(builder
                    .AddUsage({pass, resource, RenderGraphAccess::Write,
                               depth ? RenderGraphUsageKind::DepthStencilAttachment : RenderGraphUsageKind::ColorAttachment})
                    .HasValue());
        auto graph = SubmissionTest::Take(builder.Finalize());
        auto schedule = SubmissionTest::Take(CompileRenderGraph(graph));
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
        const std::array initial{RenderGraphImportedState{resource, {}, RenderGraphInitialContents::Discarded}};
        const std::array final{RenderGraphFinalState{resource, requiredFinal}};
        auto synchronization =
            SubmissionTest::Take(SynthesizeRenderGraphSynchronization(graph, schedule, queues, initial,
                                                                      includeFinal ? std::span<const RenderGraphFinalState>{final}
                                                                                   : std::span<const RenderGraphFinalState>{}));
        auto execution = SubmissionTest::Take(CompileRenderGraphExecution(graph, schedule, synchronization, queues));
        RenderGraphWorkload workload = depth ? RenderGraphWorkload{RenderGraphDepthAttachment{resource, 1.0F}}
                                             : RenderGraphWorkload{RenderGraphColorAttachment{resource, {}}};
        return {std::move(execution),
                {RenderGraphPassWorkload{pass, std::move(workload)}},
                {RenderGraphResourceInstance{resource, instance}}};
    }

}  // namespace Horo::Render::VulkanExternalTest
