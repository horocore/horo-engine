#pragma once

#include "Horo/Runtime/Render/RenderGraphWorkload.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Render::SubmissionTest {
    template <typename T> [[nodiscard]] T Take(Result<T> result) {
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    /** @brief Owning copy/ownership graph; submission views are formed only after the packet reaches its final address. */
    struct BufferGraph {
        CompiledRenderGraphExecution graph;
        std::array<RenderGraphPassWorkload, 2> workloads;
        std::array<RenderGraphResourceInstance, 3> instances;
        RenderQueueId producerQueue;
        RenderQueueId consumerQueue;
        std::array<RenderTimelinePoint, 1> producerWait;

        [[nodiscard]] std::array<RenderGraphQueueSubmission, 2> Submissions() const {
            const RenderQueueSubmission producer{producerQueue, {1}, {}, {producerQueue, 1}};
            const RenderQueueSubmission consumer{consumerQueue,
                                                 {2},
                                                 producerWait,
                                                 {consumerQueue, producerQueue == consumerQueue ? 2U : 1U}};
            return {RenderGraphQueueSubmission{producer, {0, 1}}, RenderGraphQueueSubmission{consumer, {1, 1}}};
        }
    };

    /** @brief Builds real portable graph/synchronization/execution values with preserved source and discarded first-write destinations. */
    [[nodiscard]] inline BufferGraph CopyGraph(const std::array<RenderBufferHandle, 3> handles = {RenderBufferHandle{{41}, 1, 1},
                                                                                                  RenderBufferHandle{{41}, 2, 1},
                                                                                                  RenderBufferHandle{{41}, 3, 1}},
                                               const RenderQueueId producer = {1}, const RenderQueueId consumer = {2}) {
        auto builder = Take(RenderGraphBuilder::Create({.maxPasses = 2, .maxResources = 3, .maxUsages = 4, .maxDependencies = 4}));
        const auto first = Take(builder.AddPass(RenderPassKind::Copy, RenderQueueRole::Graphics));
        const auto second = Take(builder.AddPass(RenderPassKind::Copy, RenderQueueRole::Transfer));
        const auto source = Take(builder.ImportBuffer(handles[0], RenderGraphResourceClass::Persistent));
        const auto middle = Take(builder.ImportBuffer(handles[1], RenderGraphResourceClass::Persistent));
        const auto destination = Take(builder.ImportBuffer(handles[2], RenderGraphResourceClass::Persistent));
        REQUIRE(builder.AddUsage({first, source, RenderGraphAccess::Read, RenderGraphUsageKind::CopySource}).HasValue());
        REQUIRE(builder.AddUsage({first, middle, RenderGraphAccess::Write, RenderGraphUsageKind::CopyDestination}).HasValue());
        REQUIRE(builder.AddUsage({second, middle, RenderGraphAccess::Read, RenderGraphUsageKind::CopySource}).HasValue());
        REQUIRE(builder.AddUsage({second, destination, RenderGraphAccess::Write, RenderGraphUsageKind::CopyDestination}).HasValue());
        const auto graph = Take(builder.Finalize());
        const auto schedule = Take(CompileRenderGraph(graph));
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, producer},
                                RenderQueueAssignment{RenderQueueRole::Transfer, consumer}};
        const std::array initial{RenderGraphImportedState{source,
                                                          {RenderGraphSynchronizationAccess::Read,
                                                           RenderGraphSynchronizationOperation::CopySource,
                                                           RenderGraphPipelineScope::Transfer, RenderGraphTextureLayout::NotApplicable,
                                                           producer}},
                                 RenderGraphImportedState{middle, {}, RenderGraphInitialContents::Discarded},
                                 RenderGraphImportedState{destination, {}, RenderGraphInitialContents::Discarded}};
        const auto synchronization = Take(SynthesizeRenderGraphSynchronization(graph, schedule, queues, initial));
        auto compiled = Take(CompileRenderGraphExecution(graph, schedule, synchronization, queues));
        const std::array workloads{RenderGraphPassWorkload{first, RenderGraphBufferCopy{source, middle, 0, 0, 16}},
                                   RenderGraphPassWorkload{second, RenderGraphBufferCopy{middle, destination, 0, 0, 16}}};
        const std::array instances{RenderGraphResourceInstance{source, 11}, RenderGraphResourceInstance{middle, 12},
                                   RenderGraphResourceInstance{destination, 13}};
        return {std::move(compiled), workloads, instances, producer, consumer, {RenderTimelinePoint{producer, 1}}};
    }
}  // namespace Horo::Render::SubmissionTest
