#include "Horo/Runtime/Ui/UiCaptionRenderer.h"
#include "UiAccessibilityTestUtils.h"

#include <catch2/catch_approx.hpp>

namespace Horo::Runtime::Ui {
    namespace {
        struct CaptionPaintFixture final {
            UiElementTree tree{MakeTree()};
            std::unique_ptr<CaptionCueQueue> queue;
            CaptionTimeline timeline;
            CaptionAppearance appearance{.enabled = true, .showSoundDescriptions = false};
            UiRenderSnapshotLimits limits{16, 8, 32, 1, 0, 1, 8};
            UiRenderViewId view{Owner(), 9, 1};
            Sha256Digest localization{{1}};
            std::array<UiPositionedGlyph, 1> glyphs{{{17, 0, {64, 64}, {1024, 2048}}}};
            std::array<UiTextRun, 1> runs{{{0, 0, 1, {1, 1, 1, 1}}}};
            std::array<UiRenderResourceReference, 1> resources{};

            CaptionPaintFixture() {
                auto text = LocalizedMessageRef::Create(UiMessageKey::Create("game.dialogue", "hello").Value(), "Hello").Value();
                queue = std::move(CaptionCueQueue::Create(1, {std::move(text)})).Value();
                REQUIRE(timeline.Reset(1).HasValue());
                std::array<std::uint8_t, 16> assetBytes{};
                assetBytes.back() = 1;
                resources[0] = {Assets::AssetId::FromBytes(assetBytes), UiRenderResourceRevision::Create(1).Value(),
                                UiRenderResourceRole::FontFace};
            }

            CaptionPaintCue Prepare(const CaptionCueId id = {1, 1}) const {
                return {{id, {6400, 2560}, {}},
                        tree.Find(Stable<UiElementId>(2)).Value(),
                        queue->Catalog(),
                        appearance,
                        localization,
                        glyphs,
                        runs,
                        resources};
            }

            UiRenderSnapshotDescriptor RenderDescriptor(const std::uint64_t revision = 1) const {
                return {tree.Instance(),
                        tree.Canvas(),
                        tree.SourceDocument(),
                        tree.SourceDocumentRevision(),
                        tree.Revision(),
                        UiInteractionRevision::Create(7).Value(),
                        UiRenderSnapshotRevision::Create(revision).Value(),
                        view,
                        limits};
            }

            void Enqueue(const CaptionCueId id = {1, 1}, const CaptionCueKind kind = CaptionCueKind::Dialogue,
                         const std::uint64_t start = 10) {
                REQUIRE(queue->TryEnqueue({id, kind, 0, 0, start, 20}) == CaptionAdmission::Accepted);
            }
        };
    }  // namespace

    TEST_CASE("Caption presenter extracts canonical background and text independently of audio or display",
              "[runtime_ui][captions][render][allocation]") {
        CaptionPaintFixture fixture;
        fixture.Enqueue();
        fixture.Enqueue({1, 2}, CaptionCueKind::Sound);
        fixture.Enqueue({1, 3}, CaptionCueKind::Dialogue, 20);
        fixture.timeline.BeginFrame(10);
        REQUIRE(fixture.timeline.Drain(*fixture.queue) == 0);
        auto renderer = CaptionRenderer::Create({fixture.view, fixture.limits, 2}).Value();
        const std::array prepared{fixture.Prepare(), fixture.Prepare({1, 2}), fixture.Prepare({1, 3})};
        const auto before = accessibilityAllocations.load();
        auto rendered = renderer.Render(fixture.tree, fixture.RenderDescriptor(), {{0, 0}, {1280 * 64, 720 * 64}}, fixture.timeline,
                                        fixture.appearance, fixture.localization, prepared);
        const auto after = accessibilityAllocations.load();
        REQUIRE(rendered.HasValue());
        REQUIRE(after == before);
        const auto &frame = rendered.Value();
        REQUIRE(frame.layout.count == 1);
        REQUIRE(frame.snapshot.Commands().size() == 2);
        REQUIRE(std::holds_alternative<UiSolidDraw>(frame.snapshot.Commands()[0].payload));
        REQUIRE(std::holds_alternative<UiTextDraw>(frame.snapshot.Commands()[1].payload));
        REQUIRE(frame.snapshot.Glyphs()[0].origin.x == frame.layout.placements[0].bounds.origin.x + 64);
        REQUIRE(frame.snapshot.Glyphs()[0].origin.y == frame.layout.placements[0].bounds.origin.y + 64);
        REQUIRE(frame.snapshot.Resources()[0].role == UiRenderResourceRole::FontFace);
        REQUIRE(std::get<UiSolidDraw>(frame.snapshot.Commands()[0].payload).color.alpha == Catch::Approx(0.75F));
        REQUIRE(fixture.timeline.Reset(2).HasValue());
        fixture.queue.reset();
        fixture.glyphs[0].glyph = 999;
        REQUIRE(frame.snapshot.Glyphs()[0].glyph == 17);
    }

    TEST_CASE("Caption paint snapshots outlive presenter and retained source tree without borrowing source glyph tables",
              "[runtime_ui][captions][render][lifetime]") {
        std::optional<CaptionRenderFrame> retained;
        {
            CaptionPaintFixture fixture;
            fixture.Enqueue();
            fixture.timeline.BeginFrame(10);
            REQUIRE(fixture.timeline.Drain(*fixture.queue) == 0);
            auto renderer = CaptionRenderer::Create({fixture.view, fixture.limits, 2}).Value();
            const std::array prepared{fixture.Prepare()};
            auto rendered = renderer.Render(fixture.tree, fixture.RenderDescriptor(), {{0, 0}, {1280 * 64, 720 * 64}}, fixture.timeline,
                                            fixture.appearance, fixture.localization, prepared);
            REQUIRE(rendered.HasValue());
            retained.emplace(std::move(rendered).Value());
        }
        REQUIRE(retained->snapshot.IsValid());
        REQUIRE(retained->snapshot.Glyphs()[0].glyph == 17);
        REQUIRE(retained->snapshot.TextRuns()[0].glyphCount == 1);
    }

    TEST_CASE("Caption appearance changes require coherent prepared text and preserve failed publication revision for retry",
              "[runtime_ui][captions][render]") {
        CaptionPaintFixture fixture;
        fixture.Enqueue();
        fixture.timeline.BeginFrame(10);
        REQUIRE(fixture.timeline.Drain(*fixture.queue) == 0);
        auto renderer = CaptionRenderer::Create({fixture.view, fixture.limits, 2}).Value();
        std::array prepared{fixture.Prepare()};
        fixture.appearance.textColor = {0.5F, 0, 0, 1};
        fixture.appearance.backgroundOpacity = 0.5F;
        const UiLogicalRect viewport{{0, 0}, {1280 * 64, 720 * 64}};
        REQUIRE(renderer
                    .Render(fixture.tree, fixture.RenderDescriptor(), viewport, fixture.timeline, fixture.appearance, fixture.localization,
                            prepared)
                    .HasError());
        prepared[0] = fixture.Prepare();
        auto rendered = renderer.Render(fixture.tree, fixture.RenderDescriptor(), viewport, fixture.timeline, fixture.appearance,
                                        fixture.localization, prepared);
        REQUIRE(rendered.HasValue());
        REQUIRE(rendered.Value().snapshot.TextRuns()[0].color.red == Catch::Approx(0.21404114F));
        REQUIRE(rendered.Value().localization == fixture.localization);
        REQUIRE(std::get<UiSolidDraw>(rendered.Value().snapshot.Commands()[0].payload).color.alpha == Catch::Approx(0.5F));
        fixture.appearance.enabled = false;
        const auto disabled = renderer.Render(fixture.tree, fixture.RenderDescriptor(2), viewport, fixture.timeline, fixture.appearance,
                                              fixture.localization, prepared);
        REQUIRE(disabled.HasValue());
        REQUIRE(disabled.Value().snapshot.Commands().empty());
    }

    TEST_CASE("Caption presenter rejects foreign catalog leases duplicates invalid glyphs and capacity overflow",
              "[runtime_ui][captions][render]") {
        CaptionPaintFixture fixture;
        fixture.Enqueue();
        fixture.timeline.BeginFrame(10);
        REQUIRE(fixture.timeline.Drain(*fixture.queue) == 0);
        auto renderer = CaptionRenderer::Create({fixture.view, fixture.limits, 2}).Value();
        const UiLogicalRect viewport{{0, 0}, {1280 * 64, 720 * 64}};
        std::array prepared{fixture.Prepare()};
        prepared[0].catalog = std::make_shared<const std::vector<LocalizedMessageRef>>(*prepared[0].catalog);
        REQUIRE(renderer
                    .Render(fixture.tree, fixture.RenderDescriptor(), viewport, fixture.timeline, fixture.appearance, fixture.localization,
                            prepared)
                    .HasError());
        prepared[0] = fixture.Prepare();
        prepared[0].localization.bytes[0] = 2;
        REQUIRE(renderer
                    .Render(fixture.tree, fixture.RenderDescriptor(), viewport, fixture.timeline, fixture.appearance, fixture.localization,
                            prepared)
                    .HasError());
        prepared[0] = fixture.Prepare();
        const std::array duplicate{prepared[0], prepared[0]};
        REQUIRE(renderer
                    .Render(fixture.tree, fixture.RenderDescriptor(), viewport, fixture.timeline, fixture.appearance, fixture.localization,
                            duplicate)
                    .HasError());
        fixture.glyphs[0].origin.x = 6400;
        REQUIRE(renderer
                    .Render(fixture.tree, fixture.RenderDescriptor(), viewport, fixture.timeline, fixture.appearance, fixture.localization,
                            prepared)
                    .HasError());
        fixture.glyphs[0].origin.x = 64;
        fixture.runs[0].fontResource = 1;
        REQUIRE(renderer
                    .Render(fixture.tree, fixture.RenderDescriptor(), viewport, fixture.timeline, fixture.appearance, fixture.localization,
                            prepared)
                    .HasError());
        fixture.runs[0].fontResource = 0;
        fixture.resources[0].role = UiRenderResourceRole::Image;
        REQUIRE(renderer
                    .Render(fixture.tree, fixture.RenderDescriptor(), viewport, fixture.timeline, fixture.appearance, fixture.localization,
                            prepared)
                    .HasError());
        fixture.resources[0].role = UiRenderResourceRole::FontFace;
        fixture.Enqueue({1, 2});
        REQUIRE(fixture.timeline.Drain(*fixture.queue) == 0);
        auto small = fixture.limits;
        small.commands = 2;
        auto boundedRenderer = CaptionRenderer::Create({fixture.view, small, 2}).Value();
        auto descriptor = fixture.RenderDescriptor();
        descriptor.limits = small;
        const std::array two{fixture.Prepare(), fixture.Prepare({1, 2})};
        REQUIRE(boundedRenderer.Render(fixture.tree, descriptor, viewport, fixture.timeline, fixture.appearance, fixture.localization, two)
                    .HasError());
    }

    TEST_CASE("Canonical caption paint remains within safe area across reference viewport aspect ratios",
              "[runtime_ui][captions][render][safe_area]") {
        CaptionPaintFixture fixture;
        fixture.Enqueue();
        fixture.timeline.BeginFrame(10);
        REQUIRE(fixture.timeline.Drain(*fixture.queue) == 0);
        auto renderer = CaptionRenderer::Create({fixture.view, fixture.limits, 2}).Value();
        const std::array prepared{fixture.Prepare()};
        std::uint64_t revision = 0;
        for (const UiLogicalExtent extent :
             {UiLogicalExtent{1920 * 64, 1080 * 64}, UiLogicalExtent{1024 * 64, 768 * 64}, UiLogicalExtent{3440 * 64, 1440 * 64}}) {
            const auto rendered = renderer.Render(fixture.tree, fixture.RenderDescriptor(++revision), {{0, 0}, extent}, fixture.timeline,
                                                  fixture.appearance, fixture.localization, prepared);
            REQUIRE(rendered.HasValue());
            const auto &safe = rendered.Value().layout.safeArea;
            for (const auto &command : rendered.Value().snapshot.Commands()) {
                REQUIRE(command.rect.origin.x >= safe.origin.x);
                REQUIRE(command.rect.origin.y >= safe.origin.y);
                REQUIRE(command.rect.origin.x + command.rect.extent.width <= safe.origin.x + safe.extent.width);
                REQUIRE(command.rect.origin.y + command.rect.extent.height <= safe.origin.y + safe.extent.height);
            }
        }
    }
}  // namespace Horo::Runtime::Ui
