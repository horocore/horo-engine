#include "Horo/Runtime/Render/RenderGraphWorkload.h"
#include "Horo/Runtime/Render/UiPaintKernel.h"
#include "RenderGraphTestUtils.h"

#include <catch2/catch_test_macros.hpp>
#include <optional>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    class MetadataKernel final : public IResidentUiPaintKernel {};

    /** @brief Real compiled pass/table admission only; no native raster or pixel evidence is manufactured. */
    struct PaintGraph final {
        std::optional<CompiledRenderGraphExecution> execution;
        std::array<RenderGraphPassWorkload, 1> workloads;
        std::array<RenderGraphResourceInstance, 5> instances;
        std::array<RenderGraphResourceId, 1> sampled;
        std::array<UiPaintDraw, 1> draws{UiPaintDraw{0, 3, {0, 0, 640, 480}}};

        PaintGraph() {
            auto builder = Test::RequireBuilder({.maxPasses = 1, .maxResources = 5, .maxUsages = 5, .maxDependencies = 1});
            const auto pass = Test::RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
            std::array<RenderGraphResourceId, 5> ids;
            std::array<RenderGraphImportedState, 5> initial;
            constexpr std::array kinds{RenderGraphUsageKind::VertexBuffer, RenderGraphUsageKind::IndexBuffer,
                                       RenderGraphUsageKind::UniformBuffer, RenderGraphUsageKind::ColorAttachment,
                                       RenderGraphUsageKind::Sampled};
            for (std::size_t index = 0; index < ids.size(); ++index) {
                ids[index] =
                    index < 3
                        ? Test::RequireResource(builder.ImportBuffer(Test::BufferHandle(index + 1), RenderGraphResourceClass::Persistent))
                        : Test::RequireResource(
                              builder.ImportTexture(Test::TextureHandle(index + 1), RenderGraphResourceClass::Persistent));
                Test::RequireUsage(builder,
                                   {pass, ids[index], index == 3 ? RenderGraphAccess::ReadWrite : RenderGraphAccess::Read, kinds[index]});
                const auto operation = index < 3    ? RenderGraphSynchronizationOperation::GraphicsInputBuffer
                                       : index == 3 ? RenderGraphSynchronizationOperation::ColorAttachment
                                                    : RenderGraphSynchronizationOperation::Sampled;
                const auto layout = index < 3    ? RenderGraphTextureLayout::NotApplicable
                                    : index == 3 ? RenderGraphTextureLayout::ColorAttachment
                                                 : RenderGraphTextureLayout::ShaderReadOnly;
                initial[index] = {ids[index],
                                  {index == 3 ? RenderGraphSynchronizationAccess::ReadWrite : RenderGraphSynchronizationAccess::Read,
                                   operation,
                                   RenderGraphPipelineScope::Graphics,
                                   layout,
                                   {1}}};
                instances[index] = {ids[index], index + 1};
            }
            Compile(builder, initial);
            sampled[0] = ids[4];
            workloads[0] = {pass, RenderGraphUiPaint{.color = ids[3],
                                                     .vertices = ids[0],
                                                     .indices = ids[1],
                                                     .parameters = ids[2],
                                                     .sampledResources = sampled,
                                                     .extent = {640, 480},
                                                     .vertexCount = 3,
                                                     .indexCount = 3,
                                                     .draws = draws,
                                                     .kernel = std::make_shared<MetadataKernel>(),
                                                     .frameRevision = 1}};
        }

        /** @brief Finalizes the actual graph, schedule and synchronization before workload admission. */
        void Compile(RenderGraphBuilder &builder, std::span<const RenderGraphImportedState> initial) {
            auto graph = Test::RequireGraph(builder);
            auto schedule = Test::RequireSchedule(graph);
            const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
            auto synchronization = SynthesizeRenderGraphSynchronization(graph, schedule, queues, initial);
            REQUIRE(synchronization.HasValue());
            auto compiled = CompileRenderGraphExecution(graph, schedule, synchronization.Value(), queues);
            REQUIRE(compiled.HasValue());
            execution = std::move(compiled).Value();
        }

        RenderGraphUiPaint &Paint() {
            return std::get<RenderGraphUiPaint>(workloads[0].workload);
        }

        Result<void> Validate() const {
            return ValidateRenderGraphExecutionRequest({.frame = {1},
                                                        .graph = *execution,
                                                        .workloads = workloads,
                                                        .resources = instances,
                                                        .role = RenderGraphExecutionRole::UiComposition});
        }
    };
}  // namespace

TEST_CASE("UI immutable sampled table admits dynamic source coverage and hidden contents", "[renderer][ui-paint][graph]") {
    PaintGraph fixture;
    REQUIRE(fixture.Validate().HasValue());
    SECTION("Active alpha source") {
        fixture.draws[0].sampling = UiPaintSampling::Alpha;
        fixture.draws[0].sampled = 0;
    }
    SECTION("Independent minification magnification and mip selection") {
        fixture.draws[0].filter = RenderSamplerFilter::Nearest;
        fixture.draws[0].magFilter = RenderSamplerFilter::Linear;
        fixture.draws[0].mipmap = UiPaintMipmap::Linear;
    }
    SECTION("Complete ancestor multiplication") {
        fixture.draws[0].operation = UiPaintOperation::MultiplyCoverage;
        fixture.draws[0].sampling = UiPaintSampling::Alpha;
        fixture.draws[0].sampled = 0;
    }
    SECTION("Fractional mask coverage") {
        fixture.draws[0].coverage = 0;
    }
    SECTION("Ordinary visibility uses retained pass structure") {
        fixture.Paint().draws = {};
        fixture.Paint().activity = UiPaintPassActivity::Hidden;
    }
    REQUIRE(fixture.Validate().HasValue());
}

TEST_CASE("UI immutable sampled table rejects undeclared malformed or mismatched selections", "[renderer][ui-paint][graph]") {
    PaintGraph fixture;
    REQUIRE(fixture.Validate().HasValue());
    SECTION("Read table cannot disappear while graph reads remain declared") {
        fixture.Paint().sampledResources = {};
    }
    SECTION("Foreign table owner") {
        fixture.sampled[0].owner = {};
    }
    SECTION("Color output cannot be sampled in the same pass") {
        fixture.sampled[0] = fixture.Paint().color;
    }
    SECTION("Mipmap selection is closed") {
        fixture.draws[0].mipmap = static_cast<UiPaintMipmap>(3);
    }
    SECTION("Source index is bounded") {
        fixture.draws[0].sampling = UiPaintSampling::Color;
        fixture.draws[0].sampled = 1;
    }
    SECTION("Coverage index is bounded") {
        fixture.draws[0].coverage = 1;
    }
    SECTION("Hidden pass cannot contain live draws") {
        fixture.Paint().activity = UiPaintPassActivity::Hidden;
    }
    SECTION("Empty active pass is not implicit success") {
        fixture.Paint().draws = {};
    }
    SECTION("Mask local bounds must remain finite") {
        fixture.draws[0].maskExtent = {0, 1};
    }
    REQUIRE(fixture.Validate().HasError());
}
