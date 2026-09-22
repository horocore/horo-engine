#include "Horo/Runtime/Ui/UiRenderGeometry.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Runtime::Ui {
    namespace {
        SerializedUiId StableBytes(const std::uint8_t marker) {
            SerializedUiId bytes{};
            bytes.back() = marker;
            return bytes;
        }

        UiOwnershipGeneration Owner() {
            return UiOwnershipGeneration::Create(17).Value();
        }

        UiDocumentId Document() {
            return UiDocumentId::Create(StableBytes(1)).Value();
        }

        UiElementId Element(const std::uint8_t marker) {
            return UiElementId::Create(StableBytes(marker)).Value();
        }

        Assets::AssetId Asset(const std::uint8_t marker) {
            std::array<std::uint8_t, 16> bytes{};
            bytes.back() = marker;
            return Assets::AssetId::FromBytes(bytes);
        }

        UiElementTree MakeTree() {
            const UiElementTreeDescriptor descriptor{.instance = {Owner(), 1, 1},
                                                     .canvas = {Owner(), 2, 1},
                                                     .document = Document(),
                                                     .documentRevision = UiDocumentRevision::Create(4).Value(),
                                                     .treeRevision = UiRuntimeTreeRevision::Create(6).Value(),
                                                     .limits = {2, 2, 2}};
            auto allocatorResult = UiElementSlotAllocator::Create(Owner());
            REQUIRE(allocatorResult.HasValue());
            auto allocator = std::move(allocatorResult).Value();
            const std::array elements{UiElementDescriptor{Element(2), {}}, UiElementDescriptor{Element(3), Element(2)}};
            auto treeResult = UiElementTree::Create(allocator, descriptor, elements);
            REQUIRE(treeResult.HasValue());
            return std::move(treeResult).Value();
        }

        std::array<UiRenderResourceReference, 2> MakeResources() {
            return {UiRenderResourceReference{Asset(1), UiRenderResourceRevision::Create(3).Value(), UiRenderResourceRole::Image},
                    UiRenderResourceReference{Asset(2), UiRenderResourceRevision::Create(3).Value(), UiRenderResourceRole::FontFace}};
        }

        std::array<UiPositionedGlyph, 2> MakeGlyphs() {
            return {UiPositionedGlyph{17, 0, {320, 0}, {32, 16}, {0.0F, 0.0F, 0.5F, 1.0F}},
                    UiPositionedGlyph{18, 1, {352, 0}, {32, 16}, {0.5F, 0.0F, 1.0F, 1.0F}}};
        }

        UiDrawCommand MakeNineSliceCommand(const UiElementHandle child) {
            return UiDrawCommand{child,
                                 {{400, 0}, {128, 96}},
                                 0,
                                 NoUiRenderIndex,
                                 NoUiRenderIndex,
                                 1.0F,
                                 UiNineSliceDraw{0, {0.0F, 0.0F, 1.0F, 1.0F}, {64, 64}, {8, 8, 8, 8}, {1.0F, 1.0F, 1.0F, 1.0F}}};
        }

        void AddSolidAndBorderCommands(std::vector<UiDrawCommand> &commands, const UiElementHandle root, const std::int32_t borderWidth) {
            commands.push_back(
                UiDrawCommand{root, {{0, 0}, {100, 50}}, 0, NoUiRenderIndex, NoUiRenderIndex, 1.0F, UiSolidDraw{{0.1F, 0.2F, 0.3F, 1.0F}}});
            commands.push_back(UiDrawCommand{root,
                                             {{100, 0}, {100, 50}},
                                             0,
                                             NoUiRenderIndex,
                                             NoUiRenderIndex,
                                             1.0F,
                                             UiSolidDraw{{0.4F, 0.5F, 0.6F, 1.0F}}});
            commands.push_back(UiDrawCommand{root,
                                             {{0, 0}, {200, 50}},
                                             0,
                                             NoUiRenderIndex,
                                             NoUiRenderIndex,
                                             1.0F,
                                             UiBorderDraw{{1.0F, 1.0F, 1.0F, 1.0F}, borderWidth}});
        }

        void AddImageCommands(std::vector<UiDrawCommand> &commands, const UiElementHandle child) {
            commands.push_back(UiDrawCommand{child,
                                             {{0, 64}, {64, 64}},
                                             0,
                                             NoUiRenderIndex,
                                             NoUiRenderIndex,
                                             0.75F,
                                             UiImageDraw{0, {1.0F, 1.0F, 1.0F, 1.0F}}});
            commands.push_back(UiDrawCommand{child,
                                             {{64, 64}, {64, 64}},
                                             0,
                                             NoUiRenderIndex,
                                             NoUiRenderIndex,
                                             0.75F,
                                             UiImageDraw{0, {0.8F, 0.8F, 0.8F, 1.0F}}});
            commands.push_back(UiDrawCommand{child,
                                             {{128, 64}, {64, 64}},
                                             0,
                                             NoUiRenderIndex,
                                             NoUiRenderIndex,
                                             1.0F,
                                             UiSpriteDraw{0, {0.25F, 0.0F, 0.75F, 1.0F}, {1.0F, 1.0F, 1.0F, 1.0F}}});
        }

        void AddTextCommand(std::vector<UiDrawCommand> &commands, const UiElementHandle child) {
            commands.push_back(UiDrawCommand{child, {{320, 0}, {64, 16}}, 0, NoUiRenderIndex, NoUiRenderIndex, 1.0F, UiTextDraw{0}});
        }

        std::vector<UiDrawCommand> MakeCommands(const UiElementHandle root, const UiElementHandle child, const std::int32_t borderWidth,
                                                const bool includeNineSlice = false) {
            std::vector<UiDrawCommand> commands;
            AddSolidAndBorderCommands(commands, root, borderWidth);
            AddImageCommands(commands, child);
            AddTextCommand(commands, child);
            if (includeNineSlice)
                commands.push_back(MakeNineSliceCommand(child));
            return commands;
        }

        UiRenderSnapshot MakeSnapshot(const std::uint64_t revision, const std::int32_t borderWidth = 4,
                                      const bool includeNineSlice = false) {
            auto tree = MakeTree();
            const auto root = tree.Root().Value().handle;
            const auto child = tree.Find(Element(3)).Value();
            const UiRenderSnapshotLimits limits{includeNineSlice ? 8U : 7U, 1, 2, 0, 0, 1, 2};
            auto extractorResult = UiRenderExtractor::Create({{Owner(), 9, 1}, limits, 1});
            REQUIRE(extractorResult.HasValue());
            auto extractor = std::move(extractorResult).Value();

            const auto resources = MakeResources();
            const std::array transforms{UiLogicalTransform{}};
            const auto glyphs = MakeGlyphs();
            const std::array textRuns{UiTextRun{1, 0, 2, {1.0F, 1.0F, 1.0F, 1.0F}}};
            const std::array<UiClip, 0> clips{};
            const std::array<UiMask, 0> masks{};
            const auto commands = MakeCommands(root, child, borderWidth, includeNineSlice);
            const UiRenderSnapshotDescriptor descriptor{.instance = tree.Instance(),
                                                        .canvas = tree.Canvas(),
                                                        .document = tree.SourceDocument(),
                                                        .documentRevision = tree.SourceDocumentRevision(),
                                                        .treeRevision = tree.Revision(),
                                                        .interactionRevision = UiInteractionRevision::Create(8).Value(),
                                                        .snapshotRevision = UiRenderSnapshotRevision::Create(revision).Value(),
                                                        .view = {Owner(), 9, 1},
                                                        .limits = limits};
            auto snapshot = extractor.Extract(tree, descriptor, {commands, textRuns, glyphs, clips, masks, transforms, resources});
            REQUIRE(snapshot.HasValue());
            return std::move(snapshot).Value();
        }

        UiRenderGeometryArena MakeArena(const std::uint32_t concurrentPlans = 2, const std::uint32_t vertices = 64,
                                        const std::uint32_t indices = 96) {
            auto result = UiRenderGeometryArena::Create({{Owner(), 9, 1}, {vertices, indices, 16}, concurrentPlans});
            REQUIRE(result.HasValue());
            return std::move(result).Value();
        }

        TEST_CASE("UI geometry generates indexed primitives and preserves authored batch order", "[runtime_ui][render_geometry]") {
            const auto snapshot = MakeSnapshot(2);
            auto arena = MakeArena();
            auto planResult = arena.Build(snapshot);
            REQUIRE(planResult.HasValue());
            auto plan = std::move(planResult).Value();

            REQUIRE(plan.Descriptor().snapshotRevision == UiRenderSnapshotRevision::Create(2).Value());
            REQUIRE(plan.Descriptor().commandCount == 7);
            REQUIRE(plan.Vertices().size() == 44);
            REQUIRE(plan.Indices().size() == 66);
            REQUIRE(plan.Batches().size() == 5);
            REQUIRE(plan.SourceSnapshot().Descriptor().view == snapshot.Descriptor().view);
            REQUIRE(plan.Vertices()[0].x == 0.0F);
            REQUIRE(plan.Vertices()[0].y == 0.0F);
            REQUIRE(plan.Vertices()[2].x == 100.0F);
            REQUIRE(plan.Vertices()[2].y == 50.0F);
            REQUIRE(plan.Vertices()[24].color.alpha == 0.75F);
            REQUIRE(plan.Vertices()[32].u == 0.25F);
            REQUIRE(plan.Vertices()[34].u == 0.75F);
            REQUIRE(plan.Vertices()[36].x == 320.0F);
            REQUIRE(plan.Vertices()[42].x == 384.0F);

            REQUIRE(plan.Batches()[0].key.primitive == UiRenderGeometryPrimitive::SolidRectangle);
            REQUIRE(plan.Batches()[0].firstCommand == 0);
            REQUIRE(plan.Batches()[0].commandCount == 2);
            REQUIRE(plan.Batches()[1].key.primitive == UiRenderGeometryPrimitive::BorderRectangle);
            REQUIRE(plan.Batches()[1].firstCommand == 2);
            REQUIRE(plan.Batches()[2].key.primitive == UiRenderGeometryPrimitive::ImageRectangle);
            REQUIRE(plan.Batches()[2].firstCommand == 3);
            REQUIRE(plan.Batches()[2].commandCount == 2);
            REQUIRE(plan.Batches()[3].key.primitive == UiRenderGeometryPrimitive::SpriteRectangle);
            REQUIRE(plan.Batches()[3].firstCommand == 5);
            REQUIRE(plan.Batches()[4].key.primitive == UiRenderGeometryPrimitive::TextGlyphs);
            REQUIRE(plan.Batches()[4].firstCommand == 6);

            for (const auto &vertex : plan.Vertices())
                REQUIRE(vertex.IsValid());
            for (const auto index : plan.Indices())
                REQUIRE(index < plan.Vertices().size());
            for (const auto &batch : plan.Batches())
                REQUIRE(batch.IsValid(plan.Vertices().size(), plan.Indices().size(), plan.Descriptor().commandCount));
        }

        TEST_CASE("Pixel snapping is deterministic, safe-area aware, and derived from logical render points",
                  "[runtime_ui][render_geometry][pixel_snap]") {
            const UiResolvedScreenCanvas canvas{{12800, 14400},        {405, 457}, {2, 1}, {5, 7, 400, 450}, {5, 7, 0, 0}, {}, {}, {},
                                                UiPixelSnapMode::Edges};
            const auto halfPixel = SnapUiPointToPixels(canvas, 16.0F, 16.0F);
            REQUIRE(halfPixel.HasValue());
            REQUIRE(halfPixel.Value() == UiPixelSnappedPoint{32.0F, 32.0F});

            const auto disabled = SnapUiPointToPixels(UiResolvedScreenCanvas{{12800, 14400},
                                                                             {400, 450},
                                                                             {2, 1},
                                                                             {},
                                                                             {},
                                                                             {},
                                                                             {},
                                                                             {},
                                                                             UiPixelSnapMode::Disabled},
                                                      16.0F, 16.0F);
            REQUIRE(disabled.HasValue());
            REQUIRE(disabled.Value() == UiPixelSnappedPoint{16.0F, 16.0F});

            const auto malformed = SnapUiPointToPixels(canvas, 0.0F, std::numeric_limits<float>::infinity());
            REQUIRE(malformed.HasError());
            REQUIRE(malformed.ErrorValue().code.Value() == UiErrors::CanvasSpaceInvalid.code.Value());

            const auto snapshot = MakeSnapshot(2);
            auto arena = MakeArena();
            const UiResolvedScreenCanvas buildCanvas{{25600, 25600},        {400, 400}, {1, 1}, {0, 0, 400, 400}, {}, {}, {}, {},
                                                     UiPixelSnapMode::Edges};
            const auto planResult = arena.Build(snapshot, buildCanvas);
            REQUIRE(planResult.HasValue());
            const auto plan = std::move(planResult).Value();
            REQUIRE(plan.Descriptor().presentation.has_value());
            REQUIRE(plan.Descriptor().presentation->pixelSnap == UiPixelSnapMode::Edges);
            REQUIRE(plan.Vertices()[2].x == 128.0F);
            REQUIRE(plan.Vertices()[2].y == 64.0F);
        }

        TEST_CASE("UI geometry clamps oversized border strips without edge overlap", "[runtime_ui][render_geometry][edge]") {
            const auto snapshot = MakeSnapshot(2, 200);
            auto arena = MakeArena();
            auto planResult = arena.Build(snapshot);
            REQUIRE(planResult.HasValue());
            const auto plan = std::move(planResult).Value();

            REQUIRE(plan.Vertices()[8].y == 0.0F);
            REQUIRE(plan.Vertices()[10].y == 25.0F);
            REQUIRE(plan.Vertices()[12].y == 25.0F);
            REQUIRE(plan.Vertices()[14].y == 50.0F);
            REQUIRE(plan.Vertices()[16].y == 25.0F);
            REQUIRE(plan.Vertices()[17].y == 25.0F);
        }

        TEST_CASE("UI geometry expands a nine-slice image into nine bounded quads", "[runtime_ui][render_geometry][image]") {
            const auto snapshot = MakeSnapshot(2, 4, true);
            auto arena = MakeArena(2, 128, 192);
            auto planResult = arena.Build(snapshot);
            REQUIRE(planResult.HasValue());
            const auto plan = std::move(planResult).Value();

            REQUIRE(plan.Descriptor().commandCount == 8);
            REQUIRE(plan.Vertices().size() == 80);
            REQUIRE(plan.Indices().size() == 120);
            REQUIRE(plan.Batches().size() == 6);
            REQUIRE(plan.Batches()[5].key.primitive == UiRenderGeometryPrimitive::NineSliceRectangle);
            REQUIRE(plan.Batches()[5].firstCommand == 7);
            REQUIRE(plan.Batches()[5].vertexCount == 36);
            REQUIRE(plan.Batches()[5].indexCount == 54);
            REQUIRE(plan.Vertices()[44].x == 400.0F);
            REQUIRE(plan.Vertices()[44].y == 0.0F);
            REQUIRE(plan.Vertices()[46].x == 408.0F);
            REQUIRE(plan.Vertices()[46].y == 8.0F);
            REQUIRE(plan.Vertices()[78].x == 528.0F);
            REQUIRE(plan.Vertices()[78].y == 96.0F);
        }

        TEST_CASE("UI geometry rejects capacity pressure without fallback allocation", "[runtime_ui][render_geometry][limits]") {
            const auto snapshot = MakeSnapshot(2);
            auto arenaResult = UiRenderGeometryArena::Create({{Owner(), 9, 1}, {8, 12, 4}, 1});
            REQUIRE(arenaResult.HasValue());
            auto arena = std::move(arenaResult).Value();
            const auto failed = arena.Build(snapshot);
            REQUIRE(failed.HasError());
            REQUIRE(failed.ErrorValue().code.Value() == UiErrors::RenderGeometryCapacityExceeded.code.Value());
            REQUIRE(arena.Statistics().failedBuilds == 1);
            REQUIRE(arena.Statistics().activeLeases == 0);
            REQUIRE(arena.IsDrained());

            const auto invalid = UiRenderGeometryArena::Create({{Owner(), 9, 1}, {0, 12, 4}, 1});
            REQUIRE(invalid.HasError());
        }

        TEST_CASE("UI geometry reports leased plan-slot exhaustion", "[runtime_ui][render_geometry][limits]") {
            const auto snapshot = MakeSnapshot(2);
            auto arena = MakeArena(1);
            auto retainedResult = arena.Build(snapshot);
            REQUIRE(retainedResult.HasValue());
            std::optional<UiRenderGeometryPlan> retained{std::move(retainedResult).Value()};

            const auto failed = arena.Build(snapshot);
            REQUIRE(failed.HasError());
            REQUIRE(failed.ErrorValue().code.Value() == UiErrors::RenderGeometryStorageExhausted.code.Value());
            REQUIRE(arena.Statistics().failedBuilds == 1);
            REQUIRE(arena.Statistics().activeLeases == 1);

            retained.reset();
            REQUIRE(arena.Build(snapshot).HasValue());
        }

        TEST_CASE("UI geometry plans retain source leases across reload and shutdown", "[runtime_ui][render_geometry][lifecycle]") {
            const auto firstSnapshot = MakeSnapshot(2);
            const auto replacementSnapshot = MakeSnapshot(3);
            std::optional<UiRenderGeometryPlan> retained;
            {
                auto arena = MakeArena();
                auto firstResult = arena.Build(firstSnapshot);
                REQUIRE(firstResult.HasValue());
                retained.emplace(std::move(firstResult).Value());
                auto replacementResult = arena.Build(replacementSnapshot);
                REQUIRE(replacementResult.HasValue());
                auto replacement = std::move(replacementResult).Value();
                REQUIRE(replacement.SourceSnapshot().Descriptor().snapshotRevision == UiRenderSnapshotRevision::Create(3).Value());
                REQUIRE(arena.Statistics().activeLeases == 2);
                arena.Close();
                REQUIRE(arena.State() == UiRenderGeometryArenaState::Closed);
                REQUIRE(arena.IsDrained() == false);
                REQUIRE(arena.Build(replacementSnapshot).HasError());
                REQUIRE(arena.Statistics().failedBuilds == 1);
                REQUIRE(retained->Vertices().size() == 44);
            }
            REQUIRE(retained->SourceSnapshot().IsValid());
            REQUIRE(retained->Descriptor().snapshotRevision == UiRenderSnapshotRevision::Create(2).Value());
            retained.reset();
        }

        static_assert(!std::is_copy_constructible_v<UiRenderGeometryArena>);
        static_assert(std::is_copy_constructible_v<UiRenderGeometryPlan>);
    }  // namespace
}  // namespace Horo::Runtime::Ui
