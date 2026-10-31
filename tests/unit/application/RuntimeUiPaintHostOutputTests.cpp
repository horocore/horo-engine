#include "RuntimeUiPaintHostTestFixture.h"

#include <catch2/generators/catch_generators.hpp>

using namespace Horo;
using namespace Horo::Tests::UiPaintHostTesting;

namespace {
    /** @brief Inspects the actual admitted leaf raster bounds rather than synthesizing a pixel image. */
    void CheckLeafSafeBounds(const UiCanvasPixelRect &content) {
        bool found{};
        for (const auto &pass : Tests::RenderFrontendTesting::lifecycleState.uiPaint.passes) {
            for (const auto &draw : pass.draws) {
                if (draw.sampling != Render::UiPaintSampling::Color)
                    continue;
                found = true;
                CHECK(draw.scissor.x >= content.x);
                CHECK(draw.scissor.y >= content.y);
                CHECK(draw.scissor.x + draw.scissor.width <= content.x + content.width);
                CHECK(draw.scissor.y + draw.scissor.height <= content.y + content.height);
            }
        }
        REQUIRE(found);
    }
}  // namespace

TEST_CASE("Empty application UI startup does not require optional native paint capability", "[application][runtime_ui][paint]") {
    auto frontend = Frontend(false);
    DeterministicClock clock;
    auto host = Take(Runtime::RuntimeHost::Create(clock));
    const Render::FramebufferExtent output{100, 100};
    auto composition = Take(RuntimeUiPaintHost::Compose(*frontend, host->DispatchSource(), UiSceneReconciliationLimits{Source::Owner()}));
    auto port = composition.port;
    REQUIRE(host->AddParticipant(std::move(composition.participant)).HasValue());
    REQUIRE(host->AddParticipant(std::make_unique<Renderer>(*frontend, port, output)).HasValue());
    REQUIRE(host->Startup().HasValue());
    const auto residents = Tests::RenderFrontendTesting::lifecycleState.createBufferCount;
    clock.Advance(Duration::FromMilliseconds(16));
    REQUIRE(host->RunFrame().HasValue());
    CHECK(host->State() == Runtime::RuntimeLifecycleState::Running);
    CHECK(port.Phase() == RuntimeUiPaintHostPhase::NoSource);
    CHECK(Tests::RenderFrontendTesting::lifecycleState.createBufferCount == residents);
    CHECK(Tests::RenderFrontendTesting::lifecycleState.uiPaint.updates == 0);
    host->Shutdown();
    REQUIRE(Take(port.CollectRetired()));
}

TEST_CASE("Unavailable optional native paint rejects real source attachment without blocking ordinary application frames",
          "[application][runtime_ui][paint]") {
    Fixture fixture{false};
    REQUIRE(fixture.port.Phase() == RuntimeUiPaintHostPhase::NoSource);
    const auto residents = Tests::RenderFrontendTesting::lifecycleState.createBufferCount;
    fixture.Frame();
    fixture.Frame();
    CHECK(Tests::RenderFrontendTesting::lifecycleState.uiPaint.updates == 0);
    CHECK(Tests::RenderFrontendTesting::lifecycleState.createBufferCount == residents);
    CHECK(Take(fixture.port.Issue(fixture.source.Grant()))->Validate().HasValue());
    fixture.Stop();
}

TEST_CASE("Application UI coalesces actual resize DPI and insets before new native presentation restores input",
          "[application][runtime_ui][paint][output]") {
    const auto evidence = GENERATE(UiCanvasViewportEvidence{{160, 120}, {1, 1}, {}}, UiCanvasViewportEvidence{{100, 100}, {2, 1}, {}},
                                   UiCanvasViewportEvidence{{100, 100}, {1, 1}, {5, 7, 9, 11}});
    Fixture fixture;
    fixture.Frame();
    fixture.Frame();
    REQUIRE(fixture.port.InputEligible(fixture.view));
    auto previous = Take(fixture.port.AcquireFrame());
    const auto before = previous.Layout().Descriptor();
    const auto residents = Tests::RenderFrontendTesting::lifecycleState.createBufferCount;
    REQUIRE(fixture.port.UpdateOutput({{180, 140}, {3, 2}, {}}).HasValue());
    REQUIRE(fixture.port.UpdateOutput(evidence).HasValue());
    CHECK_FALSE(fixture.port.InputEligible(fixture.view));
    REQUIRE(fixture.port.Presented(2, true).HasValue());
    CHECK_FALSE(fixture.port.InputEligible(fixture.view));
    fixture.output = {evidence.pixelExtent.width, evidence.pixelExtent.height};
    fixture.Frame();
    CHECK_FALSE(fixture.port.InputEligible(fixture.view));
    CHECK(Tests::RenderFrontendTesting::lifecycleState.createBufferCount == residents);
    fixture.Frame();
    REQUIRE(fixture.port.InputEligible(fixture.view));
    // Keep the original explicit reader across refresh and repeated updates; completion releases each submitted frame.
    for (unsigned index = 0; index < 4; ++index) {
        fixture.Frame();
        CHECK(Tests::RenderFrontendTesting::lifecycleState.graphLease == nullptr);
        CHECK(fixture.port.InputEligible(fixture.view));
        CHECK(previous.Layout().Descriptor().interaction == before.interaction);
    }
    auto current = Take(fixture.port.AcquireFrame());
    CHECK(current.Layout().Descriptor().interaction > before.interaction);
    CHECK(current.Layout().Descriptor().sources.canvas > before.sources.canvas);
    const auto resolved = Take(ResolveUiScreenCanvasWithEvidence(fixture.authoredCanvas, evidence));
    REQUIRE_FALSE(current.Layout().Records().empty());
    CHECK((current.Layout().Records().front().constraints.maximum ==
           UiLogicalExtent{resolved.logicalExtent.width, resolved.logicalExtent.height}));
    const auto content = resolved.ContentPixelRect();
    REQUIRE(content.width > 0);
    REQUIRE(content.height > 0);
    CheckLeafSafeBounds(content);
    CHECK(Tests::RenderFrontendTesting::lifecycleState.createBufferCount == residents + 3);
    REQUIRE(fixture.port.UpdateOutput(evidence).HasValue());
    CHECK(fixture.port.UpdateOutput({{0, 100}, {1, 1}, {}}).HasError());
    CHECK(fixture.port.InputEligible(fixture.view));
    fixture.Frame();
    CHECK(Tests::RenderFrontendTesting::lifecycleState.createBufferCount == residents + 3);
    previous = {};
    current = {};
    fixture.Stop();
}
