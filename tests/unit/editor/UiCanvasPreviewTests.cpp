#include "Horo/Runtime/Ui/UiDocumentSerialization.h"
#include "Horo/Runtime/Ui/UiStyleAsset.h"
#include "UiCanvasPreview.h"

#include <catch2/catch_test_macros.hpp>
#include <limits>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Runtime::Ui;

    template <typename T> T Id(std::uint8_t value) {
        SerializedUiId bytes{};
        bytes.back() = value;
        return T::Create(bytes).Value();
    }

    UiDocument Document(bool profiles = false) {
        UiDocumentBuilder builder{Id<UiDocumentId>(1), UiDocumentRevision::Create(1).Value()};
        REQUIRE(builder
                    .AddCanvas({.id = Id<UiCanvasId>(2),
                                .rootElement = Id<UiElementId>(3),
                                .referenceResolution = {800, 600},
                                .presentation = {.safeArea = UiSafeAreaMode::Inset}})
                    .HasValue());
        REQUIRE(builder.AddElement(CreateUiElement(UiElementKind::Canvas, Id<UiElementId>(3), {}).Value()).HasValue());
        auto panel = CreateUiElement(UiElementKind::Panel, Id<UiElementId>(4), Id<UiElementId>(3)).Value();
        panel.presentation->layout.positioning = UiLayoutPositioning::Absolute;
        panel.presentation->layout.width = UiLength::Dip(100 * 64);
        panel.presentation->layout.height = UiLength::Dip(60 * 64);
        panel.presentation->layout.offsets.left = 20 * 64;
        panel.presentation->layout.offsets.top = 30 * 64;
        REQUIRE(builder.AddElement(panel).HasValue());
        if (profiles) {
            auto compact = panel.presentation->layout;
            compact.width = UiLength::Dip(40 * 64);
            REQUIRE(
                builder
                    .AddPresentationProfiles(
                        {Id<UiCanvasId>(2),
                         UiPresentationProfileRevision::Create(1).Value(),
                         {{Id<UiPresentationProfileId>(5), 1, {.formFactor = UiPresentationFormFactor::Handheld}, {{panel.id, compact}}}}})
                    .HasValue());
        }
        auto source = std::move(builder).Build();
        REQUIRE(source.HasValue());
        return std::move(source).Value();
    }

    DocumentInstanceId Instance() {
        DocumentIdentityRegistry identities;
        return identities.Open({DocumentKind::UiCanvas, SourceDocumentId::Parse("Hud.uicanvas").Value()}).Value().identity.instance;
    }
}  // namespace

TEST_CASE("Canvas preview evaluates real Runtime UI layout and device profiles without source mutation", "[ui_canvas][preview]") {
    const auto source = Document(true);
    const auto before = SerializeUiDocument(source).Value();
    UiCanvasPreviewState state;
    auto desktop = BuildUiCanvasPreview(source, state, Instance());
    REQUIRE(desktop.HasValue());
    REQUIRE(desktop.Value().boxes.size() == 2);
    CHECK(desktop.Value().boxes.back().bounds.extent.width == 100 * 64);
    CHECK_FALSE(desktop.Value().profile.has_value());
    state.device = UiCanvasPreviewDevice::Phone;
    auto phone = BuildUiCanvasPreview(source, state, Instance());
    REQUIRE(phone.HasValue());
    CHECK(phone.Value().viewport.dpiScale == UiCanvasDeviceScale{3, 1});
    CHECK(phone.Value().canvas.safeAreaInsets.top == 141);
    CHECK(phone.Value().profile == Id<UiPresentationProfileId>(5));
    CHECK(phone.Value().boxes.back().bounds.extent.width == 40 * 64);
    state.rotated = true;
    auto rotated = BuildUiCanvasPreview(source, state, Instance());
    REQUIRE(rotated.HasValue());
    CHECK(rotated.Value().viewport.pixelExtent == UiCanvasPixelExtent{2532, 1170});
    CHECK(rotated.Value().viewport.safeAreaInsets.left == 102);
    CHECK(SerializeUiDocument(source).Value() == before);
}

TEST_CASE("Preview zoom does not alter logical layout and invalid evidence never mutates source", "[ui_canvas][preview]") {
    const auto source = Document();
    UiCanvasPreviewState state;
    const auto baseline = BuildUiCanvasPreview(source, state, Instance());
    REQUIRE(baseline.HasValue());
    state.zoomPercent = 800;
    state.pan = {10000, -10000};
    const auto zoom = BuildUiCanvasPreview(source, state, Instance());
    REQUIRE(zoom.HasValue());
    CHECK(zoom.Value().boxes.back().bounds == baseline.Value().boxes.back().bounds);
    state.zoomPercent = 0;
    CHECK(BuildUiCanvasPreview(source, state, Instance()).HasError());
    CHECK(BuildUiCanvasPreview(source, {}, {}).HasError());
}

TEST_CASE("Visual gestures preserve canonical presentation and exact fixed point values across device DPI", "[ui_canvas][manipulation]") {
    const auto source = Document();
    const auto &panel = source.Elements().back();
    const UiLogicalRect box{{20 * 64, 30 * 64}, {100 * 64, 60 * 64}};
    UiCanvasPreviewState state;
    state.tool = UiCanvasLayoutTool::Move;
    state.snap = false;
    const auto move = PrepareUiCanvasLayoutGesture(panel, box, {7, -9}, {800 * 64, 600 * 64}, state);
    REQUIRE(move.HasValue());
    CHECK(move.Value().presentation->layout.offsets.left == 20 * 64 + 7);
    CHECK(move.Value().presentation->layout.offsets.top == 30 * 64 - 9);
    state.device = UiCanvasPreviewDevice::Phone;
    state.zoomPercent = 200;
    const auto phoneMove = PrepareUiCanvasLayoutGesture(panel, box, {7, -9}, {800 * 64, 600 * 64}, state);
    REQUIRE(phoneMove.HasValue());
    CHECK(phoneMove.Value().presentation == move.Value().presentation);
    state.tool = UiCanvasLayoutTool::Resize;
    auto resize = PrepareUiCanvasLayoutGesture(panel, box, {11, 13}, {800 * 64, 600 * 64}, state);
    REQUIRE(resize.HasValue());
    CHECK(resize.Value().presentation->layout.width == UiLength::Dip(100 * 64 + 11));
    CHECK(resize.Value().presentation->layout.height == UiLength::Dip(60 * 64 + 13));
    CHECK(resize.Value().presentation->inlineStyle == panel.presentation->inlineStyle);
    CHECK(PrepareUiCanvasLayoutGesture(panel, box, {-100 * 64 - 1, 0}, {}, state).HasError());
    state.tool = UiCanvasLayoutTool::Anchors;
    auto anchor = PrepareUiCanvasLayoutGesture(panel, box, {400 * 64, 300 * 64}, {800 * 64, 600 * 64}, state);
    REQUIRE(anchor.HasValue());
    CHECK(anchor.Value().presentation->layout.anchors.horizontal.start == 32);
    CHECK(anchor.Value().presentation->layout.anchors.vertical.start == 32);
    CHECK(PrepareUiCanvasLayoutGesture(panel, box, {1, 1}, {}, state).HasError());
}

TEST_CASE("Visual snapping is deterministic and bounded guides override the closest grid point", "[ui_canvas][manipulation]") {
    const auto source = Document();
    const auto &panel = source.Elements().back();
    UiCanvasPreviewState state;
    state.tool = UiCanvasLayoutTool::Move;
    state.verticalGuides[0] = 20 * 64 + 5;
    state.verticalGuideCount = 1;
    auto move = PrepareUiCanvasLayoutGesture(panel, {{20 * 64, 30 * 64}, {100, 100}}, {7, 0}, {}, state);
    REQUIRE(move.HasValue());
    CHECK(move.Value().presentation->layout.offsets.left == 20 * 64 + 5);
    state.verticalGuideCount = 9;
    CHECK(PrepareUiCanvasLayoutGesture(panel, {{}, {100, 100}}, {}, {}, state).HasError());
    state = {};
    state.tool = UiCanvasLayoutTool::Move;
    auto root = source.Elements().front();
    CHECK(PrepareUiCanvasLayoutGesture(root, {{}, {100, 100}}, {}, {}, state).HasError());
    auto overflow = panel;
    overflow.presentation->layout.offsets.left = std::numeric_limits<UiScalar>::max();
    state.snap = false;
    CHECK(PrepareUiCanvasLayoutGesture(overflow, {{}, {100, 100}}, {1, 0}, {}, state).HasError());
}

TEST_CASE("Authoring profile admission rejects foreign source topology and revisions", "[ui_canvas][preview][profiles]") {
    const auto source = Document(true);
    const auto owner = UiOwnershipGeneration::Create(1).Value();
    auto allocatorResult = UiElementSlotAllocator::Create(owner);
    REQUIRE(allocatorResult.HasValue());
    auto allocator = std::move(allocatorResult).Value();
    const std::array elements{UiElementDescriptor{Id<UiElementId>(3), {}}, UiElementDescriptor{Id<UiElementId>(5), Id<UiElementId>(3)}};
    auto tree = UiElementTree::Create(allocator,
                                      {{owner, 1, 1},
                                       {owner, 2, 1},
                                       source.Id(),
                                       source.Revision(),
                                       UiRuntimeTreeRevision::Create(1).Value(),
                                       {2, 4, 1}},
                                      elements);
    REQUIRE(tree.HasValue());
    CHECK(UiPresentationProfileTable::CreateForAuthoring(source, tree.Value(), Id<UiCanvasId>(2)).HasError());
    CHECK(UiPresentationProfileTable::CreateForAuthoring(source, tree.Value(), Id<UiCanvasId>(9)).HasError());
    CHECK(UiPresentationProfileTable::CreateForAuthoring(source, tree.Value(), {}).HasError());
    auto treeOwner = std::move(tree).Value();
    REQUIRE(treeOwner.BeginRetirement().HasValue());
    CHECK(UiPresentationProfileTable::CreateForAuthoring(source, treeOwner, Id<UiCanvasId>(2)).HasError());
}

TEST_CASE("Preview supports element counts above the structural command queue capacity", "[ui_canvas][preview][capacity]") {
    UiDocumentBuilder builder{Id<UiDocumentId>(1), UiDocumentRevision::Create(1).Value()};
    REQUIRE(builder.AddCanvas({.id = Id<UiCanvasId>(2), .rootElement = Id<UiElementId>(3)}).HasValue());
    REQUIRE(builder.AddElement(CreateUiElement(UiElementKind::Canvas, Id<UiElementId>(3), {}).Value()).HasValue());
    for (std::uint32_t i = 0; i < MaximumUiStructuralCommands + 1; ++i) {
        SerializedUiId bytes{};
        bytes[bytes.size() - 3] = 1;
        bytes[bytes.size() - 2] = static_cast<std::uint8_t>(i >> 8);
        bytes.back() = static_cast<std::uint8_t>(i);
        REQUIRE(builder.AddElement(CreateUiElement(UiElementKind::Panel, UiElementId::Create(bytes).Value(), Id<UiElementId>(3)).Value())
                    .HasValue());
    }
    auto source = std::move(builder).Build();
    REQUIRE(source.HasValue());
    auto preview = BuildUiCanvasPreview(source.Value(), {}, Instance());
    REQUIRE(preview.HasValue());
    CHECK(preview.Value().boxes.size() == MaximumUiStructuralCommands + 2);
}

TEST_CASE("Canvas preview uses the real selected theme computed projection after responsive bases", "[ui_canvas][preview][styles]") {
    const auto source = Document(true);
    UiStyleAssetDocument style{UiStyleContentRevision::Create(1).Value(), {.id = Id<RuntimeStyleAssetId>(8)}, {}, {}};
    style.style.assignments = {{CoreUiStylePropertyId(UiCoreStyleProperty::Width), UiStyleValueSource::Literal(UiStyleDimension{73 * 64}),
                                false},
                               {CoreUiStylePropertyId(UiCoreStyleProperty::Height), UiStyleValueSource::Literal(UiStyleDimension{29 * 64}),
                                false}};
    auto schema = CreateCoreUiStyleSchema({});
    REQUIRE(schema.HasValue());
    auto registry = PrepareUiStyleRegistry(std::span{&style, 1}, schema.Value(), RuntimeStyleGeneration::Create(1).Value());
    REQUIRE(registry.HasValue());
    UiInspectorResourceSnapshot resources;
    resources.styles = std::make_shared<RuntimeStyleRegistry>(std::move(registry).Value());
    resources.theme = style.style.id;
    resources.publication = 1;
    UiCanvasPreviewState state;
    state.device = UiCanvasPreviewDevice::Phone;
    auto preview = BuildUiCanvasPreview(source, state, Instance(), &resources);
    REQUIRE(preview.HasValue());
    CHECK(preview.Value().profile == Id<UiPresentationProfileId>(5));
    CHECK(preview.Value().boxes.back().bounds.extent.width == 73 * 64);
    CHECK(preview.Value().boxes.back().bounds.extent.height == 29 * 64);
    resources.theme = Id<RuntimeStyleAssetId>(9);
    CHECK(BuildUiCanvasPreview(source, state, Instance(), &resources).HasError());
}
