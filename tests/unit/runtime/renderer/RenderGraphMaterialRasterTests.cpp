#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/RenderGraphExecutionErrors.h"
#include "MaterialBindingTestFixture.h"
#include "RenderGraphTestUtils.h"
#include "support/TypedIdentityTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <optional>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    constexpr RenderResourceOwnerId Owner{17};

    // This adapter owns metadata leases only; these tests make no native rendering claim.
    class MetadataMaterials final : public IMaterialBindingBackend {
        class Lease final : public IResidentMaterialBinding {};

    public:
        Result<std::unique_ptr<IResidentMaterialBinding>> Realize(const MaterialBindingDescriptor &) override {
            return Result<std::unique_ptr<IResidentMaterialBinding>>::Success(std::make_unique<Lease>());
        }
    };

    struct RasterFixture {
        MetadataMaterials backend;
        std::unique_ptr<MaterialBindingTable> materials;
        std::optional<CompiledRenderGraphExecution> graph;
        std::array<RenderGraphPassWorkload, 1> workloads;
        std::array<RenderGraphResourceInstance, 4> instances;
        std::array<RenderGraphMaterialDraw, 1> draws;
        std::array<RenderGraphMaterialRead, 1> reads;
        std::array<RenderGraphMeshInstance, 1> meshes{{{{Owner, 9, 1}, 90}}};
        std::array<RenderGraphTextureViewInstance, 1> views{{{{Owner, 8, 1}, 80}}};

        RasterFixture() {
            auto builder = Test::RequireBuilder({.maxPasses = 1, .maxResources = 4, .maxUsages = 4, .maxDependencies = 1});
            auto pass = Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
            const auto color = Test::RequireResource(builder.ImportTexture({Owner, 7, 1}, RenderGraphResourceClass::Persistent));
            Test::RequireUsage(builder, {pass, color, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment});
            std::array<RenderGraphResourceId, 3> buffers;
            const std::array kinds{RenderGraphUsageKind::VertexBuffer, RenderGraphUsageKind::IndexBuffer, RenderGraphUsageKind::Storage};
            std::array<RenderGraphImportedState, 4> initial;
            initial[0] = {color, {}, RenderGraphInitialContents::Discarded};
            instances[0] = {color, 70};
            for (std::size_t index = 0; index < buffers.size(); ++index) {
                buffers[index] = Test::RequireResource(
                    builder.ImportBuffer({Owner, static_cast<std::uint32_t>(index + 1), 1}, RenderGraphResourceClass::Persistent));
                Test::RequireUsage(builder, {pass, buffers[index], RenderGraphAccess::Read, kinds[index]});
                initial[index + 1] = {buffers[index],
                                      {RenderGraphSynchronizationAccess::Read,
                                       index == 2 ? RenderGraphSynchronizationOperation::Storage
                                                  : RenderGraphSynchronizationOperation::GraphicsInputBuffer,
                                       RenderGraphPipelineScope::Graphics,
                                       RenderGraphTextureLayout::NotApplicable,
                                       {1}}};
                instances[index + 1] = {buffers[index], index + 10};
            }
            auto authored = Test::RequireGraph(builder);
            auto schedule = Test::RequireSchedule(authored);
            const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
            auto sync = SynthesizeRenderGraphSynchronization(authored, schedule, queues, initial);
            REQUIRE(sync.HasValue());
            auto compiled = CompileRenderGraphExecution(authored, schedule, sync.Value(), queues);
            REQUIRE(compiled.HasValue());
            graph = std::move(compiled).Value();
            MakeMaterial();
            draws[0].mesh = meshes[0].mesh;
            draws[0].vertices = buffers[0];
            draws[0].indices = buffers[1];
            reads[0] = {buffers[2], {Owner, 3, 1}, {}};
            workloads[0] =
                {pass,
                 RenderGraphMaterialRaster{.color = color, .colorView = views[0].view, .extent = {64, 64}, .draws = draws, .reads = reads}};
        }

        void MakeMaterial() {
            auto table = MaterialBindingTable::Create(Owner, backend);
            REQUIRE(table.HasValue());
            materials = std::move(table).Value();
            auto descriptor = Tests::MaterialBindingFixture::Descriptor();
            descriptor.layout.parameters.clear();
            descriptor.layout.resources[0].kind = ShaderResourceKind::StorageBuffer;
            descriptor.resources[0].value = MaterialBufferBinding{{Owner, 3, 1}, 0, 64};
            auto published = materials->Publish(std::move(descriptor));
            REQUIRE(published.HasValue());
            auto lease = materials->Acquire(published.Value().binding);
            REQUIRE(lease.HasValue());
            draws[0].material = std::move(lease).Value();
        }

        Result<void> Validate() const {
            return ValidateRenderGraphExecutionRequest(
                {.frame = {1}, .graph = *graph, .workloads = workloads, .resources = instances, .meshes = meshes, .views = views});
        }

        RenderGraphMaterialRaster &Raster() {
            return std::get<RenderGraphMaterialRaster>(workloads[0].workload);
        }
    };
}  // namespace

TEST_CASE("Material graph raster consumes exact readonly storage and geometry declarations", "[renderer][material-graph]") {
    RasterFixture fixture;
    REQUIRE(fixture.Validate().HasValue());
    SECTION("missing graph input") {
        fixture.Raster().reads = {};
    }
    SECTION("wrong resident buffer generation") {
        ++fixture.reads[0].buffer.generation;
    }
    SECTION("swapped vertex and index uses") {
        std::swap(fixture.draws[0].vertices, fixture.draws[0].indices);
    }
    SECTION("unresolved mesh") {
        fixture.meshes[0].mesh = {};
    }
    SECTION("unresolved color view") {
        fixture.views[0].view = {};
    }
    SECTION("invalid region") {
        fixture.Raster().region = RenderMaterialRegion{63, 0, 2, 1};
    }
    SECTION("depth-only cannot retain color") {
        fixture.Raster().depthOnly = true;
    }
    SECTION("missing material lease") {
        fixture.draws[0].material.reset();
    }
    Tests::RequireActionableError(fixture.Validate(), RenderGraphExecutionErrors::InvalidGraph);
}
