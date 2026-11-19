#include "Horo/Runtime/Render/RenderGraphSynchronization.h"
#include "Horo/Runtime/Render/UiPaintProgram.h"
#include "UiPaintProgramTestSupport.h"

#include <catch2/generators/catch_generators.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Tests::RenderFrontendTesting;
    using namespace Horo::Tests::UiPaintProgramTesting;
}  // namespace

TEST_CASE("Canvas surface paints an exact sampleable offscreen root through the existing UI graph",
          "[renderer][ui-paint][program][canvas-surface]") {
    ProgramFixture fixture;
    const auto geometry = fixture.Extract();
    const std::array textures{fixture.texture->Creation().handle};
    auto program = UiRequire(UiPaintProgram::CreateCanvasSurface(*fixture.frontend, {fixture.projection, {512, 768, 128}, 2, 32},
                                                                 geometry.SourceSnapshot(), textures, CookedUiPaintKernel{}));
    REQUIRE(fixture.frontend->ProcessResourceRequests().HasValue());
    REQUIRE(program->ValidatePreparation().HasValue());
    CHECK(lifecycleState.uiPaint.kernels == 1);

    std::array<UiRenderImageBinding, 1> bindings;
    auto submission = fixture.Submission(*program, geometry, bindings);
    const auto first = UiRequire(program->Prepare(0, submission));
    const auto root = UiRequire(program->CanvasSurface(first));
    CHECK(root.color != textures.front());
    CHECK((root.extent == FramebufferExtent{640, 480}));
    auto other = fixture.Submission(*program, geometry, bindings);
    bindings.front().texture = UiRequire(program->TextureResource(1, textures.front()));
    const auto second = UiRequire(program->Prepare(1, other));
    const auto secondRoot = UiRequire(program->CanvasSurface(second));
    CHECK(secondRoot.color != root.color);

    fixture.Encode(*program, geometry, first);
    const auto &passes = lifecycleState.uiPaint.passes;
    REQUIRE(passes.size() == 5);
    CHECK(passes.back().color.IsValid());
    CHECK(passes.back().colorTexture == root.color);
    CHECK(passes.back().clearColor);
    CHECK_FALSE(passes.back().draws.empty());
    for (const auto &pass : std::span{passes}.first(passes.size() - 1))
        CHECK(pass.colorTexture != root.color);

    const auto replacement = fixture.Extract();
    const auto current = UiRequire(program->Prepare(0, fixture.Submission(*program, replacement, bindings)));
    CHECK(program->CanvasSurface(first).HasError());
    CHECK(UiRequire(program->CanvasSurface(current)).color == root.color);
    CHECK(UiRequire(program->CanvasSurface(second)).color == secondRoot.color);

    auto ordinary = fixture.Create(replacement);
    const auto screen = UiRequire(ordinary->Prepare(0, fixture.Submission(*ordinary, replacement, bindings)));
    CHECK(ordinary->CanvasSurface(screen).HasError());
    CHECK(program->CanvasSurface(screen).HasError());
    REQUIRE(program->Shutdown().HasValue());
    CHECK(program->CanvasSurface(current).HasError());
    CHECK(program->CanvasSurface(second).HasError());
    const auto retired = fixture.frontend->ResourceState(root.color);
    CHECK((retired.HasError() || retired.Value() != RenderResourceState::Ready));
}

TEST_CASE("Scene graph consumes the exact prepared sampleable canvas in one frame lease", "[renderer][ui-paint][program][canvas-surface]") {
    const bool explicitSubmissions = GENERATE(false, true);
    ProgramFixture fixture{explicitSubmissions};
    const auto geometry = fixture.Extract();
    const std::array textures{fixture.texture->Creation().handle};
    constexpr FramebufferExtent extent{640, 480};
    constexpr RenderQueueId queue{1};
    const std::array bufferDescriptors{RenderBufferDescriptor{sizeof(UiPaintVertex) * 3, RenderBufferUsage::Vertex},
                                       RenderBufferDescriptor{sizeof(std::uint32_t) * 3, RenderBufferUsage::Index},
                                       RenderBufferDescriptor{256, RenderBufferUsage::Uniform}};
    const std::array usages{RenderGraphUsageKind::VertexBuffer, RenderGraphUsageKind::IndexBuffer, RenderGraphUsageKind::UniformBuffer};
    std::array<RenderBufferHandle, 3> handles;
    for (std::size_t index = 0; index < handles.size(); ++index)
        handles[index] = UiRequire(fixture.frontend->CreateBuffer(bufferDescriptors[index], {})).handle;

    struct ConsumingPass final {
        RenderGraphPassRef pass;
        std::array<RenderGraphResourceId, 3> buffers;
        std::array<RenderGraphResourceId, 1> sampled;
        RenderTextureHandle canvas;
    };

    std::array<ConsumingPass, 2> consumers{};
    bool declareSample = true;
    const UiPaintSceneCanvasGraph recipe{{16, 32, 128, 32},
                                         queue,
                                         [&](RenderGraphBuilder &builder, const UiPaintSceneCanvasSource &canvas,
                                             std::vector<RenderGraphImportedState> &initial) -> Result<RenderGraphPassRef> {
        CHECK(canvas.surface.extent == extent);
        auto &consumer = consumers.at(canvas.slot);
        consumer.canvas = canvas.surface.color;
        consumer.pass = UiRequire(builder.AddPass(RenderPassKind::Graphics, RenderQueueRole::Graphics));
        consumer.sampled = {canvas.color};
        if (declareSample)
            REQUIRE(builder.AddUsage({consumer.pass, canvas.color, RenderGraphAccess::Read, RenderGraphUsageKind::Sampled}).HasValue());
        for (std::size_t index = 0; index < handles.size(); ++index) {
            const auto id = UiRequire(builder.ImportBuffer(handles[index], RenderGraphResourceClass::Persistent));
            consumer.buffers[index] = id;
            REQUIRE(builder.AddUsage({consumer.pass, id, RenderGraphAccess::Read, usages[index]}).HasValue());
            initial.push_back({id,
                               {RenderGraphSynchronizationAccess::Read, RenderGraphSynchronizationOperation::GraphicsInputBuffer,
                                RenderGraphPipelineScope::Graphics, RenderGraphTextureLayout::NotApplicable, queue}});
        }
        return Result<RenderGraphPassRef>::Success(consumer.pass);
    }};
    const UiPaintProgramDescriptor descriptor{fixture.projection, {512, 768, 128}, 2, 32};
    SECTION("a current exact slot executes with its sampled consumer") {}
    SECTION("an unconsumed candidate never publishes") {
        declareSample = false;
        CHECK(UiPaintProgram::CreateSceneCanvas(*fixture.frontend, descriptor, geometry.SourceSnapshot(), textures, CookedUiPaintKernel{},
                                                recipe)
                  .HasError());
        CHECK(lifecycleState.presentCount == 0);
        return;
    }

    auto program = UiRequire(UiPaintProgram::CreateSceneCanvas(*fixture.frontend, descriptor, geometry.SourceSnapshot(), textures,
                                                               CookedUiPaintKernel{}, recipe));
    auto kernel = UiRequire(fixture.frontend->RealizeUiPaintKernel({}));
    REQUIRE(fixture.frontend->ProcessResourceRequests().HasValue());
    REQUIRE(program->ValidatePreparation().HasValue());
    REQUIRE(program->Inventory().passes == 6);
    REQUIRE(consumers[0].canvas != consumers[1].canvas);
    std::array<UiRenderImageBinding, 1> bindings;
    const auto stale = UiRequire(program->Prepare(0, fixture.Submission(*program, geometry, bindings)));
    const auto prepared = UiRequire(program->Prepare(0, fixture.Submission(*program, geometry, bindings)));
    CHECK(UiRequire(program->CanvasSurface(prepared)).color == consumers[0].canvas);
    const auto &consumer = consumers.front();
    constexpr std::array draws{UiPaintDraw{.indexCount = 3,
                                           .scissor = {0, 0, extent.width, extent.height},
                                           .sampling = UiPaintSampling::Premultiplied,
                                           .sampled = 0}};
    const std::array workloads{RenderGraphPassWorkload{consumer.pass, RenderGraphUiPaint{.vertices = consumer.buffers[0],
                                                                                         .indices = consumer.buffers[1],
                                                                                         .parameters = consumer.buffers[2],
                                                                                         .sampledResources = consumer.sampled,
                                                                                         .extent = extent,
                                                                                         .vertexCount = 3,
                                                                                         .indexCount = 3,
                                                                                         .draws = draws,
                                                                                         .kernel = std::move(kernel),
                                                                                         .frameRevision = 1}}};
    auto frame = UiRequire(fixture.frontend->BeginFrame({.frameNumber = fixture.frameNumber++, .outputExtent = extent}));
    CHECK(program->Submit(frame, prepared, fixture.Submission(*program, geometry, bindings)).HasError());
    CHECK(program->ExecuteSceneCanvas(frame, stale, fixture.Submission(*program, geometry, bindings), workloads).HasError());
    auto foreign = workloads;
    foreign.front().pass = consumers[1].pass;
    CHECK(program->ExecuteSceneCanvas(frame, prepared, fixture.Submission(*program, geometry, bindings), foreign).HasError());
    CHECK(lifecycleState.graphResources.empty());
    CHECK(lifecycleState.reconstruction.recorded.empty());
    REQUIRE(program->ExecuteSceneCanvas(frame, prepared, fixture.Submission(*program, geometry, bindings), workloads).HasValue());
    CHECK(program->ExecuteSceneCanvas(frame, prepared, fixture.Submission(*program, geometry, bindings), workloads).HasError());
    CHECK(lifecycleState.executeCount == 1);
    if (explicitSubmissions) {
        REQUIRE(lifecycleState.reconstruction.recorded.size() == 1);
        const auto signal = lifecycleState.reconstruction.recorded.front();
        CHECK(signal.IsValid());
        CHECK(signal.queue == queue);
        CHECK(lifecycleState.graphLease != nullptr);
    } else
        CHECK(lifecycleState.graphResources.size() == program->Inventory().resources);
    REQUIRE(frame.Present().HasValue());
    CHECK(lifecycleState.presentCount == 1);
    REQUIRE(program->Shutdown().HasValue());
    CHECK(program->CanvasSurface(prepared).HasError());
}
