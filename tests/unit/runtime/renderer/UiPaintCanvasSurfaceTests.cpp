#include "Horo/Runtime/Render/UiPaintProgram.h"
#include "UiPaintProgramTestSupport.h"

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
