#pragma once

#include "../ui/UiRenderSnapshotTestSupport.h"
#include "Horo/Runtime/Render/UiRenderSubmission.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

namespace Horo::Render::Test {
    using namespace Runtime::Ui;

    template <typename T> T UiRequire(Result<T> result) {
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    inline SerializedUiId UiBytes(const std::uint8_t marker) {
        SerializedUiId bytes{};
        bytes.back() = marker;
        return bytes;
    }

    inline UiOwnershipGeneration UiOwner(const std::uint64_t value = 17) {
        return UiRequire(UiOwnershipGeneration::Create(value));
    }

    inline Assets::AssetId UiAsset(const std::uint8_t marker) {
        return Assets::AssetId::FromBytes(UiBytes(marker));
    }

    inline UiImageResource UiImage() {
        const std::array pages{
            UiImagePage{{UiAsset(1), UiRequire(Assets::AssetTypeId::Parse("core.texture")), true}, {4, 4}, UiImageColorSpace::Linear}};
        return UiRequire(UiImageResource::Create({UiImageResourceKind::Image, UiImageFallbackPolicy::Reject, pages, {}}));
    }

    inline UiGlyphAtlasGlyphKey UiGlyphKey(const std::uint32_t glyph = 1, const std::uint64_t revision = 3) {
        return {UiRequire(UiTextFaceId::Create(UiBytes(2))),
                glyph,
                {UiRequire(UiTextScale::Create(UiTextLayoutScaleUnit))},
                UiRequire(UiFontFaceRevision::Create(revision))};
    }

    inline UiGlyphAtlas UiAtlas(const std::uint64_t owner = 17, const std::uint32_t pages = 1) {
        return UiRequire(UiGlyphAtlas::Create({UiOwner(owner),
                                               {8, 4},
                                               {4, 4},
                                               UiGlyphAtlasRasterFormat::Rgba8,
                                               UiGlyphKey(0),
                                               {pages, 8, 64, 2, 4, 1},
                                               UiRequire(UiGlyphAtlasRevision::Create(1))}));
    }

    inline UiFontFace UiFont(const std::uint64_t revision = 3) {
        const auto path = std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "assets/fonts/inter/InterVariable.ttf";
        std::ifstream input(path, std::ios::binary);
        REQUIRE(input.good());
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
        return UiRequire(UiFontFace::Create({UiRequire(UiFontFaceId::Create(UiBytes(2))), 0, UiAsset(2),
                                             UiRequire(UiFontFaceRevision::Create(revision))},
                                            bytes));
    }

    inline UiElementTree UiTree() {
        auto allocator = UiRequire(UiElementSlotAllocator::Create(UiOwner()));
        const UiElementTreeDescriptor descriptor{.instance = {UiOwner(), 1, 1},
                                                 .canvas = {UiOwner(), 2, 1},
                                                 .document = UiRequire(UiDocumentId::Create(UiBytes(1))),
                                                 .documentRevision = UiRequire(UiDocumentRevision::Create(4)),
                                                 .treeRevision = UiRequire(UiRuntimeTreeRevision::Create(6)),
                                                 .limits = {1, 1, 1}};
        const std::array elements{UiElementDescriptor{UiRequire(UiElementId::Create(UiBytes(2))), {}}};
        return UiRequire(UiElementTree::Create(allocator, descriptor, elements));
    }

    inline UiRenderSnapshot UiSnapshot(const bool text, const std::span<const UiGlyphAtlasLookup> lookups) {
        auto tree = UiTree();
        const UiRenderSnapshotLimits limits{2, 1, 2, 0, 0, 1, 2};
        auto extractor = UiRequire(UiRenderExtractor::Create({{UiOwner(), 9, 1}, limits, 1}));
        const auto resourceRevision = UiRequire(UiRenderResourceRevision::Create(3));
        const std::array resources{UiRenderResourceReference{UiAsset(1), resourceRevision, UiRenderResourceRole::Image,
                                                             UiImageColorSpace::Linear},
                                   UiRenderResourceReference{UiAsset(2), resourceRevision, UiRenderResourceRole::FontFace}};
        const auto root = UiRequire(tree.Root()).handle;
        const std::array
            commands{UiDrawCommand{root, {{0, 0}, {64, 64}}, 0, NoUiRenderIndex, NoUiRenderIndex, 1.0F, UiImageDraw{0, {1, 1, 1, 1}}},
                     UiDrawCommand{root, {{64, 0}, {64, 64}}, 0, NoUiRenderIndex, NoUiRenderIndex, 1.0F, UiTextDraw{0}}};
        std::array<UiPositionedGlyph, 2> glyphs;
        REQUIRE(lookups.size() <= glyphs.size());
        for (std::size_t index = 0; index < lookups.size(); ++index)
            glyphs[index] = {lookups[index].resolved.glyph,
                             static_cast<std::uint32_t>(index),
                             {64, 0},
                             {64, 64},
                             lookups[index].placement.uv};
        const std::array runs{UiTextRun{1, 0, static_cast<std::uint32_t>(lookups.size()), {1, 1, 1, 1}}};
        const std::array transforms{UiLogicalTransform{}};
        const auto descriptor =
            Runtime::Ui::Test::SnapshotDescriptor(tree, UiRequire(UiInteractionRevision::Create(8)),
                                                  UiRequire(UiRenderSnapshotRevision::Create(1)), {UiOwner(), 9, 1}, limits);
        return UiRequire(extractor.Extract(tree, descriptor,
                                           {.commands = std::span{commands}.first(text ? 2 : 1),
                                            .textRuns = std::span{runs}.first(text ? 1 : 0),
                                            .glyphs = std::span{glyphs}.first(lookups.size()),
                                            .transforms = transforms,
                                            .resources = std::span{resources}.first(text ? 2 : 1)}));
    }

    inline UiGlyphAtlasLookup UiPinGlyph(UiGlyphAtlas &atlas, const UiGlyphAtlasFrameId frame,
                                         const UiGlyphAtlasGlyphKey key = UiGlyphKey(), const UiFontFace *face = nullptr) {
        const std::array<std::byte, 64> bytes{};
        const auto upload = UiRequire(atlas.RequestUpload({key, UiGlyphAtlasRasterFormat::Rgba8, 4, 4, 16, bytes, face}));
        REQUIRE(atlas.MarkSubmitted(upload, {1, 1}).HasValue());
        REQUIRE(atlas.Complete(upload).HasValue());
        return UiRequire(atlas.Resolve(frame, key));
    }

    struct UiSubmissionSources final {
        UiImageResourceRegistry images{UiRequire(UiImageResourceRegistry::Create({UiOwner(), 1}))};
        UiImageResourceHandle image{
            UiRequire(images.Publish(UiImage(), UiRequire(UiImageResourceRevision::Create(3)), UiImageResidencyState::Resident))};
        UiGlyphAtlas atlas{UiAtlas()};
        UiGlyphAtlasFrameId frame{UiRequire(atlas.BeginFrame())};
        std::optional<UiFontFace> font;
        UiRenderGeometryArena arena{UiRequire(UiRenderGeometryArena::Create({{UiOwner(), 9, 1}, {16, 24, 2}, 1}))};
        std::optional<UiRenderGeometryPlan> geometry;

        explicit UiSubmissionSources(const bool text = true, const bool twoPages = false) : atlas(UiAtlas(17, twoPages ? 2 : 1)) {
            std::array<UiGlyphAtlasLookup, 2> lookups;
            if (text) {
                font = UiFont();
                lookups[0] = UiPinGlyph(atlas, frame, UiGlyphKey(), &*font);
                if (twoPages) {
                    static_cast<void>(UiPinGlyph(atlas, frame, UiGlyphKey(2), &*font));
                    lookups[1] = UiPinGlyph(atlas, frame, UiGlyphKey(3), &*font);
                    REQUIRE(lookups[0].placement.page != lookups[1].placement.page);
                }
            }
            geometry = UiRequire(arena.Build(UiSnapshot(text, std::span{lookups}.first(text ? (twoPages ? 2 : 1) : 0))));
        }

        void CloseSources() {
            geometry.reset();
            font.reset();
            images.Close();
            arena.Close();
            atlas.Shutdown();
        }
    };
}  // namespace Horo::Render::Test
