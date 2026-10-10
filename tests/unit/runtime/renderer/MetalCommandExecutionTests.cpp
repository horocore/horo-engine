#include "MetalRenderTestSupport.h"
#include "RenderGraphTestUtils.h"
#include "RenderTransientGraphTestSupport.h"
#include "runtime/renderer/modules/metal/MetalCommandExecution.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Render::MetalBackendTests;

    CompiledRenderGraphExecution CompileExecution(RenderGraphBuilder &builder, const std::span<const RenderQueueAssignment> queues,
                                                  const std::span<const RenderGraphImportedState> states = {}) {
        RenderGraph graph = Test::RequireGraph(builder);
        RenderGraphSchedule schedule = Test::RequireSchedule(graph);
        auto synchronization = SynthesizeRenderGraphSynchronization(graph, schedule, queues, states);
        REQUIRE(synchronization.HasValue());
        auto execution = CompileRenderGraphExecution(graph, schedule, synchronization.Value(), queues);
        REQUIRE(execution.HasValue());
        return std::move(execution).Value();
    }

    TEST_CASE("Metal graph workloads preserve compiled ordering and reject mismatched bindings before encoding",
              "[unit][runtime][renderer][metal]") {
        auto builder = Test::RequireBuilder();
        const auto first = Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
        const auto second = Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
        auto graph = CompileExecution(builder, queues);
        const PrimaryOutputAttachment red{.clearColor = {1, 0, 0, 1}};
        const PrimaryOutputAttachment green{.clearColor = {0, 1, 0, 1}};
        std::array bindings{RenderGraphPassWorkload{first, red}, RenderGraphPassWorkload{second, green}};
        PortState state;
        FakePresentationPort port{state};
        FakeMetalRuntime runtime{port, state};
        REQUIRE(runtime.BeginFrame({64, 64}).HasValue());

        std::swap(bindings[0], bindings[1]);
        REQUIRE(Detail::ExecuteMetalRenderGraph(runtime, {{1}, graph, bindings, {}}).HasError());
        REQUIRE(state.executeCount == 0);
        REQUIRE(state.frameActive);
        std::swap(bindings[0], bindings[1]);
        REQUIRE(Detail::ExecuteMetalRenderGraph(runtime, {{1}, graph, std::span{bindings}.first(1), {}}).HasError());
        REQUIRE(state.executeCount == 0);
        REQUIRE(Detail::ExecuteMetalRenderGraph(runtime, {{1}, graph, bindings, {}}).HasValue());
        REQUIRE(state.executeCount == 2);
        REQUIRE(state.attachment.clearColor.green == 1);
    }

    TEST_CASE("Metal graph encoding failures abort partial frames and preserve native diagnostics", "[unit][runtime][renderer][metal]") {
        auto builder = Test::RequireBuilder();
        const auto pass = Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
        auto graph = CompileExecution(builder, queues);
        const std::array bindings{RenderGraphPassWorkload{pass, PrimaryOutputAttachment{}}};
        PortState state{.failure = PortFailure::Execute};
        FakePresentationPort port{state};
        FakeMetalRuntime runtime{port, state};
        REQUIRE(runtime.BeginFrame({64, 64}).HasValue());
        const auto result = Detail::ExecuteMetalRenderGraph(runtime, {{1}, graph, bindings, {}});
        Test::RequireError(result, "render.test.execute_failed");
        REQUIRE(state.abortCount == 1);
        REQUIRE_FALSE(state.frameActive);
    }

    TEST_CASE("Metal graph admission rejects unsupported workloads before encoding a valid prefix", "[unit][runtime][renderer][metal]") {
        auto builder = Test::RequireBuilder();
        const auto first = Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
        const auto second = Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
        auto graph = CompileExecution(builder, queues);
        std::array bindings{RenderGraphPassWorkload{first, PrimaryOutputAttachment{}},
                            RenderGraphPassWorkload{second, PrimaryOutputAttachment{}}};
        SECTION("duplicate pass bindings") {
            bindings[1].pass = bindings[0].pass;
        }
        SECTION("invalid attachment operation") {
            std::get<PrimaryOutputAttachment>(bindings[1].workload).loadOperation = static_cast<AttachmentLoadOperation>(255);
        }
        SECTION("unbound graph color workload") {
            bindings[1].workload = RenderGraphColorAttachment{};
        }
        SECTION("copy workload on graphics pass") {
            bindings[1].workload = RenderGraphBufferCopy{};
        }
        PortState state;
        FakePresentationPort port{state};
        FakeMetalRuntime runtime{port, state};
        REQUIRE(Detail::ExecuteMetalRenderGraph(runtime, {{1}, graph, bindings, {}}).HasError());
        REQUIRE(state.executeCount == 0);
    }

    TEST_CASE("Metal resource graphs on one effective queue still require resource workload bindings", "[unit][runtime][renderer][metal]") {
        auto builder = Test::RequireBuilder();
        const auto write = Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
        const auto resource = Test::RequireResource(builder.AddTransientResource(RenderGraphResourceKind::Buffer));
        Test::RequireUsage(builder, {write, resource, RenderGraphAccess::Write, RenderGraphUsageKind::Storage});
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
        auto graph = CompileExecution(builder, queues);
        const std::array bindings{RenderGraphPassWorkload{write, PrimaryOutputAttachment{}}};
        PortState state;
        FakePresentationPort port{state};
        FakeMetalRuntime runtime{port, state};
        const std::array instances{RenderGraphResourceInstance{resource, 1}};
        Test::RequireError(Detail::ExecuteMetalRenderGraph(runtime, {{1}, graph, bindings, instances}),
                           "render.metal.unsupported_graph_execution");
        REQUIRE(state.executeCount == 0);
    }

    TEST_CASE("Metal graph resource and ownership operations fail without inventing submission bindings",
              "[unit][runtime][renderer][metal]") {
        auto builder = Test::RequireBuilder();
        const auto write = Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
        const auto read = Test::RequirePass(builder, RenderPassKind::Compute, RenderQueueRole::Compute);
        const auto resource = Test::RequireResource(builder.AddTransientResource(RenderGraphResourceKind::Buffer));
        Test::RequireUsage(builder, {write, resource, RenderGraphAccess::Write, RenderGraphUsageKind::Storage});
        Test::RequireUsage(builder, {read, resource, RenderGraphAccess::Read, RenderGraphUsageKind::Storage});
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}},
                                RenderQueueAssignment{RenderQueueRole::Compute, {2}}};
        auto graph = CompileExecution(builder, queues);
        const std::array bindings{RenderGraphPassWorkload{write, std::monostate{}}, RenderGraphPassWorkload{read, std::monostate{}}};
        PortState state;
        FakePresentationPort port{state};
        FakeMetalRuntime runtime{port, state};
        REQUIRE_FALSE(graph.AcquireTransfers().empty());
        const std::array instances{RenderGraphResourceInstance{resource, 1}};
        Test::RequireError(Detail::ExecuteMetalRenderGraph(runtime, {{1}, graph, bindings, instances}),
                           "render.metal.unsupported_graph_execution");
        REQUIRE(state.executeCount == 0);
    }

    class GraphRuntime final : public FakeMetalRuntime {
    public:
        using FakeMetalRuntime::FakeMetalRuntime;
        mutable std::size_t validationCount{0};
        std::size_t encodingCount{0};
        std::uint64_t invalidInstance{0};
        std::size_t failAt{0};

        Result<void> ValidateGraphWorkload(const RenderGraphWorkload &,
                                           const std::span<const RenderGraphResourceInstance> resources) const override {
            ++validationCount;
            for (const auto &resource : resources) {
                if (resource.instance == invalidInstance) {
                    return Result<void>::Failure(MakePortError("render.test.resource_stale", "Injected stale resident generation."));
                }
            }
            return Result<void>::Success();
        }

        Result<void> ExecuteGraphWorkload(const RenderGraphWorkload &, const std::span<const RenderGraphResourceInstance>) override {
            ++encodingCount;
            if (encodingCount == failAt) {
                return Result<void>::Failure(MakePortError("render.test.graph_encode_failed", "Injected graph encode failure."));
            }
            return Result<void>::Success();
        }
    };

    TEST_CASE("Metal admitted transient color aliases preserve native validation and canonical encoding", "[renderer][metal][transient]") {
        auto sources = TransientTest::ColorGraph();
        const auto resources = sources.execution.Resources();
        REQUIRE(resources.size() == 2);
        const std::array instances{RenderGraphResourceInstance{resources[0].id, 41}, RenderGraphResourceInstance{resources[1].id, 41}};
        PortState state;
        FakePresentationPort port{state};
        GraphRuntime runtime{port, state};
        REQUIRE(runtime.BeginFrame({2, 2}).HasValue());
        SECTION("frontend admission is mandatory") {
            Test::RequireError(Detail::ExecuteMetalRenderGraph(runtime, {{1}, sources.execution, sources.workloads, instances}),
                               "render.metal.unsupported_graph_execution");
            CHECK(runtime.validationCount == 0);
            CHECK(runtime.encodingCount == 0);
        }
        SECTION("native instance validation precedes every encode") {
            runtime.invalidInstance = 41;
            Test::RequireError(Detail::ExecuteMetalRenderGraph(runtime,
                                                               {{1}, sources.execution, sources.workloads, instances, nullptr, true}),
                               "render.test.resource_stale");
            CHECK(runtime.encodingCount == 0);
        }
        SECTION("one actual backing supports two ordered attachment operations") {
            REQUIRE(
                Detail::ExecuteMetalRenderGraph(runtime, {{1}, sources.execution, sources.workloads, instances, nullptr, true}).HasValue());
            CHECK(runtime.validationCount == 2);
            CHECK(runtime.encodingCount == 2);
        }
        SECTION("partial encoder failure discards the unsent Metal frame") {
            runtime.failAt = 2;
            Test::RequireError(Detail::ExecuteMetalRenderGraph(runtime,
                                                               {{1}, sources.execution, sources.workloads, instances, nullptr, true}),
                               "render.test.graph_encode_failed");
            CHECK(runtime.encodingCount == 2);
            CHECK(state.abortCount == 1);
            CHECK_FALSE(state.frameActive);
        }
    }

    /** @brief Builds the write/read-write color dependency used by native workload validation scenarios. */
    CompiledRenderGraphExecution CompileColorHazards(RenderGraphPassRef &first, RenderGraphPassRef &second,
                                                     RenderGraphResourceId &texture) {
        auto builder = Test::RequireBuilder();
        first = Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
        second = Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
        texture = Test::RequireResource(builder.ImportTexture(Test::TextureHandle(), RenderGraphResourceClass::Persistent));
        Test::RequireUsage(builder, {first, texture, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment});
        Test::RequireUsage(builder, {second, texture, RenderGraphAccess::ReadWrite, RenderGraphUsageKind::ColorAttachment});
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
        const std::array initial{RenderGraphImportedState{texture,
                                                          {RenderGraphSynchronizationAccess::Write,
                                                           RenderGraphSynchronizationOperation::ColorAttachment,
                                                           RenderGraphPipelineScope::Graphics,
                                                           RenderGraphTextureLayout::ColorAttachment,
                                                           {1}}}};
        return CompileExecution(builder, queues, initial);
    }

    TEST_CASE("Metal resident graph hazards execute all validated color operations in compiled order", "[unit][runtime][renderer][metal]") {
        RenderGraphPassRef first;
        RenderGraphPassRef second;
        RenderGraphResourceId texture;
        auto graph = CompileColorHazards(first, second, texture);
        REQUIRE_FALSE(graph.Transitions().empty());
        const std::array instances{RenderGraphResourceInstance{texture, 41}};
        std::array bindings{RenderGraphPassWorkload{first, RenderGraphColorAttachment{texture, {}}},
                            RenderGraphPassWorkload{second,
                                                    RenderGraphColorAttachment{texture, {.loadOperation = AttachmentLoadOperation::Load}}}};
        PortState state;
        FakePresentationPort port{state};
        GraphRuntime runtime{port, state};
        REQUIRE(runtime.BeginFrame({64, 64}).HasValue());

        SECTION("successful hazard chain") {
            REQUIRE(Detail::ExecuteMetalRenderGraph(runtime, {{1}, graph, bindings, instances}).HasValue());
            REQUIRE(runtime.validationCount == 2);
            REQUIRE(runtime.encodingCount == 2);
        }
        SECTION("unresolved generation rejects whole frame before encoding") {
            runtime.invalidInstance = 41;
            Test::RequireError(Detail::ExecuteMetalRenderGraph(runtime, {{1}, graph, bindings, instances}), "render.test.resource_stale");
            REQUIRE(runtime.encodingCount == 0);
        }
        SECTION("undefined write cannot initialize a later read") {
            std::get<RenderGraphColorAttachment>(bindings[0].workload).operations.loadOperation = AttachmentLoadOperation::DontCare;
            REQUIRE(Detail::ExecuteMetalRenderGraph(runtime, {{1}, graph, bindings, instances}).HasError());
            REQUIRE(runtime.encodingCount == 0);
        }
        SECTION("discarded write cannot initialize a later read") {
            std::get<RenderGraphColorAttachment>(bindings[0].workload).operations.storeOperation = AttachmentStoreOperation::DontCare;
            REQUIRE(Detail::ExecuteMetalRenderGraph(runtime, {{1}, graph, bindings, instances}).HasError());
            REQUIRE(runtime.encodingCount == 0);
        }
        SECTION("partial native failure aborts") {
            runtime.failAt = 2;
            Test::RequireError(Detail::ExecuteMetalRenderGraph(runtime, {{1}, graph, bindings, instances}),
                               "render.test.graph_encode_failed");
            REQUIRE(runtime.validationCount == 2);
            REQUIRE(runtime.encodingCount == 2);
            REQUIRE(state.abortCount == 1);
            REQUIRE_FALSE(state.frameActive);
        }
    }

    TEST_CASE("Metal buffer copy matches both compiled source and destination declarations", "[unit][runtime][renderer][metal]") {
        auto builder = Test::RequireBuilder();
        const auto pass = Test::RequirePass(builder, RenderPassKind::Copy, RenderQueueRole::Transfer);
        const auto source = Test::RequireResource(builder.ImportBuffer(Test::BufferHandle(1), RenderGraphResourceClass::Persistent));
        const auto destination = Test::RequireResource(builder.ImportBuffer(Test::BufferHandle(2), RenderGraphResourceClass::Persistent));
        Test::RequireUsage(builder, {pass, source, RenderGraphAccess::Read, RenderGraphUsageKind::CopySource});
        Test::RequireUsage(builder, {pass, destination, RenderGraphAccess::Write, RenderGraphUsageKind::CopyDestination});
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Transfer, {1}}};
        const RenderGraphLogicalState state{RenderGraphSynchronizationAccess::Read,
                                            RenderGraphSynchronizationOperation::CopySource,
                                            RenderGraphPipelineScope::Transfer,
                                            RenderGraphTextureLayout::NotApplicable,
                                            {1}};
        const std::array initial{RenderGraphImportedState{source, state}, RenderGraphImportedState{destination, state}};
        auto graph = CompileExecution(builder, queues, initial);
        const std::array instances{RenderGraphResourceInstance{source, 1}, RenderGraphResourceInstance{destination, 2}};
        std::array bindings{RenderGraphPassWorkload{pass, RenderGraphBufferCopy{source, destination, 0, 8, 16}}};
        PortState portState;
        FakePresentationPort port{portState};
        GraphRuntime runtime{port, portState};
        SECTION("valid copy") {
            REQUIRE(Detail::ExecuteMetalRenderGraph(runtime, {{1}, graph, bindings, instances}).HasValue());
            REQUIRE(runtime.encodingCount == 1);
        }
        SECTION("zero byte copy") {
            std::get<RenderGraphBufferCopy>(bindings[0].workload).byteCount = 0;
            REQUIRE(Detail::ExecuteMetalRenderGraph(runtime, {{1}, graph, bindings, instances}).HasError());
            REQUIRE(runtime.encodingCount == 0);
        }
        SECTION("swapped resource operations") {
            std::swap(std::get<RenderGraphBufferCopy>(bindings[0].workload).source,
                      std::get<RenderGraphBufferCopy>(bindings[0].workload).destination);
            REQUIRE(Detail::ExecuteMetalRenderGraph(runtime, {{1}, graph, bindings, instances}).HasError());
            REQUIRE(runtime.encodingCount == 0);
        }
        SECTION("resource bindings cannot be reordered") {
            const std::array reordered{instances[1], instances[0]};
            REQUIRE(Detail::ExecuteMetalRenderGraph(runtime, {{1}, graph, bindings, reordered}).HasError());
            REQUIRE(runtime.encodingCount == 0);
        }
    }

    TEST_CASE("Metal backend encoding failures invalidate the frame token before presentation", "[unit][runtime][renderer][metal]") {
        PortState state;
        FakePresentationPort port{state};
        MetalEditorGraphicsBridge bridge;
        auto backend = CreateInitializedBackend(port, state, bridge);
        const auto frame = backend->BeginFrame({1, {64, 64}}).Value();
        auto builder = Test::RequireBuilder();
        const auto pass = Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
        const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
        auto graph = CompileExecution(builder, queues);
        const std::array workloads{RenderGraphPassWorkload{pass, PrimaryOutputAttachment{}}};
        state.failure = PortFailure::Execute;
        Test::RequireError(backend->ExecuteGraph({frame, graph, workloads, {}}), "render.test.execute_failed");
        REQUIRE(state.abortCount == 1);
        REQUIRE_FALSE(state.frameActive);
        REQUIRE(backend->Present(frame).HasError());
        REQUIRE(state.presentCount == 0);
        state.failure = PortFailure::None;
        REQUIRE(backend->BeginFrame({2, {64, 64}}).HasValue());
        backend->Shutdown();
    }
}  // namespace
