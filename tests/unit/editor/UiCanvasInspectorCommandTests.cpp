#include "Horo/Foundation/Platform.h"
#include "Horo/Runtime/Ui/UiDocumentSerialization.h"
#include "UiInspectorDraft.h"

#include <catch2/catch_test_macros.hpp>
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

        InspectorSource() {
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
            REQUIRE(owner.Select(owner.CurrentState(), Id<UiElementId>(3)).HasValue());
            return owner;
        }
    };

    UiInspectorContext Context(const UiCanvasDocument &owner, const std::uint64_t selection = 1) {
        return {.document = owner.Identity().instance,
                .state = owner.CurrentState(),
                .revision = owner.Document().Revision(),
                .element = owner.Selection(),
                .selectionRevision = selection};
    }

    UiElementPresentation Presentation(const UiCanvasDocument &owner) {
        return *owner.Document().Elements().front().presentation;
    }

    void Capture(UiInspectorDraft &draft, const UiCanvasDocument &owner) {
        const auto presentation = Presentation(owner);
        REQUIRE(draft.Begin(Context(owner), {.layout = presentation.layout, .style = presentation.inlineStyle.front()}));
    }
}  // namespace

TEST_CASE("UI inspector commits one typed presentation transaction and preserves other properties", "[editor][ui][inspector][command]") {
    InspectorSource source;
    auto owner = source.Open();
    UiInspectorDraft draft;
    Capture(draft, owner);
    const auto baseline = Presentation(owner);
    const auto context = Context(owner);
    draft.Values().layout.width = UiLength::Dip(640);
    draft.Values().style->value = UiStyleValueSource::Literal(UiStyleScalar{0.75F});
    auto prepared = draft.PreparePresentation(context, baseline, true);
    REQUIRE(prepared.HasValue());
    CHECK_FALSE(owner.IsDirty());
    REQUIRE(ExecuteUiInspectorPresentation(owner, context, 1, prepared.Value()).HasValue());
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
    CHECK(ExecuteUiInspectorPresentation(owner, context, 1, prepared.Value()).HasError());
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
    ProjectMutationCoordinator mutations;
    NativeDurableFileSystem files;
    auto saved = owner.Save(source.root, mutations, files);
    REQUIRE(saved.HasValue());
    REQUIRE(saved.Value().status == UiCanvasDocumentSaveStatus::Saved);
    REQUIRE(owner.Reload().HasValue());
    CHECK_FALSE(owner.IsDirty());
    CHECK_FALSE(owner.Selection().IsValid());
    CHECK(Presentation(owner).layout.height == UiLength::Percent(32));
    CHECK(Presentation(owner).inlineStyle == baseline.inlineStyle);
}
