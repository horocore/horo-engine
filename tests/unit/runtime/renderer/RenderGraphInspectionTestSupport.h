#pragma once
/** @brief Shared real compiler-output fixtures for graph inspection and GUI contract tests. */
#include "Horo/Runtime/Render/RenderGraphInspection.h"
#include "RenderGraphTestUtils.h"

#include <array>

namespace Horo::Render::Test {
    constexpr std::array Queues{RenderQueueAssignment{RenderQueueRole::Graphics, RenderQueueId{3}},
                                RenderQueueAssignment{RenderQueueRole::Compute, RenderQueueId{5}}};

    struct Sources {
        RenderGraph graph;
        RenderGraphSchedule schedule;
        RenderGraphLifetimePlan lifetime;
        CompiledRenderGraphExecution execution;
    };

    /** @brief Finishes real synchronization/execution compilation while preserving owned source provenance. */
    inline Sources FinishSources(RenderGraph graph, RenderGraphSchedule schedule, RenderGraphLifetimePlan lifetime,
                                 const std::span<const RenderGraphImportedState> initialStates = {}) {
        auto synchronization = SynthesizeRenderGraphSynchronization(graph, schedule, Queues, initialStates);
        REQUIRE(synchronization.HasValue());
        auto execution = CompileRenderGraphExecution(graph, schedule, synchronization.Value(), Queues);
        REQUIRE(execution.HasValue());
        return {std::move(graph), std::move(schedule), std::move(lifetime), std::move(execution).Value()};
    }

    inline Sources CompileSources() {
        auto builder = RequireBuilder();
        const auto write = RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
        const auto read = RequirePass(builder, RenderPassKind::Compute, RenderQueueRole::Compute);
        const auto culled =
            RequirePass(builder, RenderPassKind::Compute, RenderQueueRole::Compute, RenderGraphPassCullPolicy::AllowCullIfOutputsUnused);
        const auto used = RequireResource(builder.AddTransientResource(RenderGraphResourceKind::Buffer));
        const auto unused = RequireResource(builder.AddTransientResource(RenderGraphResourceKind::Buffer));
        RequireUsage(builder, {write, used, RenderGraphAccess::Write, RenderGraphUsageKind::Storage});
        RequireUsage(builder, {read, used, RenderGraphAccess::Read, RenderGraphUsageKind::Storage});
        RequireUsage(builder, {culled, unused, RenderGraphAccess::Write, RenderGraphUsageKind::Storage});
        RequireDependency(builder, {write, read, RenderGraphDependencyKind::ResourceHazard});
        RequireExport(builder, used);
        auto graph = RequireGraph(builder);
        auto schedule = RequireSchedule(graph);
        const RenderBufferDescriptor descriptor{.byteSize = 256, .usage = RenderBufferUsage::Storage};
        const std::array requirements{RenderGraphTransientRequirement{used, descriptor},
                                      RenderGraphTransientRequirement{unused, descriptor}};
        auto lifetime = CompileRenderGraphLifetimePlan(graph, schedule, requirements);
        REQUIRE(lifetime.HasValue());
        return FinishSources(std::move(graph), std::move(schedule), std::move(lifetime).Value());
    }

}  // namespace Horo::Render::Test
