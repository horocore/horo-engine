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

    /** @brief A dual border contributes eight solid quads through the selected paint execution path. */
    bool HasFocusOutline() {
        for (const auto &pass : Tests::RenderFrontendTesting::lifecycleState.uiPaint.passes)
            for (const auto &draw : pass.draws)
                if (draw.sampling == Render::UiPaintSampling::Solid && draw.indexCount >= 8 * 6)
                    return true;
        return false;
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

TEST_CASE("Application paint extracts isolated read-only views from one source", "[application][runtime_ui][paint][views]") {
    Fixture fixture;
    const auto instance = Source::Instance();
    const UiRenderViewId preview{Source::Owner(), 21, 1};
    const UiRenderViewId thumbnail{Source::Owner(), 22, 1};
    auto viewport = Runtime::Ui::AnimationTests::Config().viewport;
    const auto previewAttachment = Take(fixture.port.AttachSnapshotView(instance, preview, viewport));
    viewport.constraints.maximum.width = viewport.content.extent.width = 60;
    const auto thumbnailAttachment = Take(fixture.port.AttachSnapshotView(instance, thumbnail, viewport));
    CHECK(fixture.port.AttachSnapshotView(instance, preview, viewport).HasError());
    viewport.constraints.maximum.width = viewport.content.extent.width = 120;
    REQUIRE(fixture.port.UpdateSnapshotView(instance, previewAttachment, viewport).HasValue());
    CHECK(fixture.port.ExtractSnapshotView(instance, previewAttachment).HasError());
    fixture.Frame();
    const auto previewSnapshot = Take(fixture.port.ExtractSnapshotView(instance, previewAttachment));
    const auto thumbnailSnapshot = Take(fixture.port.ExtractSnapshotView(instance, thumbnailAttachment));
    CHECK(previewSnapshot.Descriptor().view == preview);
    CHECK(thumbnailSnapshot.Descriptor().view == thumbnail);
    REQUIRE_FALSE(previewSnapshot.Commands().empty());
    REQUIRE_FALSE(thumbnailSnapshot.Commands().empty());
    CHECK(previewSnapshot.Commands().front().rect.extent.width > thumbnailSnapshot.Commands().front().rect.extent.width);
    CHECK_FALSE(fixture.port.InputEligible(preview));
    CHECK_FALSE(fixture.port.InputEligible(thumbnail));
    REQUIRE(fixture.port.DetachSnapshotView(instance, previewAttachment).HasValue());
    CHECK(fixture.port.ExtractSnapshotView(instance, previewAttachment).HasError());
    CHECK(fixture.port.UpdateSnapshotView(instance, previewAttachment, viewport).HasError());
    CHECK(fixture.port.ExtractSnapshotView(instance, thumbnailAttachment).HasValue());
    fixture.host->Shutdown();
    CHECK(fixture.port.ExtractSnapshotView(instance, thumbnailAttachment).HasError());
    CHECK_FALSE(previewSnapshot.Commands().empty());
    CHECK_FALSE(thumbnailSnapshot.Commands().empty());
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

TEST_CASE("Application UI projects committed accessibility through the selected native paint",
          "[application][runtime_ui][paint][accessibility]") {
    Fixture fixture{true, {}, true};
    fixture.Frame();
    fixture.Frame();
    auto initial = Take(fixture.port.AcquireFrame());
    CHECK_FALSE(initial.Styles().Records().front().state.Contains(UiVisualStateMask{UiVisualState::HighContrast}));
    REQUIRE(HasFocusOutline());
    REQUIRE(fixture.port.UpdateVisualAccessibility({10, true, false}).HasValue());
    CHECK(fixture.port.UpdateVisualAccessibility({9, false, false}).HasError());
    const auto uploads = Tests::RenderFrontendTesting::lifecycleState.uiPaint.updates;
    fixture.Frame();
    auto current = Take(fixture.port.AcquireFrame());
    CHECK(current.Styles().Records().front().state.Contains(UiVisualStateMask{UiVisualState::HighContrast}));
    CHECK(HasFocusOutline());
    CHECK_FALSE(initial.Styles().Records().front().state.Contains(UiVisualStateMask{UiVisualState::HighContrast}));
    CHECK(initial.Styles().Descriptor().geometry == current.Styles().Descriptor().geometry);
    CHECK(Tests::RenderFrontendTesting::lifecycleState.uiPaint.updates > uploads);
    initial = {};
    current = {};
    fixture.Stop();
    CHECK(fixture.port.UpdateVisualAccessibility({11, false, false}).HasError());
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

TEST_CASE("Application user scale resizes the derived canvas and keeps the focused control reachable",
          "[application][runtime_ui][paint][accessibility]") {
    Fixture fixture{true, {}, false, true};
    REQUIRE(fixture.port.UpdateOutput({{100, 100}, {1, 1}, {}}).HasValue());
    fixture.Frame();
    fixture.Frame();
    REQUIRE(fixture.port.InputEligible(fixture.view));
    auto before = Take(fixture.port.AcquireFrame());
    const auto child = before.Controls().front().source.element;
    REQUIRE(Take(before.Layout().Get(child)).arrangement.hitTest.extent.width == 5 * UiScalarUnitsPerDip);

    REQUIRE(fixture.port.UpdateVisualAccessibility({2, false, false, {3, 1}, DefaultUiAccessibleTargetDips}).HasValue());
    CHECK_FALSE(fixture.port.InputEligible(fixture.view));
    CHECK(fixture.port.UpdateVisualAccessibility({1, false, false, {1, 1}}).HasError());
    CHECK(fixture.port.UpdateVisualAccessibility({3, false, false, {4, 1}}).HasError());
    fixture.Frame();
    CHECK_FALSE(fixture.port.InputEligible(fixture.view));
    fixture.Frame();
    REQUIRE(fixture.port.InputEligible(fixture.view));
    auto scaled = Take(fixture.port.AcquireFrame());
    const auto root = scaled.Layout().Records().front();
    REQUIRE(root.constraints.maximum.width == 2133);
    REQUIRE(Take(scaled.Layout().Get(child)).arrangement.hitTest.extent.width >= DefaultUiAccessibleTargetDips * UiScalarUnitsPerDip);
    REQUIRE(scaled.Clipping());
    REQUIRE_FALSE(scaled.Clipping()->Scrolls().empty());
    CHECK(scaled.Clipping()->Scrolls().front().offset.y > 0);
    CHECK(HasFocusOutline());
    CHECK(Take(before.Layout().Get(child)).arrangement.hitTest.extent.width == 5 * UiScalarUnitsPerDip);

    const auto revision = std::max(scaled.Layout().Descriptor().sources.tree.Value(), scaled.Layout().Descriptor().interaction.Value());
    before = {};
    scaled = {};
    REQUIRE(fixture.port.UpdateVisualAccessibility({3, false, false, {1, 1}, DefaultUiAccessibleTargetDips}).HasValue());
    auto replacement = fixture.source.Make(2, revision + 1);
    auto inputs = Runtime::Ui::AnimationTests::Resources(replacement.Canvases().front());
    REQUIRE(fixture.port
                .Reload({std::move(replacement),
                         fixture.source.slots,
                         std::move(inputs.registry),
                         std::move(inputs.styles),
                         std::move(inputs.definition),
                         {UiAnimationReloadPolicy::Cancel, Tests::UiAccess::Lifecycle}},
                        fixture.Declaration(2), fixture.Assets())
                .HasValue());
    CHECK_FALSE(fixture.port.InputEligible(fixture.view));
    fixture.Frame();
    fixture.Frame();
    auto reset = Take(fixture.port.AcquireFrame());
    CHECK(reset.Layout().Descriptor().sources.document.Value() == 2);
    CHECK(reset.Layout().Records().front().constraints.maximum.width == 6400);
    reset = {};
    fixture.Stop();
    CHECK(fixture.port.UpdateVisualAccessibility({4, false, false, {2, 1}}).HasError());
}
