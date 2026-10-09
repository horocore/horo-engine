#include "RenderGraphTestUtils.h"
#include "UiSubmissionBackendFixture.h"

namespace Horo::Render::Test {
    namespace {
        struct UiImportedGraph final {
            CompiledRenderGraphExecution execution;
            RenderGraphPassRef pass;
            RenderGraphResourceId image;
            RenderGraphResourceId atlas;
            RenderGraphResourceId secondAtlas;
        };

        UiImportedGraph UiCompileGraph(const RenderTextureHandle image, const RenderTextureHandle atlas,
                                       const std::optional<RenderTextureHandle> secondAtlas = {}) {
            auto limits = SmallLimits();
            limits.maxResources = 3;
            auto builder = RequireBuilder(limits);
            const auto pass = RequirePass(builder, RenderPassKind::Graphics, RenderQueueRole::Graphics);
            const auto imageId = RequireResource(builder.ImportTexture(image, RenderGraphResourceClass::Persistent));
            const auto atlasId = RequireResource(builder.ImportTexture(atlas, RenderGraphResourceClass::Persistent));
            const auto secondAtlasId = secondAtlas
                                           ? RequireResource(builder.ImportTexture(*secondAtlas, RenderGraphResourceClass::Persistent))
                                           : RenderGraphResourceId{};
            auto graph = RequireGraph(builder);
            const auto schedule = RequireSchedule(graph);
            const std::array queues{RenderQueueAssignment{RenderQueueRole::Graphics, {1}}};
            const RenderGraphLogicalState state{RenderGraphSynchronizationAccess::Read,
                                                RenderGraphSynchronizationOperation::Sampled,
                                                RenderGraphPipelineScope::Graphics,
                                                RenderGraphTextureLayout::ShaderReadOnly,
                                                {1}};
            const std::array initial{RenderGraphImportedState{imageId, state}, RenderGraphImportedState{atlasId, state},
                                     RenderGraphImportedState{secondAtlasId, state}};
            const auto synchronization =
                UiRequire(SynthesizeRenderGraphSynchronization(graph, schedule, queues, std::span{initial}.first(secondAtlas ? 3 : 2)));
            return {UiRequire(CompileRenderGraphExecution(graph, schedule, synchronization, queues)), pass, imageId, atlasId,
                    secondAtlasId};
        }

        struct UiSubmissionHost final {
            UiSubmissionBackendState state;
            std::unique_ptr<RenderFrontend> frontend{UiFrontend(state)};
            UiSubmissionSources sources;
            std::array<std::byte, 64> imageBytes{};
            std::array<std::byte, 128> atlasBytes{};
            std::optional<UiRenderImageTexture> image{
                UiRequire(frontend->CreateUiImageTexture(sources.images, sources.image, 0, imageBytes))};
            std::optional<UiRenderAtlasTexture> atlas{
                UiRequire(frontend->CreateUiGlyphAtlasTexture(sources.atlas, sources.atlas.Pages().front(), atlasBytes))};
            std::optional<UiRenderAtlasTexture> secondAtlas;
            UiImportedGraph graph;
            std::array<UiRenderImageBinding, 1> images{{{&sources.images, sources.image, 0, graph.image, &*image}}};
            std::array<UiRenderFontBinding, 2> fonts;
            std::array<RenderGraphPassWorkload, 1> workloads{{{graph.pass, std::monostate{}}}};

            explicit UiSubmissionHost(const bool twoPages = false)
                : sources(true, twoPages),
                  secondAtlas(twoPages ? std::optional{UiRequire(
                                             frontend->CreateUiGlyphAtlasTexture(sources.atlas, sources.atlas.Pages()[1], atlasBytes))}
                                       : std::nullopt),
                  graph(UiCompileGraph(image->Creation().handle, atlas->Creation().handle,
                                       secondAtlas ? std::optional{secondAtlas->Creation().handle} : std::nullopt)) {
                fonts[0] = {&*sources.font, graph.atlas, &*atlas};
                if (secondAtlas)
                    fonts[1] = {&*sources.font, graph.secondAtlas, &*secondAtlas};
                REQUIRE(frontend->ProcessResourceRequests().HasValue());
            }

            UiRenderSubmission Request() {
                return {&*sources.geometry, images, std::span{fonts}.first(secondAtlas ? 2 : 1),
                        UiRequire(sources.atlas.SealFrame(sources.frame))};
            }

            void DropSources() {
                sources.CloseSources();
                image.reset();
                atlas.reset();
                secondAtlas.reset();
            }

            void RequireRejected(UiRenderSubmission request) {
                REQUIRE(frontend->SubmitUiGraph({1, {64, 64}}, graph.execution, workloads, std::move(request)).HasError());
                REQUIRE(state.executions == 0);
                REQUIRE(sources.atlas.IsDrained());
            }
        };
    }  // namespace

    TEST_CASE("UI host retains geometry images and font atlas pins past presentation resize and reload",
              "[renderer][runtime_ui][submission_lifetime]") {
        UiSubmissionHost host;
        const auto imageTexture = host.image->Creation().handle;
        const auto atlasTexture = host.atlas->Creation().handle;
        REQUIRE(host.frontend->SubmitUiGraph({1, {64, 64}}, host.graph.execution, host.workloads, host.Request()).HasValue());
        REQUIRE(host.frontend->Resize({128, 128}).HasValue());
        const auto replacement = host.sources.images.Reload(host.sources.image, UiImage(), UiRequire(UiImageResourceRevision::Create(4)),
                                                            UiImageResidencyState::Resident);
        REQUIRE(replacement.HasValue());
        host.DropSources();
        REQUIRE_FALSE(host.sources.images.IsDrained());
        REQUIRE_FALSE(host.sources.arena.IsDrained());
        REQUIRE_FALSE(host.sources.atlas.IsDrained());
        REQUIRE(host.frontend->ReleaseTexture(imageTexture).HasValue());
        REQUIRE(host.frontend->ReleaseTexture(atlasTexture).HasValue());
        REQUIRE(host.frontend->ProcessResourceRequests().HasValue());
        REQUIRE(host.state.destroyedTextures == 0);
        host.state.Complete();
        REQUIRE(host.sources.images.IsDrained());
        REQUIRE(host.sources.arena.IsDrained());
        REQUIRE(host.sources.atlas.IsDrained());
        REQUIRE(host.frontend->ProcessResourceRequests().HasValue());
        REQUIRE(host.state.destroyedTextures == 2);
    }

    TEST_CASE("UI host rejects arbitrary textures and stale or foreign image publications", "[renderer][runtime_ui][submission_lifetime]") {
        UiSubmissionHost host;
        SECTION("valid imported texture with wrong frontend-issued source authority") {
            host.images[0].texture = host.graph.atlas;
        }
        SECTION("stale image handle after actual reload") {
            REQUIRE(
                host.sources.images
                    .Reload(host.sources.image, UiImage(), UiRequire(UiImageResourceRevision::Create(4)), UiImageResidencyState::Resident)
                    .HasValue());
        }
        SECTION("current newer image paired with old realization and extraction") {
            host.images[0].image =
                UiRequire(host.sources.images.Reload(host.sources.image, UiImage(), UiRequire(UiImageResourceRevision::Create(4)),
                                                     UiImageResidencyState::Resident));
        }
        SECTION("closed source authority") {
            host.sources.images.Close();
        }
        SECTION("equal-looking handle from a different registry is not the same publication") {
            auto foreign = UiRequire(UiImageResourceRegistry::Create({UiOwner(), 1}));
            const auto handle =
                UiRequire(foreign.Publish(UiImage(), UiRequire(UiImageResourceRevision::Create(3)), UiImageResidencyState::Resident));
            REQUIRE(handle == host.sources.image);
            host.images[0].registry = &foreign;
            host.RequireRejected(host.Request());
            return;
        }
        host.RequireRejected(host.Request());
    }

    TEST_CASE("UI host proves actual atlas owner frame glyph and font payload correspondence",
              "[renderer][runtime_ui][submission_lifetime]") {
        UiSubmissionHost host;
        auto request = host.Request();
        SECTION("cross-atlas lease with equal-looking owner page revision and glyph evidence") {
            auto foreign = UiAtlas();
            REQUIRE(foreign.Pages().front() == host.atlas->Page());
            const auto frame = UiRequire(foreign.BeginFrame());
            static_cast<void>(UiPinGlyph(foreign, frame, UiGlyphKey(), &*host.sources.font));
            request.atlas = UiRequire(foreign.SealFrame(frame));
            host.RequireRejected(std::move(request));
            REQUIRE(foreign.IsDrained());
            return;
        }
        SECTION("different valid frame pins the wrong glyph") {
            const auto frame = UiRequire(host.sources.atlas.BeginFrame());
            static_cast<void>(UiPinGlyph(host.sources.atlas, frame, UiGlyphKey(2), &*host.sources.font));
            request.atlas = UiRequire(host.sources.atlas.SealFrame(frame));
        }
        SECTION("matching font declarations cannot substitute a different immutable payload") {
            host.sources.font = UiFont();
        }
        SECTION("wrong font source generation") {
            host.sources.font = UiFont(4);
        }
        SECTION("atlas mapped to an arbitrary valid imported texture") {
            host.fonts[0].texture = host.graph.image;
        }
        SECTION("shutdown rejects new admission despite retained old page/frame leases") {
            host.sources.atlas.Shutdown();
        }
        host.RequireRejected(std::move(request));
    }

    TEST_CASE("UI host abandons unsent generations exactly once on execution presentation and exception failures",
              "[renderer][runtime_ui][submission_lifetime]") {
        UiSubmissionHost host;
        SECTION("encoding failure") {
            host.state.failExecute = true;
        }
        SECTION("presentation failure before native submission") {
            host.state.failPresent = true;
        }
        SECTION("encoding exception") {
            host.state.throwExecute = true;
        }
        REQUIRE(host.frontend->SubmitUiGraph({1, {64, 64}}, host.graph.execution, host.workloads, host.Request()).HasError());
        REQUIRE(host.state.abandoned == 1);
        host.DropSources();
        REQUIRE(host.sources.images.IsDrained());
        REQUIRE(host.sources.arena.IsDrained());
        REQUIRE(host.sources.atlas.IsDrained());
    }

    TEST_CASE("UI owner destruction and renderer shutdown retire owned submissions without source borrows",
              "[renderer][runtime_ui][submission_lifetime]") {
        UiSubmissionBackendState state;
        auto frontend = UiFrontend(state);
        {
            UiSubmissionSources sources;
            const std::array<std::byte, 64> imageBytes{};
            const std::array<std::byte, 128> atlasBytes{};
            const auto image = UiRequire(frontend->CreateUiImageTexture(sources.images, sources.image, 0, imageBytes));
            const auto atlas = UiRequire(frontend->CreateUiGlyphAtlasTexture(sources.atlas, sources.atlas.Pages().front(), atlasBytes));
            const auto graph = UiCompileGraph(image.Creation().handle, atlas.Creation().handle);
            const std::array images{UiRenderImageBinding{&sources.images, sources.image, 0, graph.image, &image}};
            const std::array fonts{UiRenderFontBinding{&*sources.font, graph.atlas, &atlas}};
            const std::array workloads{RenderGraphPassWorkload{graph.pass, std::monostate{}}};
            REQUIRE(frontend
                        ->SubmitUiGraph({1, {64, 64}}, graph.execution, workloads,
                                        {&*sources.geometry, images, fonts, UiRequire(sources.atlas.SealFrame(sources.frame))})
                        .HasValue());
        }
        REQUIRE(state.submitted[0] != nullptr);
        frontend.reset();
        REQUIRE(state.submitted[0] == nullptr);
        state.Complete();
    }

    TEST_CASE("UI host retains failed submitted generations until teardown and rejects them after renderer restart",
              "[renderer][runtime_ui][submission_lifetime]") {
        UiSubmissionHost host;
        REQUIRE(host.frontend->SubmitUiGraph({1, {64, 64}}, host.graph.execution, host.workloads, host.Request()).HasValue());
        host.DropSources();
        host.state.failedCompletion = true;
        REQUIRE(host.frontend->BeginFrame({2, {64, 64}}).HasError());
        REQUIRE(host.state.submitted[0] != nullptr);
        REQUIRE_FALSE(host.sources.images.IsDrained());
        REQUIRE_FALSE(host.sources.arena.IsDrained());
        REQUIRE_FALSE(host.sources.atlas.IsDrained());
        host.frontend.reset();
        REQUIRE(host.state.submitted[0] == nullptr);
        REQUIRE(host.sources.images.IsDrained());
        REQUIRE(host.sources.arena.IsDrained());
        REQUIRE(host.sources.atlas.IsDrained());

        UiSubmissionHost restarted;
        restarted.graph = std::move(host.graph);
        restarted.workloads[0].pass = restarted.graph.pass;
        restarted.images[0].texture = restarted.graph.image;
        restarted.fonts[0].texture = restarted.graph.atlas;
        restarted.RequireRejected(restarted.Request());
        host.state.Complete();
        REQUIRE(restarted.state.submitted[0] == nullptr);
    }

    TEST_CASE("UI host supports one immutable font whose exact pinned glyphs span two atlas pages",
              "[renderer][runtime_ui][submission_lifetime]") {
        UiSubmissionHost host{true};
        auto request = host.Request();
        bool rejected{};
        SECTION("complete two-page font coverage") {}
        SECTION("missing second page") {
            request.fonts = std::span{host.fonts}.first(1);
            rejected = true;
        }
        SECTION("wrong page realization") {
            host.fonts[1].realization = &*host.atlas;
            rejected = true;
        }
        SECTION("duplicate first page cannot stand in for second page") {
            host.fonts[1] = host.fonts[0];
            rejected = true;
        }
        SECTION("page bindings need not follow glyph traversal order") {
            std::swap(host.fonts[0], host.fonts[1]);
        }
        if (rejected) {
            host.RequireRejected(std::move(request));
            return;
        }
        REQUIRE(host.frontend->SubmitUiGraph({1, {64, 64}}, host.graph.execution, host.workloads, std::move(request)).HasValue());
        host.DropSources();
        REQUIRE_FALSE(host.sources.atlas.IsDrained());
        host.state.Complete();
        REQUIRE(host.sources.atlas.IsDrained());
        REQUIRE(host.sources.images.IsDrained());
        REQUIRE(host.sources.arena.IsDrained());
    }

    TEST_CASE("UI texture realization rejects malformed source upload bytes before issuing provenance",
              "[renderer][runtime_ui][submission_lifetime]") {
        UiSubmissionBackendState state;
        auto frontend = UiFrontend(state);
        UiSubmissionSources sources;
        const std::array<std::byte, 129> malformed{};
        std::size_t imageSize{1};
        std::size_t atlasSize{1};
        SECTION("truncated upload") {}
        SECTION("oversized upload") {
            imageSize = 65;
            atlasSize = 129;
        }
        const auto image = frontend->CreateUiImageTexture(sources.images, sources.image, 0, std::span{malformed}.first(imageSize));
        const auto atlas =
            frontend->CreateUiGlyphAtlasTexture(sources.atlas, sources.atlas.Pages().front(), std::span{malformed}.first(atlasSize));
        REQUIRE(image.HasError());
        REQUIRE(atlas.HasError());
        REQUIRE(image.ErrorValue().code.Value() == "render.frontend.resource.invalid_texture_descriptor");
        REQUIRE(atlas.ErrorValue().code.Value() == "render.frontend.resource.invalid_texture_descriptor");
        REQUIRE(frontend->UploadSnapshot().pendingRequests == 0);
        REQUIRE(state.nextInstance == 1);
        const std::array<std::byte, 64> imageBytes{};
        const std::array<std::byte, 128> atlasBytes{};
        REQUIRE(frontend->CreateUiImageTexture(sources.images, sources.image, 0, imageBytes).HasValue());
        REQUIRE(frontend->CreateUiGlyphAtlasTexture(sources.atlas, sources.atlas.Pages().front(), atlasBytes).HasValue());
        REQUIRE(frontend->ProcessResourceRequests().HasValue());
        REQUIRE(state.nextInstance == 3);
    }
}  // namespace Horo::Render::Test
