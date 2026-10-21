#include "Horo/Runtime/Ui/UiDocumentSerialization.h"
#include "Horo/Runtime/Ui/UiElementCatalog.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <nlohmann/json.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime::Ui;

    template <typename T> T Id(const std::uint8_t value) {
        SerializedUiId bytes{};
        bytes.back() = value;
        return T::Create(bytes).Value();
    }

    UiLocalizedText Message() {
        const std::vector<UiLocalizedArgument> arguments{{"integer", std::int64_t{-12}},
                                                         {"decimal", 0.125},
                                                         {"boolean", true},
                                                         {"date", UiLocalizedDateTime{1234}},
                                                         {"duration", UiLocalizedDuration{99}},
                                                         {"enum", UiLocalizedStableEnum{7, 8}},
                                                         {"text", std::string{"Ürün"}},
                                                         {"shortcut", UiLocalizedShortcut{0, 65}}};
        auto reference = UiLocalizedText::Create(UiMessageKey::Create("game.ui", "label").Value(), arguments, "Source fallback");
        REQUIRE(reference.HasValue());
        return std::move(reference).Value();
    }

    Result<UiDocument> Document(const UiElementKind kind, UiElementPresentation presentation,
                                const UiDocumentSchemaVersion schema = CurrentUiDocumentSchemaVersion) {
        UiDocumentBuilder builder{Id<UiDocumentId>(1), UiDocumentRevision::Create(1).Value(), schema};
        REQUIRE(
            builder.AddCanvas({.id = Id<UiCanvasId>(2), .rootElement = Id<UiElementId>(3), .referenceResolution = {800, 600}}).HasValue());
        REQUIRE(builder.AddElement(CreateUiElement(UiElementKind::Canvas, Id<UiElementId>(3), {}).Value()).HasValue());
        auto element = CreateUiElement(kind, Id<UiElementId>(4), Id<UiElementId>(3)).Value();
        element.presentation = std::move(presentation);
        REQUIRE(builder.AddElement(std::move(element)).HasValue());
        return std::move(builder).Build();
    }

    UiElementPresentation TextPresentation() {
        UiElementPresentation value;
        value.localizedText = Message();
        value.state.visible = false;
        value.state.focusable = false;
        value.accessibility =
            UiElementAccessibility{.role = UiAccessibilityRole::StaticText, .name = Message(), .description = Message(), .hidden = true};
        return value;
    }

    UiElementTree Tree(const UiDocument &document) {
        auto allocator = UiElementSlotAllocator::Create(UiOwnershipGeneration::Create(801).Value()).Value();
        const auto owner = allocator.Ownership();
        const UiElementTreeDescriptor descriptor{.instance = {owner, 1, 1},
                                                 .canvas = {owner, 2, 1},
                                                 .document = document.Id(),
                                                 .documentRevision = document.Revision(),
                                                 .treeRevision = UiRuntimeTreeRevision::Create(1).Value(),
                                                 .limits = {8, 8, 8}};
        const std::array nodes{UiElementDescriptor{Id<UiElementId>(3), {}}, UiElementDescriptor{Id<UiElementId>(4), Id<UiElementId>(3)}};
        auto tree = UiElementTree::Create(allocator, descriptor, nodes);
        REQUIRE(tree.HasValue());
        return std::move(tree).Value();
    }

    UiControlDescriptorBase Base(const UiDocument &document, const UiElementTree &tree) {
        return {.owner = {tree.Instance(), tree.Canvas(), document.Id(), document.Revision(), tree.Revision(),
                          UiInteractionRevision::Create(1).Value()},
                .element = tree.Find(Id<UiElementId>(4)).Value(),
                .action = Id<UiActionId>(9)};
    }
}  // namespace

TEST_CASE("Extended UI authoring round trips canonical messages arguments state and accessibility through source and cook",
          "[runtime_ui][authoring][codec]") {
    auto document = Document(UiElementKind::Text, TextPresentation());
    REQUIRE(document.HasValue());
    auto encoded = SerializeUiDocument(document.Value());
    REQUIRE(encoded.HasValue());
    const auto json = nlohmann::json::parse(encoded.Value());
    CHECK(json["elements"][1]["presentation"]["version"] == 2);
    auto decoded = DeserializeUiDocument(encoded.Value());
    REQUIRE(decoded.HasValue());
    CHECK(std::ranges::equal(document.Value().Elements(), decoded.Value().Elements()));
    auto cooked = CookedUiDocument::Cook(document.Value());
    REQUIRE(cooked.HasValue());
    auto uncooked = CookedUiDocument::Decode(cooked.Value().Payload());
    REQUIRE(uncooked.HasValue());
    CHECK(std::ranges::equal(document.Value().Elements(), uncooked.Value().Elements()));
    const auto *message = UiElementLocalizedText(decoded.Value().Elements()[1]);
    REQUIRE(message != nullptr);
    CHECK(message->Arguments().size() == 8);
    CHECK(*message == Message());
}

TEST_CASE("Extended UI fields require schema 1.3 while unchanged legacy presentations retain format one",
          "[runtime_ui][authoring][migration]") {
    CHECK(Document(UiElementKind::Text, TextPresentation(), {1, 2}).HasError());
    UiElementPresentation legacy;
    const auto bytes = SerializeUiElementPresentation(legacy);
    REQUIRE(bytes.HasValue());
    CHECK(nlohmann::json::parse(bytes.Value())["version"] == 1);
    CHECK(Document(UiElementKind::Panel, legacy, {1, 2}).HasValue());
    auto migrated = DeserializeUiElementPresentation(bytes.Value());
    REQUIRE(migrated.HasValue());
    CHECK_FALSE(migrated.Value().UsesAuthoringExtensions());
    CHECK(migrated.Value() == legacy);
}

TEST_CASE("Extended UI codec rejects malformed missing unknown or invalid typed fields without guessing",
          "[runtime_ui][authoring][codec]") {
    auto source = SerializeUiElementPresentation(TextPresentation());
    REQUIRE(source.HasValue());
    const auto json = nlohmann::json::parse(source.Value());
    auto invalid = json;
    invalid["state"]["visible"] = 1;
    CHECK(DeserializeUiElementPresentation(invalid.dump()).HasError());
    invalid = json;
    invalid["state"]["kind"] = 256;
    CHECK(DeserializeUiElementPresentation(invalid.dump()).HasError());
    invalid = json;
    invalid["state"]["initial"] = false;
    CHECK(DeserializeUiElementPresentation(invalid.dump()).HasError());
    invalid = json;
    invalid["accessibility"]["role"] = 256;
    CHECK(DeserializeUiElementPresentation(invalid.dump()).HasError());
    invalid = json;
    invalid["accessibility"]["readOnly"] = true;
    CHECK(DeserializeUiElementPresentation(invalid.dump()).HasError());
    invalid = json;
    invalid.erase("localizedText");
    CHECK(DeserializeUiElementPresentation(invalid.dump()).HasError());
    invalid = json;
    invalid["liveFocused"] = true;
    CHECK(DeserializeUiElementPresentation(invalid.dump()).HasError());
    invalid = json;
    invalid["state"]["kind"] = 2;
    invalid["state"]["initial"] = {0, 1, 0, 0.5};
    CHECK(DeserializeUiElementPresentation(invalid.dump()).HasError());
}

TEST_CASE("Core kind validation rejects incompatible initial values and localized content before document publication",
          "[runtime_ui][authoring]") {
    UiElementPresentation value;
    value.state.initial = true;
    CHECK(Document(UiElementKind::Panel, value).HasError());
    CHECK(Document(UiElementKind::Toggle, value).HasValue());
    value.state.initial = UiControlScalarValues{0, 1, 0.1, 0.25};
    CHECK(Document(UiElementKind::Toggle, value).HasError());
    CHECK(Document(UiElementKind::Slider, value).HasValue());
    value.state.initial = std::string{"typed"};
    CHECK(Document(UiElementKind::Slider, value).HasError());
    CHECK(Document(UiElementKind::TextInput, value).HasValue());
    value.state.initial = std::monostate{};
    value.localizedText = Message();
    CHECK(Document(UiElementKind::Image, value).HasError());
    CHECK(Document(UiElementKind::Text, value).HasValue());
    value.localizedText.reset();
    value.accessibility = UiElementAccessibility{.role = UiAccessibilityRole::Slider, .name = Message()};
    CHECK(Document(UiElementKind::Panel, value).HasError());
    CHECK(Document(UiElementKind::Slider, value).HasValue());
}

TEST_CASE("Authored toggle values initialize an actual owner-bound control without serializing live state",
          "[runtime_ui][authoring][controls]") {
    UiElementPresentation value;
    value.state.initial = true;
    value.state.enabled = false;
    value.state.focusable = false;
    auto document = Document(UiElementKind::Toggle, value).Value();
    auto tree = Tree(document);
    auto control = ProjectUiElementControl(document, Id<UiElementId>(4), tree, UiToggleControlDescriptor{Base(document, tree)});
    REQUIRE(control.HasValue());
    auto created = UiControlStateMachine::Create(control.Value());
    REQUIRE(created.HasValue());
    auto machine = std::move(created).Value();
    auto state = machine.Snapshot();
    REQUIRE(state.HasValue());
    CHECK(std::get<UiToggleControlState>(state.Value()).checked);
    CHECK(std::get<UiToggleControlState>(state.Value()).availability == UiControlAvailability::Disabled);
    CHECK_FALSE(std::get<UiToggleControlState>(state.Value()).focused);
    REQUIRE(tree.BeginRetirement().HasValue());
    CHECK(ProjectUiElementControl(document, Id<UiElementId>(4), tree, control.Value()).HasError());
    machine.Shutdown();
    CHECK(machine.Snapshot().HasError());
}

TEST_CASE("Scalar and text authoring initialize the real control state machines and reject foreign targets",
          "[runtime_ui][authoring][controls]") {
    UiElementPresentation value;
    value.state.initial = UiControlScalarValues{-2, 2, 0.125, 0.375};
    auto document = Document(UiElementKind::Slider, value).Value();
    auto tree = Tree(document);
    auto projected = ProjectUiElementControl(document, Id<UiElementId>(4), tree, UiSliderControlDescriptor{Base(document, tree)});
    REQUIRE(projected.HasValue());
    auto machine = UiControlStateMachine::Create(projected.Value());
    REQUIRE(machine.HasValue());
    CHECK(std::get<UiSliderControlState>(machine.Value().Snapshot().Value()).value == 0.375);
    auto wrong = std::get<UiSliderControlDescriptor>(projected.Value());
    wrong.base.element = tree.Root().Value().handle;
    CHECK(ProjectUiElementControl(document, Id<UiElementId>(4), tree, wrong).HasError());
    value.state.initial = std::string{"Türkçe"};
    auto text = Document(UiElementKind::TextInput, value).Value();
    auto textTree = Tree(text);
    auto textDescriptor = ProjectUiElementControl(text, Id<UiElementId>(4), textTree, UiTextInputControlDescriptor{Base(text, textTree)});
    REQUIRE(textDescriptor.HasValue());
    auto textMachine = UiControlStateMachine::Create(textDescriptor.Value());
    REQUIRE(textMachine.HasValue());
    CHECK(std::get<UiTextInputControlState>(textMachine.Value().Snapshot().Value()).text.View() == "Türkçe");
}

TEST_CASE("Accessibility projection uses resolved owner text and preserves the canonical live value authority",
          "[runtime_ui][authoring][accessibility]") {
    const auto document = Document(UiElementKind::Text, TextPresentation()).Value();
    const auto &element = document.Elements()[1];
    const auto focused = static_cast<UiAccessibilityStateMask>(UiAccessibilityStateFlag::Focused);
    const auto focusable = static_cast<UiAccessibilityStateMask>(UiAccessibilityStateFlag::Focusable);
    UiAccessibilityNodeInput live{.element = element.id,
                                  .role = UiAccessibilityRole::Group,
                                  .name = {"Resolved localized name"},
                                  .description = {"Resolved localized description"},
                                  .state = {focused | focusable}};
    auto node = ProjectUiElementAccessibility(element, live);
    REQUIRE(node.HasValue());
    CHECK(node.Value().role == UiAccessibilityRole::StaticText);
    CHECK(node.Value().name.text == live.name.text);
    CHECK(node.Value().exposure == UiAccessibilityExposure::Hidden);
    CHECK_FALSE(node.Value().state.Has(UiAccessibilityStateFlag::Focused));
    live.element = Id<UiElementId>(99);
    CHECK(ProjectUiElementAccessibility(element, live).HasError());
}

TEST_CASE("Authored style projection uses the same initial disabled checked mask as cascade validation",
          "[runtime_ui][authoring][styles]") {
    UiElementPresentation value;
    value.state.initial = true;
    value.state.enabled = false;
    auto document = Document(UiElementKind::Toggle, value).Value();
    auto tree = Tree(document);
    auto style = ProjectUiElementStyle(document.Elements()[1], tree.Find(Id<UiElementId>(4)).Value());
    REQUIRE(style.HasValue());
    CHECK(style.Value().state.Contains(UiVisualStateMask{UiVisualState::Disabled}));
    CHECK(style.Value().state.Contains(UiVisualStateMask{UiVisualState::Checked}));
    CHECK_FALSE(style.Value().state.Contains(UiVisualStateMask{UiVisualState::Focused}));
    CHECK_FALSE(style.Value().state.Contains(UiVisualStateMask{UiVisualState::Pressed}));
    value.state.enabled = true;
    value.state.visible = false;
    CHECK(value.state.InitialVisualState().Contains(UiVisualStateMask{UiVisualState::Disabled}));
}
