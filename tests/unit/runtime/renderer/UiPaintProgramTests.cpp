#include "Horo/Runtime/Render/UiPaintProgram.h"
#include "RenderFrontendTestSupport.h"
#include "RenderResourceRegistry.h"
#include "UiPaintFrontendState.h"
#include "UiSubmissionSourceFixture.h"

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Runtime::Ui;
    using namespace Horo::Render::Test;
    using namespace Horo::Tests::RenderFrontendTesting;

    /** @brief Establishes the injected backend before constructing its owning fixture member. */
    std::unique_ptr<RenderFrontend> PaintFrontend() {
        lifecycleState = {};
        lifecycleState.uiPaint.enabled = true;
        return CreateTrackingFrontend();
    }

    /** @brief Exercises the public program, real graph and ComposeUi admission; tracking never claims native pixels. */
    struct ProgramFixture final {
        std::unique_ptr<RenderFrontend> frontend{PaintFrontend()};
        UiElementTree tree{UiTree()};
        UiRenderSnapshotLimits limits{4, 0, 0, 0, 3, 1, 1, 3};
        UiRenderExtractor extractor{UiRequire(UiRenderExtractor::Create({{UiOwner(), 9, 1}, limits, 8}))};
        UiRenderGeometryArena arena{UiRequire(UiRenderGeometryArena::Create({{UiOwner(), 9, 1}, {32, 48, 4}, 8}))};
        UiImageResourceRegistry images{UiRequire(UiImageResourceRegistry::Create({UiOwner(), 1}))};

        static UiImageResource MaskImage() {
            const std::array pages{UiImagePage{{UiAsset(1), UiRequire(Assets::AssetTypeId::Parse("core.texture")), true},
                                               {4, 4},
                                               UiImageColorSpace::Linear,
                                               {UiImageFilter::Linear, UiImageFilter::Nearest, UiImageMipmapMode::Linear}}};
            return UiRequire(UiImageResource::Create({UiImageResourceKind::Image, UiImageFallbackPolicy::Reject, pages, {}}));
        }

        UiImageResourceHandle image{
            UiRequire(images.Publish(MaskImage(), UiRequire(UiImageResourceRevision::Create(3)), UiImageResidencyState::Resident))};
        std::optional<UiRenderImageTexture> texture;
        UiResolvedScreenCanvas projection{.logicalExtent = {40960, 30720}, .pixelExtent = {640, 480}, .pixelsPerDip = {1, 1}};
        std::array<UiDrawCommand, 4> commands;
        std::array<UiLogicalTransform, 1> transforms{};
        std::array<UiMask, 2> masks;
        std::array<UiOpacityGroup, 2> groups;
        std::uint64_t revision{1}, frameNumber{1};

        ProgramFixture() {
            std::array<std::byte, 64> alpha;
            alpha.fill(std::byte{128});
            texture = UiRequire(frontend->CreateUiMaskTexture(images, image, 0, alpha));
            const auto root = UiRequire(tree.Root()).handle;
            for (auto &command : commands)
                command = {root, {{0, 0}, {640, 640}}, 0, NoUiRenderIndex, 0, 1, UiSolidDraw{{1, 0, 0, 1}}, 0};
            masks = {UiMask{{{0, 0}, {640, 640}}, 0, 0}, UiMask{{{0, 0}, {320, 640}}, 0, 0, UiMaskShape::AlphaTexture, 0}};
            groups = {UiOpacityGroup{root, {{0, 0}, {640, 640}}, 0, 4, NoUiRenderIndex, 0.5F, 0, NoUiRenderIndex, 0},
                      UiOpacityGroup{root, {{0, 0}, {640, 640}}, 1, 2, 0, 0.25F, 0, NoUiRenderIndex, 1}};
            commands[1].group = commands[2].group = 1;
            commands[1].mask = commands[2].mask = 1;
        }

        UiRenderGeometryPlan Extract() {
            const auto descriptor =
                Runtime::Ui::Test::SnapshotDescriptor(tree, UiRequire(UiInteractionRevision::Create(8)),
                                                      UiRequire(UiRenderSnapshotRevision::Create(revision++)), {UiOwner(), 9, 1}, limits);
            const std::array resources{texture->Source()};
            const auto snapshot = UiRequire(
                extractor
                    .Extract(tree, descriptor,
                             {.commands = commands, .masks = masks, .transforms = transforms, .resources = resources, .groups = groups}));
            return UiRequire(arena.Build(snapshot, projection));
        }

        UiRenderGeometryPlan ExtractInto(UiRenderGeometryArena &target) {
            const auto geometry = Extract();
            return UiRequire(target.Build(geometry.SourceSnapshot(), projection));
        }

        std::unique_ptr<UiPaintProgram> Create(const UiRenderGeometryPlan &geometry, std::span<const RenderTextureHandle> extra = {}) {
            std::vector handles{texture->Creation().handle};
            handles.insert(handles.end(), extra.begin(), extra.end());
            auto result = UiRequire(UiPaintProgram::Create(*frontend, {projection, {512, 768, 128}, 1, 32}, geometry.SourceSnapshot(),
                                                           handles, CookedUiPaintKernel{}));
            REQUIRE(frontend->ProcessResourceRequests().HasValue());
            return result;
        }

        UiRenderSubmission Submission(UiPaintProgram &program, const UiRenderGeometryPlan &geometry,
                                      std::array<UiRenderImageBinding, 1> &bindings) {
            bindings = {
                UiRenderImageBinding{&images, image, 0, UiRequire(program.TextureResource(0, texture->Creation().handle)), &*texture}};
            return {&geometry, bindings, {}, {}};
        }

        void Encode(UiPaintProgram &program, const UiRenderGeometryPlan &geometry, const UiPaintPreparedFrame &prepared) {
            auto frame = UiRequire(frontend->BeginFrame({.frameNumber = frameNumber++, .outputExtent = {640, 480}}));
            REQUIRE(frame.Execute({}).HasValue());
            std::array<UiRenderImageBinding, 1> bindings;
            REQUIRE(program.Submit(frame, prepared, Submission(program, geometry, bindings)).HasValue());
            REQUIRE(frame.Present().HasValue());
        }
    };
}  // namespace

TEST_CASE("UI program sampled table accepts unique prefix ranges and rejects duplicate or invalid handles",
          "[renderer][ui-paint][program]") {
    ProgramFixture fixture;
    const auto geometry = fixture.Extract();
    std::array<std::byte, 64> bytes{};
    const auto alternate = UiRequire(fixture.frontend->CreateUiMaskTexture(fixture.images, fixture.image, 0, bytes));
    const auto first = fixture.texture->Creation().handle;
    std::array textures{first, alternate.Creation().handle};
    bool valid = true;
    SECTION("distinct entries retain their own sampled resource") {}
    SECTION("duplicate entry is rejected before native preparation") {
        textures[1] = first;
        valid = false;
    }
    SECTION("invalid entry is rejected before native preparation") {
        textures[1] = {};
        valid = false;
    }
    const auto kernels = lifecycleState.uiPaint.kernels;
    const auto creations = lifecycleState.createTextureCount;
    auto created = UiPaintProgram::Create(*fixture.frontend, {fixture.projection, {512, 768, 128}, 1, 32}, geometry.SourceSnapshot(),
                                          textures, CookedUiPaintKernel{});
    if (valid) {
        REQUIRE(created.HasValue());
        auto program = std::move(created).Value();
        REQUIRE(fixture.frontend->ProcessResourceRequests().HasValue());
        const auto originalResource = UiRequire(program->TextureResource(0, first));
        const auto alternateResource = UiRequire(program->TextureResource(0, alternate.Creation().handle));
        CHECK(originalResource != alternateResource);
        REQUIRE(program->Shutdown().HasValue());
    } else {
        REQUIRE(created.HasError());
        CHECK(created.ErrorValue().code.Value() == UiPaintFrameErrors::InvalidInput.code.Value());
        CHECK(lifecycleState.uiPaint.kernels == kernels);
        CHECK(lifecycleState.createTextureCount == creations);
    }
}

TEST_CASE("UI program retains every failed creation outcome through operation history eviction", "[renderer][ui-paint][program]") {
    ProgramFixture fixture;
    REQUIRE(fixture.frontend->ProcessResourceRequests().HasValue());
    const auto geometry = fixture.Extract();
    const std::array textures{fixture.texture->Creation().handle};
    auto result = UiPaintProgram::Create(*fixture.frontend, {fixture.projection, {512, 768, 128}, 1, 32}, geometry.SourceSnapshot(),
                                         textures, CookedUiPaintKernel{});
    REQUIRE(result.HasValue());
    auto program = std::move(result).Value();
    lifecycleState.failResourceCreation = true;
    const auto sentinel = fixture.frontend->CreateBuffer({.byteSize = 16, .usage = RenderBufferUsage::Vertex}, {});
    REQUIRE(sentinel.HasValue());
    REQUIRE(fixture.frontend->ProcessResourceRequests().HasValue());
    const auto failed = program->ValidatePreparation();
    REQUIRE(failed.HasError());
    CHECK(failed.ErrorValue().domain.Value() == "render.test");
    CHECK(failed.ErrorValue().code.Value() == "render.test.resource_failed");
    lifecycleState.failResourceCreation = false;
    for (std::uint32_t index = 0; index <= Horo::Render::Detail::RenderResourceRegistryLimits{}.maximumOperationResults; ++index) {
        const auto created = fixture.frontend->CreateBuffer({.byteSize = 16, .usage = RenderBufferUsage::Vertex}, {});
        REQUIRE(created.HasValue());
        REQUIRE(fixture.frontend->ProcessResourceRequests().HasValue());
        REQUIRE(fixture.frontend->ReleaseBuffer(created.Value().handle).HasValue());
    }
    const auto evicted = fixture.frontend->ResourceOperationState(sentinel.Value().operation);
    REQUIRE(evicted.HasError());
    CHECK(evicted.ErrorValue().code.Value() == "render.frontend.resource.operation_unknown");
    auto unobserved = sentinel.Value().handle;
    const auto unknown = Horo::Render::Detail::ObserveUiPaintCreation(*fixture.frontend, sentinel.Value().operation, unobserved);
    REQUIRE(unknown.HasError());
    CHECK(unknown.ErrorValue().code.Value() == "render.frontend.resource.operation_unknown");
    CHECK(unobserved == sentinel.Value().handle);
    CHECK(fixture.frontend->ReleaseBuffer(unobserved).HasError());
    REQUIRE(program->Shutdown().HasValue());
    REQUIRE(program->Shutdown().HasValue());
    const auto retained = fixture.frontend->ResourceState(fixture.texture->Creation().handle);
    REQUIRE(retained.HasValue());
    CHECK(retained.Value() == RenderResourceState::Ready);
    CHECK(fixture.frontend->UploadSnapshot().pendingRequests == 0);
}

TEST_CASE("UI prepared program applies fractional ancestor child masks once at nested group boundaries", "[renderer][ui-paint][program]") {
    ProgramFixture fixture;
    const auto geometry = fixture.Extract();
    auto program = fixture.Create(geometry);
    std::array<UiRenderImageBinding, 1> bindings;
    const auto prepared = UiRequire(program->Prepare(0, fixture.Submission(*program, geometry, bindings)));
    fixture.Encode(*program, geometry, prepared);
    const auto &passes = lifecycleState.uiPaint.passes;
    REQUIRE(passes.size() == 5);
    REQUIRE(passes[0].draws.size() == 1);
    REQUIRE(passes[1].draws.size() == 1);
    CHECK(passes[0].draws[0].sampling == UiPaintSampling::Alpha);
    CHECK(passes[1].draws[0].sampling == UiPaintSampling::Alpha);
    CHECK(passes[0].draws[0].filter == RenderSamplerFilter::Linear);
    CHECK(passes[0].draws[0].magFilter == RenderSamplerFilter::Nearest);
    CHECK(passes[0].draws[0].mipmap == UiPaintMipmap::Linear);
    REQUIRE(passes[2].draws.size() == 1);
    CHECK(passes[2].draws[0].coverage == NoUiPaintTexture);
    REQUIRE(passes[3].draws.size() == 3);
    CHECK(passes[3].draws[0].coverage == NoUiPaintTexture);
    CHECK(passes[3].draws[1].opacity == 0.25F);
    CHECK(passes[3].sampled[passes[3].draws[1].coverage] == passes[1].color);
    CHECK(passes[3].draws[2].coverage == NoUiPaintTexture);
    REQUIRE(passes[4].draws.size() == 1);
    CHECK(passes[4].draws[0].opacity == 0.5F);
    CHECK(passes[4].sampled[passes[4].draws[0].coverage] == passes[0].color);
    CHECK(lifecycleState.uiPaint.kernels == 2);
}

TEST_CASE("UI prepared program ordinary mutation reuses residents and cancellation preserves accepted publication",
          "[renderer][ui-paint][program]") {
    ProgramFixture fixture;
    const auto original = fixture.Extract();
    auto program = fixture.Create(original);
    std::array<UiRenderImageBinding, 1> bindings;
    const auto accepted = UiRequire(program->Prepare(0, fixture.Submission(*program, original, bindings)));
    const auto buffers = lifecycleState.createBufferCount;
    const auto textures = lifecycleState.createTextureCount;
    fixture.groups[1].opacity = 0.75F;
    fixture.commands[0].rect.extent.width = 128;
    fixture.commands[0].payload = UiSolidDraw{{0, 1, 0, 0.5F}};
    const auto replacement = fixture.Extract();
    CancellationSource cancellation;
    cancellation.RequestCancellation();
    const auto rejected = program->Prepare(0, fixture.Submission(*program, replacement, bindings), cancellation.Token());
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == UiPaintFrameErrors::Cancelled.code.Value());
    fixture.Encode(*program, original, accepted);
    const auto updated = UiRequire(program->Prepare(0, fixture.Submission(*program, replacement, bindings)));
    fixture.Encode(*program, replacement, updated);
    CHECK(lifecycleState.uiPaint.passes[3].draws[1].opacity == 0.75F);
    CHECK(lifecycleState.createBufferCount == buffers);
    CHECK(lifecycleState.createTextureCount == textures);
    CHECK(lifecycleState.uiPaint.kernels == 2);
    auto frame = UiRequire(fixture.frontend->BeginFrame({.frameNumber = fixture.frameNumber++, .outputExtent = {640, 480}}));
    REQUIRE(frame.Execute({}).HasValue());
    REQUIRE(program->Submit(frame, accepted, fixture.Submission(*program, original, bindings)).HasError());
}

TEST_CASE("UI prepared program sibling masks retain independent paint order and complete ancestor multiplication",
          "[renderer][ui-paint][program]") {
    ProgramFixture fixture;
    fixture.groups[0].commandCount = fixture.groups[1].commandCount = 2;
    fixture.groups[1].parent = NoUiRenderIndex;
    fixture.groups[1].firstCommand = 2;
    fixture.commands[1].group = 0;
    fixture.commands[1].mask = 1;
    fixture.commands[2].group = fixture.commands[3].group = 1;
    fixture.commands[2].mask = fixture.commands[3].mask = 1;
    const auto geometry = fixture.Extract();
    auto program = fixture.Create(geometry);
    std::array<UiRenderImageBinding, 1> bindings;
    const auto prepared = UiRequire(program->Prepare(0, fixture.Submission(*program, geometry, bindings)));
    fixture.Encode(*program, geometry, prepared);
    const auto &passes = lifecycleState.uiPaint.passes;
    REQUIRE(passes.size() == 6);
    CHECK(passes[2].draws.back().operation == UiPaintOperation::MultiplyCoverage);
    const auto &root = passes.back();
    REQUIRE(root.draws.size() == 2);
    CHECK(root.draws[0].opacity == 0.5F);
    CHECK(root.draws[1].opacity == 0.25F);
    CHECK(root.sampled[root.draws[0].sampled] != root.sampled[root.draws[1].sampled]);
    CHECK(root.sampled[root.draws[0].coverage] != root.sampled[root.draws[1].coverage]);
}

TEST_CASE("UI prepared program structural refresh rejects changed ancestry before native write", "[renderer][ui-paint][program]") {
    ProgramFixture fixture;
    const auto original = fixture.Extract();
    auto program = fixture.Create(original);
    std::array<UiRenderImageBinding, 1> bindings;
    const auto accepted = UiRequire(program->Prepare(0, fixture.Submission(*program, original, bindings)));
    const auto updates = lifecycleState.uiPaint.updates;
    fixture.masks[1].parent = NoUiRenderIndex;
    const auto changed = fixture.Extract();
    const auto rejected = program->Prepare(0, fixture.Submission(*program, changed, bindings));
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == UiPaintProgramErrors::PreparationRequired.code.Value());
    CHECK(lifecycleState.uiPaint.updates == updates);
    fixture.Encode(*program, original, accepted);
    REQUIRE(program->Shutdown().HasValue());
    CHECK_FALSE(accepted.IsValid());
}

TEST_CASE("UI prepared program rejects independently issued equal-revision geometry before ComposeUi", "[renderer][ui-paint][program]") {
    ProgramFixture fixture;
    const auto original = fixture.Extract();
    auto program = fixture.Create(original);
    std::array<UiRenderImageBinding, 1> bindings;
    const auto accepted = UiRequire(program->Prepare(0, fixture.Submission(*program, original, bindings)));
    auto otherArena = UiRequire(UiRenderGeometryArena::Create({{UiOwner(), 9, 1}, {32, 48, 4}, 1}));
    std::optional<UiRenderGeometryPlan> independent;
    SECTION("Same source and exact projection") {
        independent = UiRequire(otherArena.Build(original.SourceSnapshot(), fixture.projection));
    }
    SECTION("Same source without projection") {
        independent = UiRequire(otherArena.Build(original.SourceSnapshot()));
    }
    REQUIRE(independent->Descriptor().snapshotRevision == original.Descriptor().snapshotRevision);
    REQUIRE(&independent->Descriptor() != &original.Descriptor());
    auto frame = UiRequire(fixture.frontend->BeginFrame({.frameNumber = 1, .outputExtent = {640, 480}}));
    REQUIRE(frame.Execute({}).HasValue());
    const auto executions = lifecycleState.executeCount;
    const auto rejected = program->Submit(frame, accepted, fixture.Submission(*program, *independent, bindings));
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == UiPaintProgramErrors::PreparationRequired.code.Value());
    CHECK(lifecycleState.executeCount == executions);
    CHECK(lifecycleState.presentCount == 0);
    const auto sameIssuedCopy = original;
    const auto sameTextureCopy = *fixture.texture;
    auto valid = fixture.Submission(*program, sameIssuedCopy, bindings);
    bindings[0].realization = &sameTextureCopy;
    REQUIRE(program->Submit(frame, accepted, std::move(valid)).HasValue());
    CHECK(lifecycleState.executeCount == executions + 1);
    REQUIRE(frame.Present().HasValue());
}

TEST_CASE("UI prepared program rejects swapped valid predeclared image realization before ComposeUi", "[renderer][ui-paint][program]") {
    ProgramFixture fixture;
    const auto original = fixture.Extract();
    std::array<std::byte, 64> bytes{};
    const auto alternate = UiRequire(fixture.frontend->CreateUiMaskTexture(fixture.images, fixture.image, 0, bytes));
    const std::array extra{alternate.Creation().handle};
    auto program = fixture.Create(original, extra);
    std::array<UiRenderImageBinding, 1> bindings;
    const auto accepted = UiRequire(program->Prepare(0, fixture.Submission(*program, original, bindings)));
    auto frame = UiRequire(fixture.frontend->BeginFrame({.frameNumber = 1, .outputExtent = {640, 480}}));
    REQUIRE(frame.Execute({}).HasValue());
    const auto executions = lifecycleState.executeCount;
    auto swapped = fixture.Submission(*program, original, bindings);
    bindings[0].texture = UiRequire(program->TextureResource(0, alternate.Creation().handle));
    bindings[0].realization = &alternate;
    const auto rejected = program->Submit(frame, accepted, std::move(swapped));
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == UiPaintProgramErrors::PreparationRequired.code.Value());
    CHECK(lifecycleState.executeCount == executions);
    CHECK(lifecycleState.presentCount == 0);
    REQUIRE(program->Submit(frame, accepted, fixture.Submission(*program, original, bindings)).HasValue());
    CHECK(lifecycleState.executeCount == executions + 1);
    REQUIRE(frame.Present().HasValue());
}

TEST_CASE("UI canvas batch rejects a malformed later source before any native encoding", "[renderer][ui-paint][program][batch]") {
    ProgramFixture fixture;
    const auto first = fixture.Extract();
    const auto second = fixture.Extract();
    auto firstProgram = fixture.Create(first);
    auto secondProgram = fixture.Create(second);
    std::array<UiRenderImageBinding, 1> firstBindings, secondBindings;
    const auto firstFrame = UiRequire(firstProgram->Prepare(0, fixture.Submission(*firstProgram, first, firstBindings)));
    const auto secondFrame = UiRequire(secondProgram->Prepare(0, fixture.Submission(*secondProgram, second, secondBindings)));
    auto frame = UiRequire(fixture.frontend->BeginFrame({.frameNumber = 1, .outputExtent = {640, 480}}));
    REQUIRE(frame.Execute({}).HasValue());
    const auto executed = lifecycleState.executeCount;
    std::array contributions{UiPaintProgramSubmission{firstProgram.get(), &firstFrame,
                                                      fixture.Submission(*firstProgram, first, firstBindings)},
                             UiPaintProgramSubmission{secondProgram.get(), &secondFrame,
                                                      fixture.Submission(*secondProgram, first, secondBindings)}};
    REQUIRE(UiPaintProgram::SubmitBatch(frame, contributions).HasError());
    CHECK(lifecycleState.executeCount == executed);
    CHECK(lifecycleState.presentCount == 0);
    // A caller-side correspondence error has not consumed the named stage or the valid first source.
    contributions[0].source = fixture.Submission(*firstProgram, first, firstBindings);
    contributions[1].source = fixture.Submission(*secondProgram, second, secondBindings);
    REQUIRE(UiPaintProgram::SubmitBatch(frame, contributions).HasValue());
    CHECK(lifecycleState.executeCount == executed + 2);
    CHECK(firstProgram->Submit(frame, firstFrame, fixture.Submission(*firstProgram, first, firstBindings)).HasError());
    REQUIRE(frame.Present().HasValue());
}

TEST_CASE("UI preparation-only snapshots cannot become normal uploaded geometry", "[renderer][ui-paint][program][prototype]") {
    ProgramFixture fixture;
    const auto descriptor = Runtime::Ui::Test::SnapshotDescriptor(fixture.tree, UiRequire(UiInteractionRevision::Create(8)),
                                                                  UiRequire(UiRenderSnapshotRevision::Create(fixture.revision++)),
                                                                  {UiOwner(), 9, 1}, fixture.limits);
    const std::array resources{fixture.texture->Source()};
    const auto prototype = UiRequire(fixture.extractor.ExtractPrototype(fixture.tree, descriptor,
                                                                        {.commands = fixture.commands,
                                                                         .masks = fixture.masks,
                                                                         .transforms = fixture.transforms,
                                                                         .resources = resources,
                                                                         .groups = fixture.groups}));
    const auto copy = prototype;
    CHECK(copy.IsPreparationOnly());
    CHECK(fixture.arena.Build(copy, fixture.projection).HasError());
    const std::array textures{fixture.texture->Creation().handle};
    auto program = UiRequire(UiPaintProgram::Create(*fixture.frontend, {fixture.projection, {512, 768, 128}, 1, 32}, prototype, textures,
                                                    CookedUiPaintKernel{}));
    REQUIRE(fixture.frontend->ProcessResourceRequests().HasValue());
    const auto actual = fixture.Extract();
    CHECK_FALSE(actual.SourceSnapshot().IsPreparationOnly());
    std::array<UiRenderImageBinding, 1> bindings;
    const auto prepared = UiRequire(program->Prepare(0, fixture.Submission(*program, actual, bindings)));
    fixture.Encode(*program, actual, prepared);
}

TEST_CASE("A later native UI failure preserves the single queued frame completion lease", "[renderer][ui-paint][program][batch]") {
    ProgramFixture fixture;
    const auto first = fixture.Extract();
    const auto second = fixture.Extract();
    auto firstProgram = fixture.Create(first);
    auto secondProgram = fixture.Create(second);
    std::array<UiRenderImageBinding, 1> firstBindings, secondBindings;
    const auto firstFrame = UiRequire(firstProgram->Prepare(0, fixture.Submission(*firstProgram, first, firstBindings)));
    const auto secondFrame = UiRequire(secondProgram->Prepare(0, fixture.Submission(*secondProgram, second, secondBindings)));
    auto frame = UiRequire(fixture.frontend->BeginFrame({.frameNumber = 1, .outputExtent = {640, 480}}));
    REQUIRE(frame.Execute({}).HasValue());
    lifecycleState.uiPaint.retainAfterAbort = true;
    lifecycleState.uiPaint.failGraphCall = 2;
    std::array contributions{UiPaintProgramSubmission{firstProgram.get(), &firstFrame,
                                                      fixture.Submission(*firstProgram, first, firstBindings)},
                             UiPaintProgramSubmission{secondProgram.get(), &secondFrame,
                                                      fixture.Submission(*secondProgram, second, secondBindings)}};
    REQUIRE(UiPaintProgram::SubmitBatch(frame, contributions).HasError());
    CHECK(lifecycleState.uiPaint.graphCalls == 2);
    CHECK(lifecycleState.graphLease != nullptr);
    CHECK(lifecycleState.abortCount == 1);
    CHECK(lifecycleState.presentCount == 0);
    CHECK(frame.Present().HasError());
    // Explicit simulated completion, not abort/presentation, discharges the retained native queue debt.
    lifecycleState.graphLease->Release();
    lifecycleState.graphLease = nullptr;
}

TEST_CASE("The complete UI batch fences reentrant UI and GUI stage admission", "[renderer][ui-paint][program][batch]") {
    class Gui final : public IGuiPassExecutor {
    public:
        Result<void> RenderDrawData(const GuiCompositionRequest &) override {
            ++calls;
            return Result<void>::Success();
        }

        int calls{};
    } gui;

    ProgramFixture fixture;
    const auto geometry = fixture.Extract();
    auto program = fixture.Create(geometry);
    std::array<UiRenderImageBinding, 1> bindings;
    const auto prepared = UiRequire(program->Prepare(0, fixture.Submission(*program, geometry, bindings)));
    auto frame = UiRequire(fixture.frontend->BeginFrame({.frameNumber = 1, .outputExtent = {640, 480}}));
    REQUIRE(frame.Execute({}).HasValue());
    bool nestedUiRejected{}, nestedGuiRejected{};
    lifecycleState.uiPaint.onGraph = [&] {
        nestedUiRejected = frame.ComposeUi().HasError();
        nestedGuiRejected = frame.ComposeGui(gui).HasError();
    };
    const auto submitted = program->Submit(frame, prepared, fixture.Submission(*program, geometry, bindings));
    lifecycleState.uiPaint.onGraph = {};
    REQUIRE(submitted.HasValue());
    CHECK(nestedUiRejected);
    CHECK(nestedGuiRejected);
    CHECK(gui.calls == 0);
    REQUIRE(frame.ComposeGui(gui).HasValue());
    CHECK(gui.calls == 1);
    REQUIRE(frame.Present().HasValue());
}

TEST_CASE("UI program cold structural preparation releases its source snapshot while pools remain alive",
          "[renderer][ui-paint][program][lifetime]") {
    ProgramFixture fixture;
    std::optional<UiRenderGeometryPlan> source{fixture.Extract()};
    auto program = fixture.Create(*source);
    source.reset();
    CHECK(fixture.arena.IsDrained());
    CHECK(fixture.extractor.IsDrained());
    REQUIRE(program->ValidatePreparation().HasValue());
}

TEST_CASE("UI program replacement retires superseded CPU geometry after exact native completion",
          "[renderer][ui-paint][program][lifetime]") {
    ProgramFixture fixture;
    std::optional<UiRenderGeometryPlan> original{fixture.Extract()};
    auto program = fixture.Create(*original);
    std::array<UiRenderImageBinding, 1> bindings;
    const auto accepted = UiRequire(program->Prepare(0, fixture.Submission(*program, *original, bindings)));
    lifecycleState.completeGraphOnPresent = false;
    fixture.Encode(*program, *original, accepted);
    original.reset();
    REQUIRE(lifecycleState.graphLease != nullptr);
    CHECK_FALSE(fixture.arena.IsDrained());
    lifecycleState.graphLease->Release();
    lifecycleState.graphLease = nullptr;
    CHECK_FALSE(fixture.arena.IsDrained());  // Still the valid accepted CPU publication.
    auto nextArena = UiRequire(UiRenderGeometryArena::Create({{UiOwner(), 9, 1}, {32, 48, 4}, 1}));
    const auto replacement = fixture.ExtractInto(nextArena);
    const auto updated = UiRequire(program->Prepare(0, fixture.Submission(*program, replacement, bindings)));
    CHECK(fixture.arena.IsDrained());
    CHECK_FALSE(nextArena.IsDrained());
    lifecycleState.completeGraphOnPresent = true;
    fixture.Encode(*program, replacement, updated);
}

TEST_CASE("UI program failed candidate releases only incoming CPU provenance and preserves accepted submission",
          "[renderer][ui-paint][program][lifetime]") {
    ProgramFixture fixture;
    const auto original = fixture.Extract();
    auto program = fixture.Create(original);
    std::array<UiRenderImageBinding, 1> bindings;
    const auto accepted = UiRequire(program->Prepare(0, fixture.Submission(*program, original, bindings)));
    auto nextArena = UiRequire(UiRenderGeometryArena::Create({{UiOwner(), 9, 1}, {32, 48, 4}, 1}));
    std::optional<UiRenderGeometryPlan> replacement{fixture.ExtractInto(nextArena)};
    CancellationSource cancellation;
    const ErrorCodeDescriptor *expected{};
    SECTION("cancelled unpublished upload") {
        cancellation.RequestCancellation();
        expected = &UiPaintFrameErrors::Cancelled;
    }
    SECTION("native update temporarily pending") {
        lifecycleState.uiPaint.pending = true;
        expected = &UiPaintFrameErrors::Pending;
    }
    REQUIRE(expected != nullptr);
    const auto failed = program->Prepare(0, fixture.Submission(*program, *replacement, bindings), cancellation.Token());
    REQUIRE(failed.HasError());
    CHECK(failed.ErrorValue().domain.Value() == expected->domain.Value());
    CHECK(failed.ErrorValue().code.Value() == expected->code.Value());
    replacement.reset();
    CHECK(nextArena.IsDrained());
    lifecycleState.uiPaint.pending = false;
    fixture.Encode(*program, original, accepted);
}
