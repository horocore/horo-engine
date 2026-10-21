#include "Horo/Runtime/Render/RenderGraphExecution.h"
#include "Horo/Runtime/Render/RenderGraphSynchronization.h"
#include "Horo/Runtime/Render/RenderGraphSynchronizationErrors.h"
#include "RenderGraphTestUtils.h"

#include <array>

namespace Horo::Render {
    namespace {
        using namespace Test;

        constexpr std::array SharedQueue{
            RenderQueueAssignment{RenderQueueRole::Graphics, RenderQueueId{1}},
            RenderQueueAssignment{RenderQueueRole::Compute, RenderQueueId{1}},
            RenderQueueAssignment{RenderQueueRole::Transfer, RenderQueueId{1}},
        };

        RenderGraphLogicalState BufferState(const RenderGraphSynchronizationAccess access,
                                            const RenderGraphSynchronizationOperation operation, const RenderQueueId queue = {1},
                                            const RenderGraphPipelineScope scope = RenderGraphPipelineScope::External) {
            return {access, operation, scope, RenderGraphTextureLayout::NotApplicable, queue};
        }

        Result<RenderGraphSynchronizationPlan> Synthesize(const RenderGraph &graph, const RenderGraphSchedule &schedule,
                                                          const std::span<const RenderGraphImportedState> states = {}) {
            return SynthesizeRenderGraphSynchronization(graph, schedule, SharedQueue, states);
        }

        struct CompiledGraph {
            RenderGraph graph;
            RenderGraphSchedule schedule;
        };

        CompiledGraph RequireCompiledGraph(RenderGraphBuilder &builder) {
            RenderGraph graph = RequireGraph(builder);
            RenderGraphSchedule schedule = RequireSchedule(graph);
            return {std::move(graph), std::move(schedule)};
        }

        struct WriteReadGraph {
            RenderGraphPassRef write;
            RenderGraphPassRef read;
            CompiledGraph compiled;
        };

        WriteReadGraph RequireWriteReadGraph() {
            RenderGraphBuilder builder = RequireBuilder();
            const RenderGraphPassRef write = RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
            const RenderGraphPassRef read = RequirePass(builder, RenderPassKind::Compute, RenderQueueRole::Compute);
            const RenderGraphResourceId resource = RequireResource(builder.AddTransientResource(RenderGraphResourceKind::Buffer));
            RequireUsage(builder, {write, resource, RenderGraphAccess::Write, RenderGraphUsageKind::Storage});
            RequireUsage(builder, {read, resource, RenderGraphAccess::Read, RenderGraphUsageKind::Sampled});
            return {write, read, RequireCompiledGraph(builder)};
        }

        struct ThreeUseGraph {
            std::array<RenderGraphPassRef, 3> passes;
            CompiledGraph compiled;
        };

        ThreeUseGraph RequireThreeUseComputeGraph(const std::array<RenderGraphAccess, 3> accesses,
                                                  const std::array<RenderGraphUsageKind, 3> operations) {
            RenderGraphBuilder builder = RequireBuilder();
            std::array<RenderGraphPassRef, 3> passes;
            for (RenderGraphPassRef &pass : passes) {
                pass = RequirePass(builder, RenderPassKind::Compute, RenderQueueRole::Compute);
            }
            const RenderGraphResourceId resource = RequireResource(builder.AddTransientResource(RenderGraphResourceKind::Buffer));
            for (std::size_t index = 0; index < passes.size(); ++index) {
                RequireUsage(builder, {passes[index], resource, accesses[index], operations[index]});
            }
            return {passes, RequireCompiledGraph(builder)};
        }

        RenderGraphImportedState ComputeBufferReadState(const RenderGraphResourceId resource) {
            return {resource, BufferState(RenderGraphSynchronizationAccess::Read, RenderGraphSynchronizationOperation::Sampled, {1},
                                          RenderGraphPipelineScope::Compute)};
        }

        struct ImportedBufferReadBuilder {
            RenderGraphBuilder builder;
            RenderGraphPassRef pass;
            RenderGraphResourceId resource;
        };

        ImportedBufferReadBuilder RequireImportedBufferReadBuilder() {
            RenderGraphBuilder builder = RequireBuilder();
            const RenderGraphPassRef pass = RequirePass(builder, RenderPassKind::Compute, RenderQueueRole::Compute);
            const RenderGraphResourceId resource =
                RequireResource(builder.ImportBuffer(BufferHandle(), RenderGraphResourceClass::Persistent));
            RequireUsage(builder, {pass, resource, RenderGraphAccess::Read, RenderGraphUsageKind::Sampled});
            return {std::move(builder), pass, resource};
        }
    }  // namespace

    TEST_CASE("Synchronization synthesis derives RAW and WAW transitions deterministically", "[renderer][render-graph][sync]") {
        ThreeUseGraph fixture =
            RequireThreeUseComputeGraph({RenderGraphAccess::Write, RenderGraphAccess::Write, RenderGraphAccess::Read},
                                        {RenderGraphUsageKind::Storage, RenderGraphUsageKind::Storage, RenderGraphUsageKind::Sampled});

        auto synthesized = Synthesize(fixture.compiled.graph, fixture.compiled.schedule);
        REQUIRE(synthesized.HasValue());
        const auto transitions = synthesized.Value().Transitions();
        REQUIRE(transitions.size() == 3);
        CHECK(transitions[0].hazards == RenderGraphHazard::Initial);
        CHECK(transitions[0].oldState.access == RenderGraphSynchronizationAccess::None);
        CHECK(transitions[0].after == fixture.passes[0]);
        CHECK(HasRenderGraphHazard(transitions[1].hazards, RenderGraphHazard::WriteAfterWrite));
        CHECK(transitions[1].before == fixture.passes[0]);
        CHECK(transitions[1].after == fixture.passes[1]);
        CHECK(HasRenderGraphHazard(transitions[2].hazards, RenderGraphHazard::ReadAfterWrite));
        CHECK(transitions[2].before == fixture.passes[1]);
        CHECK(transitions[2].after == fixture.passes[2]);
    }

    TEST_CASE("Compatible imported reads share state without a barrier", "[renderer][render-graph][sync]") {
        ImportedBufferReadBuilder fixture = RequireImportedBufferReadBuilder();
        const RenderGraphPassRef second = RequirePass(fixture.builder, RenderPassKind::Compute, RenderQueueRole::Compute);
        RequireUsage(fixture.builder, {second, fixture.resource, RenderGraphAccess::Read, RenderGraphUsageKind::Sampled});
        CompiledGraph compiled = RequireCompiledGraph(fixture.builder);
        const std::array initial{ComputeBufferReadState(fixture.resource)};

        auto synthesized = Synthesize(compiled.graph, compiled.schedule, initial);
        REQUIRE(synthesized.HasValue());
        CHECK(synthesized.Value().Transitions().empty());
        CHECK(synthesized.Value().OwnershipTransfers().empty());
    }

    TEST_CASE("Distinct effective queues emit one matched ownership transfer", "[renderer][render-graph][sync]") {
        WriteReadGraph fixture = RequireWriteReadGraph();
        constexpr std::array queues{
            RenderQueueAssignment{RenderQueueRole::Compute, RenderQueueId{8}},
            RenderQueueAssignment{RenderQueueRole::Graphics, RenderQueueId{5}},
        };

        auto synthesized = SynthesizeRenderGraphSynchronization(fixture.compiled.graph, fixture.compiled.schedule, queues, {});
        REQUIRE(synthesized.HasValue());
        REQUIRE(synthesized.Value().QueueAssignments().size() == 2);
        CHECK(synthesized.Value().QueueAssignments()[0] == RenderQueueAssignment{RenderQueueRole::Graphics, RenderQueueId{5}});
        CHECK(synthesized.Value().QueueAssignments()[1] == RenderQueueAssignment{RenderQueueRole::Compute, RenderQueueId{8}});
        REQUIRE(synthesized.Value().OwnershipTransfers().size() == 1);
        const RenderGraphOwnershipTransfer &transfer = synthesized.Value().OwnershipTransfers().front();
        CHECK(transfer.releaseAfter == fixture.write);
        CHECK(transfer.acquireBefore == fixture.read);
        CHECK(transfer.sourceQueue == RenderQueueId{5});
        CHECK(transfer.destinationQueue == RenderQueueId{8});
    }

    TEST_CASE("Aliased queue roles do not invent ownership transfers", "[renderer][render-graph][sync]") {
        WriteReadGraph fixture = RequireWriteReadGraph();

        auto synthesized = Synthesize(fixture.compiled.graph, fixture.compiled.schedule);
        REQUIRE(synthesized.HasValue());
        CHECK(synthesized.Value().Transitions().size() == 2);
        CHECK(synthesized.Value().OwnershipTransfers().empty());
    }

    TEST_CASE("Read-write uses retain every applicable hazard reason", "[renderer][render-graph][sync]") {
        ThreeUseGraph fixture =
            RequireThreeUseComputeGraph({RenderGraphAccess::Write, RenderGraphAccess::ReadWrite, RenderGraphAccess::ReadWrite},
                                        {RenderGraphUsageKind::Storage, RenderGraphUsageKind::Storage, RenderGraphUsageKind::Storage});

        auto synthesized = Synthesize(fixture.compiled.graph, fixture.compiled.schedule);
        REQUIRE(synthesized.HasValue());
        REQUIRE(synthesized.Value().Transitions().size() == 3);
        const RenderGraphHazard hazards = synthesized.Value().Transitions()[2].hazards;
        CHECK(HasRenderGraphHazard(hazards, RenderGraphHazard::ReadAfterWrite));
        CHECK(HasRenderGraphHazard(hazards, RenderGraphHazard::WriteAfterRead));
        CHECK(HasRenderGraphHazard(hazards, RenderGraphHazard::WriteAfterWrite));
        CHECK_FALSE(HasRenderGraphHazard(hazards, RenderGraphHazard::StateChange));
    }

    TEST_CASE("Undefined transient reads fail before synchronization publication", "[renderer][render-graph][sync]") {
        RenderGraphBuilder builder = RequireBuilder();
        const RenderGraphPassRef read = RequirePass(builder, RenderPassKind::Compute, RenderQueueRole::Compute);
        const RenderGraphResourceId resource = RequireResource(builder.AddTransientResource(RenderGraphResourceKind::Buffer));
        RequireUsage(builder, {read, resource, RenderGraphAccess::Read, RenderGraphUsageKind::Sampled});
        RenderGraph graph = RequireGraph(builder);

        RequireError(CompileRenderGraph(graph), "render.graph.read_before_write");
    }

    TEST_CASE("Imported resources require one exact compatible initial state", "[renderer][render-graph][sync]") {
        RenderGraphBuilder builder = RequireBuilder();
        const RenderGraphPassRef pass = RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
        const RenderGraphResourceId resource = RequireResource(builder.ImportTexture(TextureHandle(), RenderGraphResourceClass::External));
        RequireUsage(builder, {pass, resource, RenderGraphAccess::Read, RenderGraphUsageKind::Sampled});
        CompiledGraph compiled = RequireCompiledGraph(builder);

        RequireError(Synthesize(compiled.graph, compiled.schedule), "render.graph.synchronization.initial_state_missing");

        const std::array invalid{RenderGraphImportedState{resource, BufferState(RenderGraphSynchronizationAccess::Read,
                                                                                RenderGraphSynchronizationOperation::Sampled)}};
        RequireError(Synthesize(compiled.graph, compiled.schedule, invalid), "render.graph.synchronization.initial_state_invalid");

        const RenderGraphLogicalState valid{RenderGraphSynchronizationAccess::Read,
                                            RenderGraphSynchronizationOperation::Sampled,
                                            RenderGraphPipelineScope::External,
                                            RenderGraphTextureLayout::ShaderReadOnly,
                                            {1}};
        const std::array duplicate{RenderGraphImportedState{resource, valid}, RenderGraphImportedState{resource, valid}};
        RequireError(Synthesize(compiled.graph, compiled.schedule, duplicate), "render.graph.synchronization.initial_state_duplicate");
    }

    TEST_CASE("Synthesis rejects incomplete or ambiguous queue topology", "[renderer][render-graph][sync]") {
        RenderGraphBuilder builder = RequireBuilder();
        const RenderGraphPassRef pass = RequirePass(builder, RenderPassKind::Compute, RenderQueueRole::Compute);
        const RenderGraphResourceId resource = RequireResource(builder.AddTransientResource(RenderGraphResourceKind::Buffer));
        RequireUsage(builder, {pass, resource, RenderGraphAccess::Write, RenderGraphUsageKind::Storage});
        CompiledGraph compiled = RequireCompiledGraph(builder);

        constexpr std::array missing{RenderQueueAssignment{RenderQueueRole::Graphics, RenderQueueId{1}}};
        RequireError(SynthesizeRenderGraphSynchronization(compiled.graph, compiled.schedule, missing, {}),
                     "render.graph.synchronization.queue_topology_invalid");

        constexpr std::array duplicate{
            RenderQueueAssignment{RenderQueueRole::Compute, RenderQueueId{1}},
            RenderQueueAssignment{RenderQueueRole::Compute, RenderQueueId{2}},
        };
        RequireError(SynthesizeRenderGraphSynchronization(compiled.graph, compiled.schedule, duplicate, {}),
                     "render.graph.synchronization.queue_topology_invalid");
    }

    TEST_CASE("Synthesis rejects a pass-internal state conflict", "[renderer][render-graph][sync]") {
        ImportedBufferReadBuilder fixture = RequireImportedBufferReadBuilder();
        RequireUsage(fixture.builder, {fixture.pass, fixture.resource, RenderGraphAccess::Write, RenderGraphUsageKind::Storage});
        CompiledGraph compiled = RequireCompiledGraph(fixture.builder);
        const std::array initial{ComputeBufferReadState(fixture.resource)};

        RequireError(Synthesize(compiled.graph, compiled.schedule, initial), "render.graph.synchronization.state_unsupported");
    }

    TEST_CASE("Discarded imported contents require an exact undefined state and first write", "[renderer][render-graph][sync]") {
        auto builder = RequireBuilder();
        const auto pass = RequirePass(builder, RenderPassKind::Copy, RenderQueueRole::Transfer);
        const auto resource = RequireResource(builder.ImportBuffer(BufferHandle(), RenderGraphResourceClass::Persistent));
        bool read = false;
        SECTION("first write") {}
        SECTION("first read rejected") {
            read = true;
        }
        RequireUsage(builder, {pass, resource, read ? RenderGraphAccess::Read : RenderGraphAccess::Write,
                               read ? RenderGraphUsageKind::CopySource : RenderGraphUsageKind::CopyDestination});
        auto compiled = RequireCompiledGraph(builder);
        std::array initial{RenderGraphImportedState{resource, {}, RenderGraphInitialContents::Discarded}};
        const auto synthesized = Synthesize(compiled.graph, compiled.schedule, initial);
        if (read) {
            REQUIRE(synthesized.HasError());
        } else {
            REQUIRE(synthesized.HasValue());
            REQUIRE(synthesized.Value().Transitions().size() == 1);
            CHECK(synthesized.Value().Transitions()[0].oldState == RenderGraphLogicalState{});
            CHECK(synthesized.Value().ImportedStates()[0].contents == RenderGraphInitialContents::Discarded);
        }
        initial[0].state.queue = {1};
        CHECK(Synthesize(compiled.graph, compiled.schedule, initial).HasError());
        initial[0].state = {};
        initial[0].contents = static_cast<RenderGraphInitialContents>(255);
        CHECK(Synthesize(compiled.graph, compiled.schedule, initial).HasError());
    }

    TEST_CASE("Discarded imported buffer storage cannot begin with read-write access", "[renderer][render-graph][sync]") {
        auto builder = RequireBuilder();
        const auto pass = RequirePass(builder, RenderPassKind::Compute, RenderQueueRole::Compute);
        const auto resource = RequireResource(builder.ImportBuffer(BufferHandle(), RenderGraphResourceClass::Persistent));
        RequireUsage(builder, {pass, resource, RenderGraphAccess::ReadWrite, RenderGraphUsageKind::Storage});
        auto compiled = RequireCompiledGraph(builder);
        const std::array initial{RenderGraphImportedState{resource, {}, RenderGraphInitialContents::Discarded}};
        CHECK(Synthesize(compiled.graph, compiled.schedule, initial).HasError());
    }

    TEST_CASE("External final states survive compilation as post-producer obligations", "[render][graph][synchronization]") {
        using namespace Test;
        auto builder = RequireBuilder();
        const auto pass = RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
        const auto resource = RequireResource(builder.ImportTexture(TextureHandle(), RenderGraphResourceClass::External));
        RequireUsage(builder, {pass, resource, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment});
        const auto graph = RequireGraph(builder);
        const auto schedule = RequireSchedule(graph);
        const RenderGraphLogicalState state{RenderGraphSynchronizationAccess::Write,
                                            RenderGraphSynchronizationOperation::ColorAttachment,
                                            RenderGraphPipelineScope::External,
                                            RenderGraphTextureLayout::ColorAttachment,
                                            {1}};
        const std::array initial{RenderGraphImportedState{resource, state}};
        const std::array final{RenderGraphFinalState{resource, state}};
        auto synchronization = SynthesizeRenderGraphSynchronization(graph, schedule, SharedQueue, initial, final);
        REQUIRE(synchronization.HasValue());
        REQUIRE(synchronization.Value().ExternalReleases().size() == 1);
        const auto &release = synchronization.Value().ExternalReleases().front();
        CHECK(release.releaseAfter == pass);
        CHECK(release.resource == resource);
        CHECK(release.newState == state);
        auto execution = CompileRenderGraphExecution(graph, schedule, synchronization.Value(), SharedQueue);
        REQUIRE(execution.HasValue());
        REQUIRE(execution.Value().ExternalReleases().size() == 1);
        CHECK(execution.Value().ExternalReleases().front().releaseAfter == pass);
        auto moved = std::move(execution.Value());
        CHECK(moved.ExternalReleases().size() == 1);
    }

    TEST_CASE("Malformed duplicate and nonexternal final states cannot produce release obligations", "[render][graph][synchronization]") {
        using namespace Test;
        auto builder = RequireBuilder();
        const auto pass = RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
        RenderGraphResourceClass resourceClass = RenderGraphResourceClass::External;
        SECTION("persistent resource is not an external handoff") {
            resourceClass = RenderGraphResourceClass::Persistent;
        }
        const auto resource = RequireResource(builder.ImportTexture(TextureHandle(), resourceClass));
        RequireUsage(builder, {pass, resource, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment});
        const auto graph = RequireGraph(builder);
        const auto schedule = RequireSchedule(graph);
        const RenderGraphLogicalState state{RenderGraphSynchronizationAccess::Write,
                                            RenderGraphSynchronizationOperation::ColorAttachment,
                                            RenderGraphPipelineScope::External,
                                            RenderGraphTextureLayout::ColorAttachment,
                                            {1}};
        const std::array initial{RenderGraphImportedState{resource, state}};
        std::vector final{RenderGraphFinalState{resource, state}};
        if (resourceClass == RenderGraphResourceClass::External) {
            SECTION("duplicate") {
                final.push_back(final.front());
            }
            SECTION("foreign resource") {
                final.front().resource.owner = {};
            }
            SECTION("unknown resource") {
                final.front().resource.value = 99;
            }
            SECTION("undefined required state") {
                final.front().state.access = RenderGraphSynchronizationAccess::None;
            }
            SECTION("missing external scope") {
                final.front().state.scope = RenderGraphPipelineScope::Graphics;
            }
            SECTION("invalid queue") {
                final.front().state.queue = {};
            }
        }
        const auto synchronization = SynthesizeRenderGraphSynchronization(graph, schedule, SharedQueue, initial, final);
        REQUIRE(synchronization.HasError());
        RequireError(synchronization, "render.graph.synchronization.external_release_invalid");
    }
}  // namespace Horo::Render
