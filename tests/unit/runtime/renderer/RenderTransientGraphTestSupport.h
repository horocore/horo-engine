#pragma once

#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/RenderGraphLifetime.h"
#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "RenderGraphTestUtils.h"

#include <array>
#include <vector>

namespace Horo::Render::TransientTest {
    enum class CopyShape {
        Aliased,
        Incompatible,
        Overlapping,
        DifferentRoles
    };

    /** @brief Owns real compiler output and the exact operations that consume its lifetime proof. */
    struct GraphSources {
        RenderGraph graph;
        RenderGraphSchedule schedule;
        RenderGraphLifetimePlan lifetime;
        CompiledRenderGraphExecution execution;
        std::vector<RenderGraphPassWorkload> workloads;
    };

    inline GraphSources CompileSources(RenderGraphBuilder &builder, const std::span<const RenderGraphTransientRequirement> requirements,
                                       const std::span<const RenderGraphImportedState> initial,
                                       const std::span<const RenderQueueAssignment> queues,
                                       std::vector<RenderGraphPassWorkload> workloads) {
        auto graph = Test::RequireGraph(builder);
        auto schedule = Test::RequireSchedule(graph);
        auto lifetime = CompileRenderGraphLifetimePlan(graph, schedule, requirements);
        REQUIRE(lifetime.HasValue());
        auto synchronization = SynthesizeRenderGraphSynchronization(graph, schedule, queues, initial);
        REQUIRE(synchronization.HasValue());
        auto execution = CompileRenderGraphExecution(graph, schedule, synchronization.Value(), queues);
        REQUIRE(execution.HasValue());
        return {std::move(graph), std::move(schedule), std::move(lifetime).Value(), std::move(execution).Value(), std::move(workloads)};
    }

    inline RenderBufferDescriptor CopyBuffer(const std::size_t bytes = 16) {
        return {.byteSize = bytes,
                .usage = RenderBufferUsage::CopySource | RenderBufferUsage::CopyDestination,
                .access = RenderBufferAccess::DeviceLocal};
    }

    inline GraphSources CopyGraph(const RenderBufferHandle sourceHandle = {{41}, 1, 1},
                                  const RenderBufferHandle destinationHandle = {{41}, 2, 1}, const CopyShape shape = CopyShape::Aliased,
                                  const bool unused = false) {
        auto builder = Test::RequireBuilder({.maxPasses = 4, .maxResources = 5, .maxUsages = 8, .maxDependencies = 3});
        const auto source = Test::RequireResource(builder.ImportBuffer(sourceHandle, RenderGraphResourceClass::Persistent));
        const auto destination = Test::RequireResource(builder.ImportBuffer(destinationHandle, RenderGraphResourceClass::Persistent));
        const auto a = Test::RequireResource(builder.AddTransientResource(RenderGraphResourceKind::Buffer));
        const auto b = Test::RequireResource(builder.AddTransientResource(RenderGraphResourceKind::Buffer));
        std::array pairs{std::pair{source, a}, std::pair{a, destination}, std::pair{source, b}, std::pair{b, destination}};
        if (shape == CopyShape::Overlapping)
            pairs = {std::pair{source, a}, std::pair{a, b}, std::pair{b, destination}, std::pair{source, destination}};
        std::vector<RenderGraphPassWorkload> workloads;
        RenderGraphPassRef previous;
        for (std::size_t index = 0; index < pairs.size(); ++index) {
            const auto role = shape == CopyShape::DifferentRoles && index >= 2 ? RenderQueueRole::Graphics : RenderQueueRole::Transfer;
            const auto pass = Test::RequirePass(builder, RenderPassKind::Copy, role);
            Test::RequireUsage(builder, {pass, pairs[index].first, RenderGraphAccess::Read, RenderGraphUsageKind::CopySource});
            Test::RequireUsage(builder, {pass, pairs[index].second, RenderGraphAccess::Write, RenderGraphUsageKind::CopyDestination});
            if (previous.IsValid())
                Test::RequireDependency(builder, {previous, pass, RenderGraphDependencyKind::ExecutionOrder});
            previous = pass;
            workloads.push_back({pass, RenderGraphBufferCopy{pairs[index].first, pairs[index].second, 0, 0, 16}});
        }
        std::vector requirements{RenderGraphTransientRequirement{a, CopyBuffer()},
                                 RenderGraphTransientRequirement{b, CopyBuffer(shape == CopyShape::Incompatible ? 32 : 16)}};
        if (unused)
            requirements.push_back({Test::RequireResource(builder.AddTransientResource(RenderGraphResourceKind::Buffer)), CopyBuffer()});
        const std::array initial{RenderGraphImportedState{source,
                                                          {RenderGraphSynchronizationAccess::Read,
                                                           RenderGraphSynchronizationOperation::CopySource,
                                                           RenderGraphPipelineScope::Transfer,
                                                           RenderGraphTextureLayout::NotApplicable,
                                                           {1}}},
                                 RenderGraphImportedState{destination,
                                                          {RenderGraphSynchronizationAccess::Write,
                                                           RenderGraphSynchronizationOperation::CopyDestination,
                                                           RenderGraphPipelineScope::Transfer,
                                                           RenderGraphTextureLayout::NotApplicable,
                                                           {1}}}};
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Transfer, {1}},
                                RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
        return CompileSources(builder, requirements, initial, queues, std::move(workloads));
    }

    inline GraphSources ColorGraph(const std::array<RenderTextureHandle, 2> imported = {}) {
        auto builder = Test::RequireBuilder({.maxPasses = 2, .maxResources = 4, .maxUsages = 2, .maxDependencies = 1});
        std::vector<RenderGraphImportedState> initial;
        for (const auto texture : imported) {
            if (texture.IsValid()) {
                const auto id = Test::RequireResource(builder.ImportTexture(texture, RenderGraphResourceClass::Persistent));
                initial.push_back({id,
                                   {RenderGraphSynchronizationAccess::Read,
                                    RenderGraphSynchronizationOperation::Sampled,
                                    RenderGraphPipelineScope::Graphics,
                                    RenderGraphTextureLayout::ShaderReadOnly,
                                    {1}}});
            }
        }
        const auto a = Test::RequireResource(builder.AddTransientResource(RenderGraphResourceKind::Texture));
        const auto b = Test::RequireResource(builder.AddTransientResource(RenderGraphResourceKind::Texture));
        const auto first = Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
        const auto last = Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
        Test::RequireUsage(builder, {first, a, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment});
        Test::RequireUsage(builder, {last, b, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment});
        Test::RequireDependency(builder, {first, last, RenderGraphDependencyKind::ExecutionOrder});
        const RenderTextureDescriptor descriptor{.extent = {2, 2},
                                                 .format = RenderTextureFormat::Rgba8Unorm,
                                                 .usage = RenderTextureUsage::RenderAttachment};
        const std::array requirements{RenderGraphTransientRequirement{a, descriptor}, RenderGraphTransientRequirement{b, descriptor}};
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
        const PrimaryOutputAttachment clear{AttachmentLoadOperation::Clear, AttachmentStoreOperation::Store, {1, 0, 0, 1}};
        return CompileSources(builder, requirements, initial, queues,
                              {{first, RenderGraphColorAttachment{a, clear}}, {last, RenderGraphColorAttachment{b, clear}}});
    }

    inline RenderFrameScope BeginFrame(RenderFrontend &frontend, const std::uint64_t number = 1) {
        auto frame = frontend.BeginFrame({number, {2, 2}});
        REQUIRE(frame.HasValue());
        return std::move(frame).Value();
    }

    inline std::array<RenderBufferHandle, 2> ImportedCopyBuffers(RenderFrontend &frontend) {
        const std::array<std::byte, 16> bytes{};
        const auto source = frontend.CreateBuffer(CopyBuffer(), bytes);
        const auto destination = frontend.CreateBuffer(CopyBuffer(), bytes);
        REQUIRE(source.HasValue());
        REQUIRE(destination.HasValue());
        REQUIRE(frontend.ProcessResourceRequests().HasValue());
        return {source.Value().handle, destination.Value().handle};
    }
}  // namespace Horo::Render::TransientTest
