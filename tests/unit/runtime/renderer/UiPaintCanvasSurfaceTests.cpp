#include "Horo/Runtime/Render/RenderGraphSynchronization.h"
#include "Horo/Runtime/Render/UiPaintProgram.h"
#include "RenderExternalImages.h"
#include "RenderFrontendResourceAccess.h"
#include "RenderGraphTemplate.h"
#include "UiPaintProgramTestSupport.h"

#include <catch2/generators/catch_generators.hpp>
#include <optional>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Tests::RenderFrontendTesting;
    using namespace Horo::Tests::UiPaintProgramTesting;

    /** @brief Acquired attachment with the fake frontend's exact graphics state and device. */
    class CanvasAcquisition final : public Detail::IRenderExternalTextureSource {
    public:
        explicit CanvasAcquisition(std::uint64_t device) : device_(device) {}

        Result<void> Validate() const override {
            return Result<void>::Success();
        }

        std::uint64_t DeviceIncarnation() const noexcept override {
            return device_;
        }

        const RenderTextureDescriptor &Descriptor() const noexcept override {
            return descriptor_;
        }

        RenderGraphLogicalState AcquiredState() const noexcept override {
            return {RenderGraphSynchronizationAccess::ReadWrite,
                    RenderGraphSynchronizationOperation::ColorAttachment,
                    RenderGraphPipelineScope::Graphics,
                    RenderGraphTextureLayout::ColorAttachment,
                    {1}};
        }

        RenderGraphLogicalState RequiredFinalState() const noexcept override {
            auto released = AcquiredState();
            released.scope = RenderGraphPipelineScope::External;
            return released;
        }

    private:
        std::uint64_t device_;
        RenderTextureDescriptor descriptor_{.extent = {640, 480}, .usage = RenderTextureUsage::RenderAttachment};
    };
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
    const bool neutralRequired = GENERATE(false, true);
    const bool externalOutput = GENERATE(false, true);
    ProgramFixture fixture{explicitSubmissions, !neutralRequired};
    lifecycleState.uiPaint.requireGraphNeutralSample = neutralRequired;
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
        RenderGraphResourceId output;
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

    const auto acquired = std::make_shared<CanvasAcquisition>(fixture.frontend->Capabilities().support.deviceIncarnation);
    const Detail::UiPaintSceneCanvasRecipe externalRecipe{recipe.limits, recipe.graphicsQueue,
                                                          [&recipe, &acquired, &consumers](auto &builder, const auto &source, auto &initial,
                                                                                           auto &slots, auto &) {
        auto consumer = recipe.author(builder, source, initial);
        auto slot =
            UiRequire(Detail::PreparedRenderGraphTemplate::DeclareTexture(builder, acquired->Descriptor(), acquired->AcquiredState(),
                                                                          acquired->RequiredFinalState(), acquired->DeviceIncarnation()));
        consumers[source.slot].output = slot.resource;
        slots.push_back(slot);
        REQUIRE(builder.AddUsage({consumer.Value(), slot.resource, RenderGraphAccess::Write, RenderGraphUsageKind::ColorAttachment})
                    .HasValue());
        return consumer;
    }};
    auto program = UiRequire(
        externalOutput ? Detail::RenderFrontendResourceAccess::CreateSceneCanvas(*fixture.frontend, descriptor, geometry.SourceSnapshot(),
                                                                                 textures, {}, externalRecipe)
                       : UiPaintProgram::CreateSceneCanvas(*fixture.frontend, descriptor, geometry.SourceSnapshot(), textures, {}, recipe));
    CHECK(fixture.frontend->Capabilities().support.features.Supports(RenderCapability::UiPaint) == !neutralRequired);
    if (neutralRequired)
        CHECK(UiPaintFrameBufferPool::Create(*fixture.frontend, descriptor.capacity, descriptor.frameSlots).HasError());
    lifecycleState.uiPaint.requireGraphNeutralSample = false;
    auto kernel = UiRequire(fixture.frontend->RealizeUiPaintKernel({}));
    REQUIRE(fixture.frontend->ProcessResourceRequests().HasValue());
    REQUIRE(program->ValidatePreparation().HasValue());
    REQUIRE(program->Inventory().passes == (neutralRequired ? 7 : 6));
    REQUIRE(consumers[0].canvas != consumers[1].canvas);
    std::array<UiRenderImageBinding, 1> bindings;
    std::array<Detail::RenderGraphExternalTextureBinding, 1> acquiredBindings{};
    if (externalOutput) {
        CHECK(program->Prepare(0, fixture.Submission(*program, geometry, bindings)).HasError());
        const auto output = UiRequire(Detail::RenderExternalTextureAccess::Import(*fixture.frontend, acquired));
        acquiredBindings.front() = {consumers.front().output, output};
        CHECK(Detail::RenderFrontendResourceAccess::BindExternalSceneCanvas(*program, 1, acquiredBindings, {}).HasError());
        const auto bound = UiRequire(Detail::RenderFrontendResourceAccess::BindExternalSceneCanvas(*program, 0, acquiredBindings, {}));
        CHECK(std::get<RenderTextureHandle>(bound->Resources()[consumers.front().output.value - 1].binding) == output);
        REQUIRE(bound->ExternalReleases().size() == 1);
        CHECK(bound->ExternalReleases().front().newState == acquired->RequiredFinalState());
    }
    const auto stale = UiRequire(program->Prepare(0, fixture.Submission(*program, geometry, bindings)));
    UiPaintPreparedFrame prepared;
    if (!neutralRequired)
        prepared = UiRequire(program->Prepare(0, fixture.Submission(*program, geometry, bindings)));
    const auto &consumer = consumers.front();
    constexpr std::array draws{UiPaintDraw{.indexCount = 3,
                                           .scissor = {0, 0, extent.width, extent.height},
                                           .sampling = UiPaintSampling::Premultiplied,
                                           .sampled = 0}};
    const std::array workloads{RenderGraphPassWorkload{consumer.pass, RenderGraphUiPaint{.color = consumer.output,
                                                                                         .vertices = consumer.buffers[0],
                                                                                         .indices = consumer.buffers[1],
                                                                                         .parameters = consumer.buffers[2],
                                                                                         .sampledResources = consumer.sampled,
                                                                                         .extent = extent,
                                                                                         .vertexCount = 3,
                                                                                         .indexCount = 3,
                                                                                         .draws = draws,
                                                                                         .kernel = std::move(kernel),
                                                                                         .clearColor = externalOutput,
                                                                                         .frameRevision = 1}}};
    auto frame = UiRequire(fixture.frontend->BeginFrame({.frameNumber = fixture.frameNumber++, .outputExtent = extent}));
    if (externalOutput)
        CHECK(Detail::RenderFrontendResourceAccess::BindExternalSceneCanvas(*program, 0, acquiredBindings, {}).HasError());
    if (neutralRequired) {
        auto &upload = lifecycleState.uiPaint;
        const auto before = upload.updates;
        const auto revision = upload.revision;
        CHECK(program->Prepare(0, fixture.Submission(*program, geometry, bindings)).HasError());
        upload.onUpload = [&frame] {
            frame.Cancel();
        };
        const auto cancelled = program->PrepareSceneFrame(frame, 0, fixture.Submission(*program, geometry, bindings));
        upload.onUpload = {};
        REQUIRE(cancelled.HasError());
        CHECK(cancelled.ErrorValue().code.Value() == UiPaintFrameErrors::InvalidInput.code.Value());
        CHECK(upload.updates == before);
        CHECK(upload.revision == revision);
        CHECK(UiRequire(program->CanvasSurface(stale)).color == consumers[0].canvas);

        frame = UiRequire(fixture.frontend->BeginFrame({.frameNumber = fixture.frameNumber++, .outputExtent = extent}));
        const auto token = frame.Token();
        std::optional<RenderFrameScope> moved;
        upload.onUpload = [&frame, &moved] {
            moved.emplace(std::move(frame));
        };
        auto movedPreparation = program->PrepareSceneFrame(frame, 0, fixture.Submission(*program, geometry, bindings));
        upload.onUpload = {};
        REQUIRE(moved.has_value());
        CHECK_FALSE(frame.Token().IsValid());
        CHECK(moved->Token() == token);
        prepared = UiRequire(std::move(movedPreparation));
        CHECK(upload.updates == before + 1);
        CHECK(upload.revision == revision + 1);
        CHECK(upload.frame == token);
        frame = std::move(*moved);
    }
    CHECK(UiRequire(program->CanvasSurface(prepared)).color == consumers[0].canvas);
    CHECK(program->Submit(frame, prepared, fixture.Submission(*program, geometry, bindings)).HasError());
    CHECK(program->ExecuteSceneCanvas(frame, stale, fixture.Submission(*program, geometry, bindings), workloads).HasError());
    auto foreign = workloads;
    foreign.front().pass = consumers[1].pass;
    CHECK(program->ExecuteSceneCanvas(frame, prepared, fixture.Submission(*program, geometry, bindings), foreign).HasError());
    CHECK(lifecycleState.graphResources.empty());
    CHECK(lifecycleState.reconstruction.recorded.empty());
    if (neutralRequired) {
        lifecycleState.uiPaint.inspectGraph = [&](const RenderGraphExecutionRequest &request) {
            const auto *clear = std::get_if<RenderGraphColorAttachment>(&request.workloads.front().workload);
            REQUIRE(clear);
            CHECK(clear->operations.loadOperation == AttachmentLoadOperation::Clear);
            CHECK(clear->operations.storeOperation == AttachmentStoreOperation::Store);
            CHECK(clear->operations.clearColor.red == 1.0F);
            CHECK(clear->operations.clearColor.green == 1.0F);
            CHECK(clear->operations.clearColor.blue == 1.0F);
            CHECK(clear->operations.clearColor.alpha == 1.0F);
            const auto paintPasses = request.workloads.subspan(1, request.workloads.size() - 2);
            for (const auto &pass : paintPasses) {
                const auto &paint = std::get<RenderGraphUiPaint>(pass.workload);
                CHECK(paint.neutralSample == clear->texture);
                CHECK(paint.sampledResources.back() == clear->texture);
                CHECK(paint.frameRevision == 2);
            }
            const auto &firstPaint = std::get<RenderGraphUiPaint>(paintPasses.front().workload);
            CHECK(firstPaint.firstParameter == 0);
        };
    }
    REQUIRE(program->ExecuteSceneCanvas(frame, prepared, fixture.Submission(*program, geometry, bindings), workloads).HasValue());
    lifecycleState.uiPaint.inspectGraph = {};
    CHECK(program->ExecuteSceneCanvas(frame, prepared, fixture.Submission(*program, geometry, bindings), workloads).HasError());
    const auto uploads = lifecycleState.uiPaint.updates;
    CHECK(program->PrepareSceneFrame(frame, 0, fixture.Submission(*program, geometry, bindings)).HasError());
    CHECK(lifecycleState.uiPaint.updates == uploads);
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
    if (externalOutput) {
        const auto replacement = std::make_shared<CanvasAcquisition>(fixture.frontend->Capabilities().support.deviceIncarnation);
        acquiredBindings.front().texture = UiRequire(Detail::RenderExternalTextureAccess::Import(*fixture.frontend, replacement));
        const auto rebound = UiRequire(Detail::RenderFrontendResourceAccess::BindExternalSceneCanvas(*program, 0, acquiredBindings, {}));
        CHECK(std::get<RenderTextureHandle>(rebound->Resources()[consumers.front().output.value - 1].binding) ==
              acquiredBindings.front().texture);
    }
    if (neutralRequired || externalOutput) {
        auto later = UiRequire(fixture.frontend->BeginFrame({.frameNumber = fixture.frameNumber++, .outputExtent = extent}));
        CHECK(UiRequire(program->CanvasSurface(prepared)).color == consumers[0].canvas);
        CHECK(program->ExecuteSceneCanvas(later, prepared, fixture.Submission(*program, geometry, bindings), workloads).HasError());
        CHECK(lifecycleState.executeCount == 1);
        later.Cancel();
    }
    REQUIRE(program->Shutdown().HasValue());
    CHECK(program->CanvasSurface(prepared).HasError());
}
