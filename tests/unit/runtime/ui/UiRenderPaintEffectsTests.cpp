#include "Horo/Runtime/Ui/UiRenderSnapshot.h"
#include "UiRenderSnapshotTestSupport.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Runtime::Ui {
    namespace {
        /** @brief Real issued tree and bounded extractor; every test publishes through public transactional admission. */
        struct PaintFixture final {
            UiOwnershipGeneration ownership = UiOwnershipGeneration::Create(73).Value();
            UiElementSlotAllocator allocator = UiElementSlotAllocator::Create(ownership).Value();
            UiElementTree tree;
            UiRenderSnapshotLimits limits{4, 0, 0, 0, 4, 1, 0, 4};
            UiRenderViewId view{ownership, 9, 1};
            UiRenderExtractor extractor;
            std::array<UiLogicalTransform, 1> transforms{};
            std::array<UiDrawCommand, 4> commands{};
            std::uint64_t nextRevision{1};

            static UiElementTree MakeTree(UiElementSlotAllocator &allocator, UiOwnershipGeneration ownership) {
                SerializedUiId documentBytes{};
                documentBytes.back() = 1;
                SerializedUiId elementBytes{};
                elementBytes.back() = 2;
                const UiElementTreeDescriptor descriptor{{ownership, 1, 1},
                                                         {ownership, 2, 1},
                                                         UiDocumentId::Create(documentBytes).Value(),
                                                         UiDocumentRevision::Create(1).Value(),
                                                         UiRuntimeTreeRevision::Create(1).Value(),
                                                         {4, 4, 4}};
                const std::array elements{UiElementDescriptor{UiElementId::Create(elementBytes).Value(), {}}};
                return UiElementTree::Create(allocator, descriptor, elements).Value();
            }

            PaintFixture() : tree(MakeTree(allocator, ownership)), extractor(UiRenderExtractor::Create({view, limits, 2}).Value()) {
                for (auto &command : commands)
                    command = {tree.Root().Value().handle,           {{0, 0}, {640, 640}}, 0, NoUiRenderIndex, NoUiRenderIndex, 1.0F,
                               UiSolidDraw{{1.0F, 0.0F, 0.0F, 1.0F}}};
            }

            Result<UiRenderSnapshot> Extract(std::span<const UiOpacityGroup> groups = {}, std::span<const UiMask> masks = {}) {
                const auto descriptor = Test::SnapshotDescriptor(tree, UiInteractionRevision::Create(1).Value(),
                                                                 UiRenderSnapshotRevision::Create(nextRevision++).Value(), view, limits);
                return extractor.Extract(tree, descriptor, {commands, {}, {}, {}, masks, transforms, {}, groups});
            }

            UiOpacityGroup Group(std::uint32_t first, std::uint32_t count, std::uint32_t parent = NoUiRenderIndex) {
                return {tree.Root().Value().handle, {{0, 0}, {640, 640}}, first, count, parent, 0.5F};
            }
        };

        TEST_CASE("UI isolated groups retain nested paint intervals without distributing opacity", "[runtime_ui][render_snapshot]") {
            PaintFixture fixture;
            const std::array groups{fixture.Group(0, 4), fixture.Group(1, 2, 0)};
            fixture.commands[0].group = 0;
            fixture.commands[1].group = fixture.commands[2].group = 1;
            fixture.commands[3].group = 0;
            auto result = fixture.Extract(groups);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().Groups().size() == 2);
            REQUIRE(result.Value().Groups()[1].parent == 0);
            REQUIRE(result.Value().Groups()[0].opacity == 0.5F);
            REQUIRE(result.Value().Commands()[1].opacity == 1.0F);
        }

        TEST_CASE("UI isolation rejects crossing intervals and incorrect deepest membership", "[runtime_ui][render_snapshot]") {
            PaintFixture fixture;
            auto groups = std::array{fixture.Group(0, 2), fixture.Group(1, 3, 0)};
            fixture.commands[0].group = 0;
            fixture.commands[1].group = 1;
            REQUIRE(fixture.Extract(groups).HasError());
            groups[1].commandCount = 1;
            fixture.commands[1].group = 0;
            REQUIRE(fixture.Extract(groups).HasError());
            fixture.commands[1].group = 1;
            REQUIRE(fixture.Extract(groups).HasValue());
        }

        TEST_CASE("UI analytic masks retain bounded ancestry and reject resource ambiguity", "[runtime_ui][render_snapshot]") {
            PaintFixture fixture;
            auto masks = std::array{UiMask{{{0, 0}, {640, 640}}, NoUiRenderIndex, 0, UiMaskShape::Rectangle},
                                    UiMask{{{64, 64}, {320, 320}}, NoUiRenderIndex, 0, UiMaskShape::RoundedRectangle, 0, {32, 32, 32, 32}}};
            fixture.commands[0].mask = 1;
            auto result = fixture.Extract({}, masks);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().Masks()[1].parent == 0);
            masks[0].parent = 1;
            REQUIRE(fixture.Extract({}, masks).HasError());
            masks[0].parent = NoUiRenderIndex;
            masks[1].resource = 0;
            REQUIRE(fixture.Extract({}, masks).HasError());
        }

        TEST_CASE("UI isolation slot exhaustion preserves live snapshot leases", "[runtime_ui][render_snapshot]") {
            PaintFixture fixture;
            auto group = fixture.Group(0, 4);
            for (auto &command : fixture.commands)
                command.group = 0;
            auto retained = fixture.Extract(std::span{&group, 1});
            REQUIRE(retained.HasValue());
            group.opacity = 0.25F;
            auto replacement = fixture.Extract(std::span{&group, 1});
            REQUIRE(replacement.HasValue());
            REQUIRE(retained.Value().Groups()[0].opacity == 0.5F);
            REQUIRE(replacement.Value().Groups()[0].opacity == 0.25F);
            REQUIRE(fixture.Extract(std::span{&group, 1}).HasError());
            fixture.extractor.Close();
            REQUIRE(retained.Value().Groups()[0].opacity == 0.5F);
        }

        TEST_CASE("UI rounded masks reject negative and overlapping corner radii", "[runtime_ui][render_snapshot]") {
            PaintFixture fixture;
            UiMask mask{{{0, 0}, {640, 320}}, NoUiRenderIndex, 0, UiMaskShape::RoundedRectangle, NoUiRenderIndex, {160, 160, 160, 160}};
            REQUIRE(fixture.Extract({}, std::span{&mask, 1}).HasValue());
            mask.cornerRadii[0] = -1;
            REQUIRE(fixture.Extract({}, std::span{&mask, 1}).HasError());
            mask.cornerRadii = {320, 320, 0, 1};
            REQUIRE(fixture.Extract({}, std::span{&mask, 1}).HasError());
        }

    }  // namespace
}  // namespace Horo::Runtime::Ui
