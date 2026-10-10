#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "RenderGraphTestUtils.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <optional>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    class MetadataKernel final : public IResidentLightCullingKernel {};

    /** @brief Builds real synchronization and execution contracts without pretending to encode native work. */
    struct GraphFixture {
        std::optional<CompiledRenderGraphExecution> execution;
        std::array<RenderGraphPassWorkload, 1> workloads;
        std::array<RenderGraphResourceInstance, 4> instances;

        explicit GraphFixture(RenderPassKind kind = RenderPassKind::Compute) {
            auto builder = Test::RequireBuilder({.maxPasses = 1, .maxResources = 4, .maxUsages = 4, .maxDependencies = 1});
            const auto pass = Test::RequirePass(builder, kind, RenderQueueRole::Graphics);
            std::array<RenderGraphResourceId, 4> ids;
            std::array<RenderGraphImportedState, 4> initial;
            for (std::size_t index = 0; index < ids.size(); ++index) {
                ids[index] = Test::RequireResource(
                    builder.ImportBuffer(Test::BufferHandle(static_cast<std::uint32_t>(index + 1)), RenderGraphResourceClass::Persistent));
                Test::RequireUsage(builder, {pass, ids[index], index < 2 ? RenderGraphAccess::Read : RenderGraphAccess::Write,
                                             RenderGraphUsageKind::Storage});
                initial[index] = {ids[index],
                                  {RenderGraphSynchronizationAccess::ReadWrite,
                                   RenderGraphSynchronizationOperation::Storage,
                                   RenderGraphPipelineScope::Compute,
                                   RenderGraphTextureLayout::NotApplicable,
                                   {1}}};
                instances[index] = {ids[index], index + 1};
            }
            auto graph = Test::RequireGraph(builder);
            auto schedule = Test::RequireSchedule(graph);
            const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
            auto synchronization = SynthesizeRenderGraphSynchronization(graph, schedule, queues, initial);
            REQUIRE(synchronization.HasValue());
            auto compiled = CompileRenderGraphExecution(graph, schedule, synchronization.Value(), queues);
            REQUIRE(compiled.HasValue());
            execution = std::move(compiled).Value();
            workloads[0] = {pass,
                            RenderGraphLightCulling{ids[0], ids[1], ids[2], ids[3], {2, 1, 2, 0}, 1, std::make_shared<MetadataKernel>()}};
        }

        Result<void> Validate() const {
            return ValidateRenderGraphExecutionRequest({{1}, *execution, workloads, instances});
        }

        RenderGraphLightCulling &Culling() {
            return std::get<RenderGraphLightCulling>(workloads[0].workload);
        }
    };
}  // namespace

TEST_CASE("Light graph validation requires exact distinct storage uses and bounded dispatch", "[renderer][light-graph]") {
    GraphFixture fixture;
    REQUIRE(fixture.Validate().HasValue());
    SECTION("No kernel lease") {
        fixture.Culling().kernel.reset();
    }
    SECTION("Zero table revision") {
        fixture.Culling().tableRevision = 0;
    }
    SECTION("Reserved dispatch") {
        fixture.Culling().dispatch.reserved = 1;
    }
    SECTION("Zero clusters") {
        fixture.Culling().dispatch.clusterCount = 0;
    }
    SECTION("Unbounded lights") {
        fixture.Culling().dispatch.lightCount = LightCullingBudget::HardMaximumLights + 1;
    }
    SECTION("Aliased logical outputs") {
        fixture.Culling().references = fixture.Culling().membership;
    }
    SECTION("Swapped read and write") {
        std::swap(fixture.Culling().lights, fixture.Culling().references);
    }
    SECTION("Foreign logical owner") {
        fixture.Culling().lights.owner = {};
    }
    SECTION("Stale resource generation") {
        fixture.instances[0].resource = {};
    }
    SECTION("Reordered instances") {
        std::swap(fixture.instances[0], fixture.instances[1]);
    }
    CHECK(fixture.Validate().HasError());
}

TEST_CASE("Light graph workload is admitted only on a compute pass", "[renderer][light-graph]") {
    GraphFixture fixture(RenderPassKind::Graphics);
    CHECK(fixture.Validate().HasError());
}
