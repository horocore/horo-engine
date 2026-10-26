#include "Horo/Runtime/Ui/UiDocumentSerialization.h"
#include "Horo/Runtime/Ui/UiStyleAsset.h"
#include "UiCanvasPreview.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
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

    UiDocument Document(bool profiles = false, const UiElementPresentation *presentation = nullptr) {
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
        if (presentation)
            panel.presentation = *presentation;
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

    UiInspectorResourceSnapshot Resources(const UiStyleAssetDocument &style) {
        auto schema = CreateCoreUiStyleSchema({});
        REQUIRE(schema.HasValue());
        auto registry = PrepareUiStyleRegistry(std::span{&style, 1}, schema.Value(), RuntimeStyleGeneration::Create(1).Value());
        REQUIRE(registry.HasValue());
        UiInspectorResourceSnapshot result;
        result.styles = std::make_shared<RuntimeStyleRegistry>(std::move(registry).Value());
        result.theme = style.style.id;
        result.publication = 1;
        return result;
    }

    UiStyleAssetDocument ZeroStyle(UiStyleOrigin origin, UiCoreStyleProperty property, UiElementPresentation &presentation) {
        UiStyleAssetDocument style{UiStyleContentRevision::Create(1).Value(), {.id = Id<RuntimeStyleAssetId>(8)}, {}, {}};
        const std::vector<UiStyleAssignment> values{
            {CoreUiStylePropertyId(property), UiStyleValueSource::Literal(UiStyleDimension{0}), false}};
        if (origin == UiStyleOrigin::StyleAsset)
            style.style.assignments = values;
        else if (origin == UiStyleOrigin::Inline)
            presentation.inlineStyle = values;
        else if (origin == UiStyleOrigin::VisualState) {
            presentation.state.enabled = false;
            style.style.states.push_back({UiVisualStateMask{UiVisualState::Disabled}, {}, UiStateLayer::Availability, values});
        } else {
            const auto id = Id<UiStyleClassId>(7);
            style.style.classes.push_back({.id = id, .assignments = values});
            if (origin == UiStyleOrigin::ElementTypeClass)
                presentation.typeClass = {style.style.id, id};
            else
                presentation.classes.push_back({style.style.id, id});
        }
        return style;
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

TEST_CASE("Preview registered layout defaults preserve authored percentages padding and responsive bases", "[ui_canvas][preview][styles]") {
    auto presentation = *Document().Elements().back().presentation;
    presentation.layout.width = UiLength::Percent(64);
    presentation.layout.height = UiLength::Percent(64);
    presentation.layout.padding = {64, 128, 192, 256};
    const auto source = Document(true, &presentation);
    const auto before = SerializeUiDocument(source).Value();
    UiStyleAssetDocument style{UiStyleContentRevision::Create(1).Value(), {.id = Id<RuntimeStyleAssetId>(8)}, {}, {}};
    const auto resources = Resources(style);
    for (const auto device : {UiCanvasPreviewDevice::Desktop, UiCanvasPreviewDevice::Phone}) {
        UiCanvasPreviewState state;
        state.device = device;
        const auto unstyled = BuildUiCanvasPreview(source, state, Instance());
        const auto styled = BuildUiCanvasPreview(source, state, Instance(), &resources);
        REQUIRE(unstyled.HasValue());
        REQUIRE(styled.HasValue());
        REQUIRE(unstyled.Value().boxes.back().bounds.extent.width > 0);
        CHECK(styled.Value().boxes.back().bounds == unstyled.Value().boxes.back().bounds);
        CHECK(styled.Value().profile == unstyled.Value().profile);
    }
    CHECK(SerializeUiDocument(source).Value() == before);
}

TEST_CASE("Preview explicit asset class inline and availability zero layout values retain precedence", "[ui_canvas][preview][styles]") {
    const auto origin = GENERATE(UiStyleOrigin::StyleAsset, UiStyleOrigin::ElementTypeClass, UiStyleOrigin::AuthoredClass,
                                 UiStyleOrigin::Inline, UiStyleOrigin::VisualState);
    const auto property = GENERATE(UiCoreStyleProperty::Width, UiCoreStyleProperty::Height, UiCoreStyleProperty::Padding);
    INFO("origin=" << static_cast<unsigned>(origin) << " property=" << static_cast<unsigned>(property));
    auto presentation = *Document().Elements().back().presentation;
    presentation.layout.padding = {64, 128, 192, 256};
    const auto style = ZeroStyle(origin, property, presentation);
    const auto source = Document(false, &presentation);
    const auto resources = Resources(style);
    const auto preview = BuildUiCanvasPreview(source, {}, Instance(), &resources);
    REQUIRE(preview.HasValue());
    if (property == UiCoreStyleProperty::Width)
        CHECK(preview.Value().boxes.back().bounds.extent.width == 0);
    else if (property == UiCoreStyleProperty::Height)
        CHECK(preview.Value().boxes.back().bounds.extent.height == 0);
    else {
        presentation.layout.padding = {};
        presentation.inlineStyle.clear();
        presentation.classes.clear();
        presentation.typeClass = {};
        const auto expected = BuildUiCanvasPreview(Document(false, &presentation), {}, Instance());
        REQUIRE(expected.HasValue());
        CHECK(preview.Value().boxes.back().bounds == expected.Value().boxes.back().bounds);
    }
}

TEST_CASE("Preview invalid session evidence never changes the canonical document", "[ui_canvas][preview][validation]") {
    const auto source = Document();
    const auto before = SerializeUiDocument(source).Value();
    std::array<UiCanvasPreviewState, 7> invalid{};
    invalid[0].device = UiCanvasPreviewDevice::Count;
    invalid[1].tool = UiCanvasLayoutTool::Count;
    invalid[2].zoomPercent = 9;
    invalid[3].zoomPercent = 801;
    invalid[4].grid = 0;
    invalid[5].horizontalGuideCount = 9;
    invalid[6].verticalGuideCount = 9;
    for (const auto &state : invalid)
        CHECK(BuildUiCanvasPreview(source, state, Instance()).HasError());
    CHECK(SerializeUiDocument(source).Value() == before);
}

TEST_CASE("Preview admits the legacy implicit root and rejects world projection without changing source", "[ui_canvas][preview][legacy]") {
    for (const auto mode : {UiRenderMode::ScreenSpaceOverlay, UiRenderMode::WorldSpace}) {
        UiDocumentBuilder builder{Id<UiDocumentId>(1), UiDocumentRevision::Create(1).Value()};
        REQUIRE(builder.AddCanvas({.id = Id<UiCanvasId>(2), .rootElement = Id<UiElementId>(3), .renderMode = mode}).HasValue());
        const auto source = std::move(builder).Build();
        REQUIRE(source.HasValue());
        const auto before = SerializeUiDocument(source.Value()).Value();
        const auto preview = BuildUiCanvasPreview(source.Value(), {}, Instance());
        if (mode == UiRenderMode::ScreenSpaceOverlay) {
            REQUIRE(preview.HasValue());
            CHECK(preview.Value().boxes.size() == 1);
        } else {
            REQUIRE(preview.HasError());
            CHECK(preview.ErrorValue().domain.Value() == UiErrors::CanvasSpaceModeMismatch.domain.Value());
            CHECK(preview.ErrorValue().code.Value() == UiErrors::CanvasSpaceModeMismatch.code.Value());
        }
        CHECK(SerializeUiDocument(source.Value()).Value() == before);
    }
}

TEST_CASE("Preview preserves typed missing style and intrinsic capability failures", "[ui_canvas][preview][capabilities]") {
    auto presentation = *Document().Elements().back().presentation;
    presentation.styleAsset = Id<RuntimeStyleAssetId>(8);
    const auto styled = Document(false, &presentation);
    const auto missingStyle = BuildUiCanvasPreview(styled, {}, Instance());
    REQUIRE(missingStyle.HasError());
    CHECK(missingStyle.ErrorValue().domain.Value() == UiErrors::StyleReferenceInvalid.domain.Value());
    CHECK(missingStyle.ErrorValue().code.Value() == UiErrors::StyleReferenceInvalid.code.Value());
    presentation.styleAsset = {};
    presentation.intrinsic = {.kind = UiLayoutIntrinsicKind::Text, .required = true};
    const auto source = Document(false, &presentation);
    const auto before = SerializeUiDocument(source).Value();
    const auto missingIntrinsic = BuildUiCanvasPreview(source, {}, Instance());
    REQUIRE(missingIntrinsic.HasError());
    CHECK(missingIntrinsic.ErrorValue().domain.Value() == UiErrors::LayoutIntrinsicUnavailable.domain.Value());
    CHECK(missingIntrinsic.ErrorValue().code.Value() == UiErrors::LayoutIntrinsicUnavailable.code.Value());
    CHECK(SerializeUiDocument(source).Value() == before);
}
