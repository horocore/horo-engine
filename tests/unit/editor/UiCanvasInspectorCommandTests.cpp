#include "Horo/Foundation/Platform.h"
#include "Horo/Runtime/Ui/UiDocumentSerialization.h"
#include "UiInspectorDraft.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Runtime::Ui;

    template <typename T> T Id(const std::uint8_t value) {
        SerializedUiId bytes{};
        bytes.back() = value;
        return T::Create(bytes).Value();
    }

    struct InspectorSource final {
        std::filesystem::path root = std::filesystem::temp_directory_path() /
                                     ("horo-ui-inspector-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        DocumentIdentityRegistry identities;

        UiElementId selected;

        explicit InspectorSource(const UiElementKind kind = UiElementKind::Canvas)
            : selected(Id<UiElementId>(kind == UiElementKind::Canvas ? 3 : 4)) {
            std::filesystem::create_directories(root);
            UiDocumentBuilder builder{Id<UiDocumentId>(1), UiDocumentRevision::Create(1).Value()};
            REQUIRE(builder.AddCanvas({.id = Id<UiCanvasId>(2), .rootElement = Id<UiElementId>(3), .referenceResolution = {800, 600}})
                        .HasValue());
            auto element = CreateUiElement(UiElementKind::Canvas, Id<UiElementId>(3), {}).Value();
            element.presentation->inlineStyle.push_back(
                {.property = Id<UiStylePropertyId>(1), .value = UiStyleValueSource::Literal(UiStyleScalar{0.5F})});
            element.presentation->inlineStyle.push_back(
                {.property = Id<UiStylePropertyId>(2), .value = UiStyleValueSource::Literal(UiStyleColor{})});
            REQUIRE(builder.AddElement(element).HasValue());
            if (kind != UiElementKind::Canvas)
                REQUIRE(builder.AddElement(CreateUiElement(kind, selected, Id<UiElementId>(3)).Value()).HasValue());
            auto source = SerializeUiDocument(std::move(builder).Build().Value());
            REQUIRE(source.HasValue());
            std::ofstream out(Path());
            out << nlohmann::json{{"format", "horo.ui_canvas"}, {"formatVersion", 2}, {"document", nlohmann::json::parse(source.Value())}};
            REQUIRE(out.good());
        }

        ~InspectorSource() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        std::filesystem::path Path() const {
            return root / "Hud.uicanvas";
        }

        UiCanvasDocument Open() {
            auto identity = identities.Open({DocumentKind::UiCanvas, SourceDocumentId::Parse("Hud.uicanvas").Value()});
            REQUIRE(identity.HasValue());
            auto opened = UiCanvasDocument::Open(identity.Value().identity, Path());
            REQUIRE(opened.HasValue());
            auto owner = std::move(opened).Value();
            REQUIRE(owner.Select(owner.CurrentState(), selected).HasValue());
            return owner;
        }
    };

    UiInspectorContext Context(const UiCanvasDocument &owner, const std::uint64_t selection = 1,
                               const UiInspectorResourceSnapshot *resources = nullptr) {
        return {.document = owner.Identity().instance,
                .state = owner.CurrentState(),
                .revision = owner.Document().Revision(),
                .element = owner.Selection(),
                .selectionRevision = selection,
                .styles = resources ? resources->StyleGeneration() : RuntimeStyleGeneration{},
                .assets = resources ? resources->assets.Revision() : Assets::AssetRegistryRevision{},
                .resourcePublication = resources ? resources->publication : 0};
    }

    UiInspectorResourceSnapshot Resources(const bool stateSealed = false) {
        Assets::AssetRegistry assets;
        UiInspectorResources host{assets};
        UiStyleRegistryDefinition definition;
        definition.properties = {{.id = Id<UiStylePropertyId>(1),
                                  .category = UiStyleValueCategory::Scalar,
                                  .defaultValue = UiStyleScalar{0.5F},
                                  .minimumScalar = 0,
                                  .maximumScalar = 1},
                                 {.id = Id<UiStylePropertyId>(2), .category = UiStyleValueCategory::Color, .defaultValue = UiStyleColor{}}};
        definition.assets.push_back({.id = Id<RuntimeStyleAssetId>(1)});
        if (stateSealed)
            definition.assets.back().states.push_back(
                {UiVisualStateMask{UiVisualState::Disabled},
                 {},
                 UiStateLayer::Availability,
                 {{Id<UiStylePropertyId>(1), UiStyleValueSource::Literal(UiStyleScalar{0.25F}), true}}});
        auto registry = RuntimeStyleRegistry::Create(std::move(definition), RuntimeStyleGeneration::Create(1).Value());
        REQUIRE(registry.HasValue());
        auto lease = std::make_shared<RuntimeStyleRegistry>(std::move(registry).Value());
        REQUIRE(host.PublishStyles(lease, Id<RuntimeStyleAssetId>(1)).HasValue());
        return host.Capture();
    }

    UiElementPresentation Presentation(const UiCanvasDocument &owner) {
        return *owner.Document().Elements().front().presentation;
    }

    void Capture(UiInspectorDraft &draft, const UiCanvasDocument &owner, const UiInspectorResourceSnapshot *resources = nullptr) {
        const auto presentation = Presentation(owner);
        REQUIRE(draft.Begin(Context(owner, 1, resources), {.layout = presentation.layout, .style = presentation.inlineStyle.front()}));
    }

    /** @brief Build canonical authored samples separately from transaction and persistence assertions. */
    UiElementPresentation AuthoredSample(const UiElementKind kind, const UiElementPresentation &baseline) {
        auto candidate = baseline;
        candidate.state.visible = false;
        candidate.state.enabled = false;
        candidate.state.focusable = false;
        const std::vector<UiLocalizedArgument> arguments{UiLocalizedArgument{"count", std::int64_t{-9007199254740993LL}},
                                                         UiLocalizedArgument{"label", std::string{"Türkçe"}}};
        const auto message =
            UiLocalizedText::Create(UiMessageKey::Create("game.ui", "authored_label").Value(), arguments, "Fallback").Value();
        if (kind == UiElementKind::Text)
            candidate.localizedText = message;
        if (kind == UiElementKind::Toggle)
            candidate.state.initial = true;
        if (kind == UiElementKind::Slider)
            candidate.state.initial = UiControlScalarValues{-2, 2, 0.125, 0.375};
        if (kind == UiElementKind::TextInput)
            candidate.state.initial = std::string{"Initial ürün"};
        candidate.accessibility = UiElementAccessibility{.role = DefaultUiElementAccessibilityRole(kind),
                                                         .name = message,
                                                         .description = message,
                                                         .hidden = true};
        return candidate;
    }
}  // namespace

TEST_CASE("UI inspector commits one typed presentation transaction and preserves other properties", "[editor][ui][inspector][command]") {
    InspectorSource source;
    auto owner = source.Open();
    UiInspectorDraft draft;
    auto resources = Resources();
    Capture(draft, owner, &resources);
    const auto baseline = Presentation(owner);
    const auto context = Context(owner, 1, &resources);
    draft.Values().layout.width = UiLength::Dip(640);
    draft.Values().style->value = UiStyleValueSource::Literal(UiStyleScalar{0.75F});
    auto prepared = draft.PreparePresentation(context, baseline, true);
    REQUIRE(prepared.HasValue());
    CHECK_FALSE(owner.IsDirty());
    REQUIRE(ExecuteUiInspectorPresentation(owner, context, 1, prepared.Value(), &resources).HasValue());
    CHECK(owner.IsDirty());
    CHECK(owner.CanUndo());
    const auto actual = Presentation(owner);
    CHECK(actual.layout.width == UiLength::Dip(640));
    CHECK(actual.inlineStyle[0].value.literal == UiStyleValue{UiStyleScalar{0.75F}});
    CHECK(actual.inlineStyle[1] == baseline.inlineStyle[1]);
    REQUIRE(owner.Undo(owner.CurrentState()).HasValue());
    CHECK_FALSE(owner.IsDirty());
    CHECK(Presentation(owner) == baseline);
    CHECK_FALSE(owner.CanUndo());
    CHECK(owner.CanRedo());
    CHECK(ExecuteUiInspectorPresentation(owner, context, 1, prepared.Value(), &resources).HasError());
    REQUIRE(owner.Redo(owner.CurrentState()).HasValue());
    CHECK(Presentation(owner) == actual);
}

TEST_CASE("UI inspector invalid drafts preserve document history selection and dirty state", "[editor][ui][inspector][command]") {
    InspectorSource source;
    auto owner = source.Open();
    UiInspectorDraft draft;
    Capture(draft, owner);
    const auto before = SerializeUiDocument(owner.Document()).Value();
    const auto context = Context(owner);
    draft.Values().layout.padding.left = -1;
    CHECK(draft.PreparePresentation(context, Presentation(owner), true).HasError());
    draft.Cancel();
    CHECK(draft.PreparePresentation(context, Presentation(owner), false).HasError());
    auto invalid = Presentation(owner);
    invalid.classes.push_back({});
    CHECK(draft.PreparePresentation(context, invalid, true).HasError());
    CHECK(SerializeUiDocument(owner.Document()).Value() == before);
    CHECK(owner.CurrentState() == context.state);
    CHECK(owner.Selection() == context.element);
    CHECK_FALSE(owner.IsDirty());
    CHECK_FALSE(owner.CanUndo());
    auto prepared = draft.PreparePresentation(context, Presentation(owner), true);
    REQUIRE(prepared.HasValue());
    REQUIRE(ExecuteUiInspectorPresentation(owner, context, 1, prepared.Value()).HasValue());
    CHECK(owner.CurrentState() == context.state);
    CHECK_FALSE(owner.CanUndo());
}

TEST_CASE("UI inspector fences selection ABA, reload, moved storage and closed owners", "[editor][ui][inspector][command]") {
    InspectorSource source;
    auto owner = source.Open();
    UiInspectorDraft draft;
    Capture(draft, owner);
    const auto context = Context(owner);
    draft.Values().layout.width = UiLength::Dip(640);
    auto prepared = draft.PreparePresentation(context, Presentation(owner), true);
    REQUIRE(prepared.HasValue());
    CHECK(ExecuteUiInspectorPresentation(owner, context, 3, prepared.Value()).HasError());
    REQUIRE(owner.Select(owner.CurrentState(), {}).HasValue());
    CHECK(ExecuteUiInspectorPresentation(owner, context, 1, prepared.Value()).HasError());
    REQUIRE(owner.Select(owner.CurrentState(), context.element).HasValue());
    auto moved = std::move(owner);
    CHECK(ExecuteUiInspectorPresentation(owner, context, 1, prepared.Value()).HasError());
    REQUIRE(moved.Reload().HasValue());
    REQUIRE(moved.Select(moved.CurrentState(), context.element).HasValue());
    CHECK(ExecuteUiInspectorPresentation(moved, context, 1, prepared.Value()).HasError());
    REQUIRE(moved.Close().HasValue());
    CHECK(ExecuteUiInspectorPresentation(moved, context, 1, prepared.Value()).HasError());
}

TEST_CASE("UI inspector edits persist through source save and reload without a style registry", "[editor][ui][inspector][command]") {
    InspectorSource source;
    auto owner = source.Open();
    UiInspectorDraft draft;
    const auto baseline = Presentation(owner);
    const auto context = Context(owner);
    REQUIRE(draft.Begin(context, {.layout = baseline.layout}));
    draft.Values().layout.height = UiLength::Percent(32);
    auto prepared = draft.PreparePresentation(context, baseline, true);
    REQUIRE(prepared.HasValue());
    REQUIRE(ExecuteUiInspectorPresentation(owner, context, 1, prepared.Value()).HasValue());
    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations{files};
    auto saved = owner.Save(source.root, mutations, files);
    REQUIRE(saved.HasValue());
    REQUIRE(saved.Value().status == UiCanvasDocumentSaveStatus::Saved);
    REQUIRE(owner.Reload().HasValue());
    CHECK_FALSE(owner.IsDirty());
    CHECK_FALSE(owner.Selection().IsValid());
    CHECK(Presentation(owner).layout.height == UiLength::Percent(32));
    CHECK(Presentation(owner).inlineStyle == baseline.inlineStyle);
}

TEST_CASE("Resource validation failure and real publication ABA preserve the canonical document transaction",
          "[editor][ui][inspector][command]") {
    InspectorSource source;
    auto owner = source.Open();
    Assets::AssetRegistry assets;
    UiInspectorResources host{assets};
    const auto lease = Resources();
    REQUIRE(host.PublishStyles(lease.styles, lease.theme).HasValue());
    const auto before = host.Capture();
    UiInspectorDraft draft;
    Capture(draft, owner, &before);
    const auto context = Context(owner, 1, &before);
    const auto bytes = SerializeUiDocument(owner.Document()).Value();
    draft.Values().style->value = UiStyleValueSource::Literal(UiStyleScalar{2});
    auto invalid = draft.PreparePresentation(context, Presentation(owner), true);
    REQUIRE(invalid.HasValue());
    CHECK(ExecuteUiInspectorPresentation(owner, context, 1, invalid.Value(), &before).HasError());
    CHECK(SerializeUiDocument(owner.Document()).Value() == bytes);
    CHECK_FALSE(owner.CanUndo());
    draft.Values().style->value = UiStyleValueSource::Literal(UiStyleScalar{0.75F});
    auto prepared = draft.PreparePresentation(context, Presentation(owner), true);
    REQUIRE(prepared.HasValue());
    REQUIRE(host.PublishStyles(nullptr).HasValue());
    REQUIRE(host.PublishStyles(lease.styles, lease.theme).HasValue());
    const auto current = host.Capture();
    CHECK(current.StyleGeneration() == before.StyleGeneration());
    CHECK(ExecuteUiInspectorPresentation(owner, context, 1, prepared.Value(), &current).HasError());
    CHECK(SerializeUiDocument(owner.Document()).Value() == bytes);
    CHECK_FALSE(owner.IsDirty());
}

TEST_CASE("Authored state and accessibility edits use the same undoable canonical transaction with real style validation",
          "[editor][ui][inspector][command]") {
    InspectorSource source;
    auto owner = source.Open();
    UiInspectorDraft draft;
    const auto resources = Resources();
    const auto baseline = Presentation(owner);
    const auto context = Context(owner, 1, &resources);
    REQUIRE(draft.Begin(context, {.layout = baseline.layout}));
    auto values = baseline;
    values.state.visible = false;
    values.state.enabled = false;
    auto message = UiLocalizedText::Create(UiMessageKey::Create("game.ui", "canvas_name").Value(), "Accessible canvas").Value();
    values.accessibility = UiElementAccessibility{.role = UiAccessibilityRole::Application, .name = message};
    auto prepared = draft.PreparePresentation(context, values, true);
    REQUIRE(prepared.HasValue());
    REQUIRE(ExecuteUiInspectorPresentation(owner, context, 1, prepared.Value(), &resources).HasValue());
    CHECK(Presentation(owner).accessibility->name == message);
    CHECK_FALSE(Presentation(owner).state.visible);
    REQUIRE(owner.Undo(owner.CurrentState()).HasValue());
    CHECK(Presentation(owner) == baseline);
    CHECK_FALSE(owner.IsDirty());
    CHECK(ExecuteUiInspectorPresentation(owner, context, 1, prepared.Value()).HasError());
    REQUIRE(owner.Redo(owner.CurrentState()).HasValue());
    CHECK(Presentation(owner).accessibility->name == message);
}

TEST_CASE("State-only edits resolve the selected availability cascade before document publication", "[editor][ui][inspector][command]") {
    InspectorSource source;
    auto owner = source.Open();
    const auto resources = Resources(true);
    const auto context = Context(owner, 1, &resources);
    const auto bytes = SerializeUiDocument(owner.Document()).Value();
    auto value = Presentation(owner);
    value.state.enabled = false;
    const UiCanvasSetPresentation command{context.element, value};
    CHECK(ExecuteUiInspectorPresentation(owner, context, 1, command, &resources).HasError());
    CHECK(SerializeUiDocument(owner.Document()).Value() == bytes);
    CHECK(owner.CurrentState() == context.state);
    CHECK(owner.Selection() == context.element);
    CHECK_FALSE(owner.IsDirty());
    CHECK_FALSE(owner.CanUndo());
    CHECK_FALSE(owner.CanRedo());
    CHECK(ExecuteUiInspectorPresentation(owner, Context(owner), 1, command).HasError());
    value = Presentation(owner);
    value.layout.width = UiLength::Dip(512);
    REQUIRE(ExecuteUiInspectorPresentation(owner, context, 1, {context.element, value}, &resources).HasValue());
}

TEST_CASE("Inspector authored fields persist as complete canonical records through undo redo save reload and cook",
          "[editor][ui][inspector][command]") {
    const auto kind = GENERATE(UiElementKind::Text, UiElementKind::Toggle, UiElementKind::Slider, UiElementKind::TextInput);
    InspectorSource source{kind};
    auto owner = source.Open();
    const auto context = Context(owner);
    const auto baseline = *owner.Document().Elements().back().presentation;
    const auto candidate = AuthoredSample(kind, baseline);
    UiInspectorDraft draft;
    REQUIRE(draft.Begin(context, {.layout = baseline.layout}));
    const auto prepared = draft.PreparePresentation(context, candidate, true);
    REQUIRE(prepared.HasValue());
    REQUIRE(ExecuteUiInspectorPresentation(owner, context, 1, prepared.Value()).HasValue());
    CHECK(*owner.Document().Elements().back().presentation == candidate);
    REQUIRE(owner.Undo(owner.CurrentState()).HasValue());
    CHECK(*owner.Document().Elements().back().presentation == baseline);
    CHECK_FALSE(owner.IsDirty());
    REQUIRE(owner.Redo(owner.CurrentState()).HasValue());
    CHECK(*owner.Document().Elements().back().presentation == candidate);
    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations{files};
    const auto saved = owner.Save(source.root, mutations, files);
    REQUIRE(saved.HasValue());
    REQUIRE(saved.Value().status == UiCanvasDocumentSaveStatus::Saved);
    REQUIRE(owner.Reload().HasValue());
    CHECK(*owner.Document().Elements().back().presentation == candidate);
    const auto cooked = CookedUiDocument::Cook(owner.Document());
    REQUIRE(cooked.HasValue());
    const auto decoded = CookedUiDocument::Decode(cooked.Value().Payload());
    REQUIRE(decoded.HasValue());
    CHECK(*decoded.Value().Elements().back().presentation == candidate);
    CHECK_FALSE(owner.IsDirty());
    CHECK_FALSE(owner.CanUndo());
    CHECK_FALSE(owner.CanRedo());
}

TEST_CASE("Incompatible initial state and resource references publish no inspector transaction", "[editor][ui][inspector][command]") {
    InspectorSource source{UiElementKind::Text};
    auto owner = source.Open();
    const auto context = Context(owner);
    const auto bytes = SerializeUiDocument(owner.Document()).Value();
    const auto baseline = *owner.Document().Elements().back().presentation;
    UiInspectorDraft draft;
    REQUIRE(draft.Begin(context, {.layout = baseline.layout}));
    auto value = baseline;
    value.state.initial = true;
    auto prepared = draft.PreparePresentation(context, value, true);
    REQUIRE(prepared.HasValue());
    CHECK(ExecuteUiInspectorPresentation(owner, context, 1, prepared.Value()).HasError());
    value = baseline;
    value.inlineStyle.push_back({Id<UiStylePropertyId>(99), UiStyleValueSource::Literal(UiStyleScalar{0.5F}), false});
    prepared = draft.PreparePresentation(context, value, true);
    REQUIRE(prepared.HasValue());
    CHECK(ExecuteUiInspectorPresentation(owner, context, 1, prepared.Value()).HasError());
    value = baseline;
    value.state.initial = UiControlScalarValues{1, 0, 0, 3};
    CHECK(draft.PreparePresentation(context, value, true).HasError());
    CHECK(SerializeUiDocument(owner.Document()).Value() == bytes);
    CHECK(owner.CurrentState() == context.state);
    CHECK(owner.Selection() == context.element);
    CHECK_FALSE(owner.IsDirty());
    CHECK_FALSE(owner.CanUndo());
    CHECK_FALSE(owner.CanRedo());
}
