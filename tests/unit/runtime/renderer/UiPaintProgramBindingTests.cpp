#include "Horo/Runtime/Render/UiPaintProgram.h"
#include "RenderFrontendTestSupport.h"
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

    /** @brief Actual issued font/image/atlas inputs admitted through the existing canonical validator. */
    struct FontProgramFixture final {
        std::unique_ptr<RenderFrontend> frontend{PaintFrontend()};
        UiSubmissionSources sources;
        std::optional<UiRenderImageTexture> image;
        std::optional<UiRenderAtlasTexture> atlas, alternate;
        std::unique_ptr<UiPaintProgram> program;
        std::array<UiRenderImageBinding, 1> images;
        std::array<UiRenderFontBinding, 1> fonts;

        FontProgramFixture() {
            const std::array<std::byte, 64> imageBytes{};
            const std::array<std::byte, 128> atlasBytes{};
            image = UiRequire(frontend->CreateUiImageTexture(sources.images, sources.image, 0, imageBytes));
            atlas = UiRequire(frontend->CreateUiGlyphAtlasTexture(sources.atlas, sources.atlas.Pages().front(), atlasBytes));
            alternate = UiRequire(frontend->CreateUiGlyphAtlasTexture(sources.atlas, sources.atlas.Pages().front(), atlasBytes));
            const std::array handles{image->Creation().handle, atlas->Creation().handle, alternate->Creation().handle};
            const UiResolvedScreenCanvas projection{.logicalExtent = {40960, 30720}, .pixelExtent = {640, 480}, .pixelsPerDip = {1, 1}};
            program = UiRequire(UiPaintProgram::Create(*frontend, {projection, {64, 96, 16}, 1, 32}, sources.geometry->SourceSnapshot(),
                                                       handles, CookedUiPaintKernel{}));
            REQUIRE(frontend->ProcessResourceRequests().HasValue());
            images = {UiRenderImageBinding{&sources.images, sources.image, 0,
                                           UiRequire(program->TextureResource(0, image->Creation().handle)), &*image}};
            fonts = {UiRenderFontBinding{&*sources.font, UiRequire(program->TextureResource(0, atlas->Creation().handle)), &*atlas}};
        }

        UiRenderSubmission Request(UiGlyphAtlas::FrameLease sealed) {
            return {&*sources.geometry, images, fonts, std::move(sealed)};
        }
    };
}  // namespace

TEST_CASE("UI prepared program binds exact font texture and sealed atlas owner before ComposeUi", "[renderer][ui-paint][program]") {
    FontProgramFixture fixture;
    auto original = fixture.Request(UiRequire(fixture.sources.atlas.SealFrame(fixture.sources.frame)));
    const auto accepted = UiRequire(fixture.program->Prepare(0, original));
    auto frame = UiRequire(fixture.frontend->BeginFrame({.frameNumber = 1, .outputExtent = {640, 480}}));
    REQUIRE(frame.Execute({}).HasValue());
    const auto executions = lifecycleState.executeCount;
    SECTION("Another valid realization of the same admitted atlas page") {
        fixture.fonts[0].texture = UiRequire(fixture.program->TextureResource(0, fixture.alternate->Creation().handle));
        fixture.fonts[0].realization = &*fixture.alternate;
        const auto rejected = fixture.program->Submit(frame, accepted, std::move(original));
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == UiPaintProgramErrors::PreparationRequired.code.Value());
    }
    SECTION("Equal numeric frame and revision from a different atlas authority") {
        auto foreign = UiAtlas();
        const auto foreignFrame = UiRequire(foreign.BeginFrame());
        static_cast<void>(UiPinGlyph(foreign, foreignFrame, UiGlyphKey(), &*fixture.sources.font));
        auto sealed = UiRequire(foreign.SealFrame(foreignFrame));
        REQUIRE(sealed.Frame() == original.atlas.Frame());
        REQUIRE(sealed.Revision() == original.atlas.Revision());
        REQUIRE_FALSE(sealed.OwnsPage(fixture.atlas->Source()));
        const auto rejected = fixture.program->Submit(frame, accepted, fixture.Request(std::move(sealed)));
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == UiPaintProgramErrors::PreparationRequired.code.Value());
    }
    SECTION("Another valid frame of the same atlas") {
        const auto later = UiRequire(fixture.sources.atlas.BeginFrame());
        REQUIRE(fixture.sources.atlas.Resolve(later, UiGlyphKey()).HasValue());
        const auto rejected = fixture.program->Submit(frame, accepted, fixture.Request(UiRequire(fixture.sources.atlas.SealFrame(later))));
        REQUIRE(rejected.HasError());
        CHECK(rejected.ErrorValue().code.Value() == UiPaintProgramErrors::PreparationRequired.code.Value());
    }
    CHECK(lifecycleState.executeCount == executions);
    CHECK(lifecycleState.presentCount == 0);
}

TEST_CASE("UI prepared program accepts copied font realization with the exact prepared atlas frame", "[renderer][ui-paint][program]") {
    FontProgramFixture fixture;
    auto source = fixture.Request(UiRequire(fixture.sources.atlas.SealFrame(fixture.sources.frame)));
    const auto accepted = UiRequire(fixture.program->Prepare(0, source));
    const auto sameFont = *fixture.sources.font;
    const auto sameAtlas = *fixture.atlas;
    fixture.fonts[0].face = &sameFont;
    fixture.fonts[0].realization = &sameAtlas;
    auto frame = UiRequire(fixture.frontend->BeginFrame({.frameNumber = 1, .outputExtent = {640, 480}}));
    REQUIRE(frame.Execute({}).HasValue());
    const auto executions = lifecycleState.executeCount;
    REQUIRE(fixture.program->Submit(frame, accepted, std::move(source)).HasValue());
    CHECK(lifecycleState.executeCount == executions + 1);
    REQUIRE(frame.Present().HasValue());
}
