#include "Horo/Editor/UiCanvasDocument.h"
#include "Horo/Foundation/Platform.h"
#include "Horo/Runtime/Ui/UiDocumentSerialization.h"
#include "UiCanvasPreview.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Runtime::Ui;

    template <typename T> T Id(const std::uint8_t byte) {
        SerializedUiId bytes{};
        bytes.back() = byte;
        return T::Create(bytes).Value();
    }

    UiDocument Document(const bool legacy = false, const std::uint64_t revision = 1) {
        UiDocumentBuilder builder{Id<UiDocumentId>(1), UiDocumentRevision::Create(revision).Value()};
        REQUIRE(
            builder.AddCanvas({.id = Id<UiCanvasId>(2), .rootElement = Id<UiElementId>(3), .referenceResolution = {800, 600}}).HasValue());
        if (!legacy)
            REQUIRE(builder.AddElement(CreateUiElement(UiElementKind::Canvas, Id<UiElementId>(3), {}).Value()).HasValue());
        return std::move(builder).Build().Value();
    }

    struct Fixture final {
        std::filesystem::path root =
            std::filesystem::temp_directory_path() /
            ("horo-canvas-hierarchy-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        DocumentIdentityRegistry registry;

        Fixture() {
            std::filesystem::create_directories(root);
        }

        ~Fixture() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        std::filesystem::path Path() const {
            return root / "Hud.uicanvas";
        }

        void Write(const UiDocument &document) const {
            auto encoded = SerializeUiDocument(document);
            REQUIRE(encoded.HasValue());
            std::ofstream file(Path());
            file << nlohmann::json{{"format", "horo.ui_canvas"},
                                   {"formatVersion", 2},
                                   {"document", nlohmann::json::parse(encoded.Value())}};
            REQUIRE(file.good());
        }

        UiCanvasDocument Open(const bool legacy = false) {
            Write(Document(legacy));
            auto identity = registry.Open({DocumentKind::UiCanvas, SourceDocumentId::Parse("Hud.uicanvas").Value()});
            REQUIRE(identity.HasValue());
            auto opened = UiCanvasDocument::Open(identity.Value().identity, Path());
            REQUIRE(opened.HasValue());
            return std::move(opened).Value();
        }
    };

    void Create(UiCanvasDocument &document, const std::uint8_t element, const std::uint8_t parent = 3,
                const UiElementKind kind = UiElementKind::Panel) {
        REQUIRE(
            document.Execute(document.CurrentState(), UiCanvasCreateElement{Id<UiElementId>(element), Id<UiElementId>(parent), {}, kind})
                .HasValue());
    }
}  // namespace

TEST_CASE("Canvas creation selection hierarchy search and stable ordering share document commands", "[unit][editor][ui_canvas]") {
    Fixture fixture;
    auto document = fixture.Open();
    Create(document, 4);
    Create(document, 5, 4, UiElementKind::Text);
    Create(document, 6);
    REQUIRE(document.Selection() == Id<UiElementId>(6));
    const auto beforeSelection = document.CurrentState();
    REQUIRE(document.Select(beforeSelection, Id<UiElementId>(5)).HasValue());
    REQUIRE(document.CurrentState() == beforeSelection);
    REQUIRE(document.Hierarchy("TEXT").Value().size() == 3);
    REQUIRE(document.Hierarchy("missing").Value().empty());
    REQUIRE(document.Execute(document.CurrentState(), UiCanvasMoveElement{Id<UiElementId>(6), Id<UiElementId>(3), Id<UiElementId>(4)})
                .HasValue());
    const auto rows = document.Hierarchy().Value();
    REQUIRE(rows[1].element == Id<UiElementId>(6));
    REQUIRE(rows[3].depth == 2);
    REQUIRE(document.Execute(document.CurrentState(), UiCanvasMoveElement{Id<UiElementId>(4), Id<UiElementId>(6), {}}).HasValue());
    REQUIRE(document.Hierarchy().Value()[2].parent == Id<UiElementId>(6));
}

TEST_CASE("Invalid canvas hierarchy transactions preserve state history and selection", "[unit][editor][ui_canvas]") {
    Fixture fixture;
    auto document = fixture.Open();
    Create(document, 4);
    Create(document, 5, 4);
    Create(document, 6, 3, UiElementKind::Text);
    const auto state = document.CurrentState();
    const auto bytes = SerializeUiDocument(document.Document()).Value();
    const auto selection = document.Selection();
    const std::array<UiCanvasElementCommand, 7> invalid{
        UiCanvasMoveElement{Id<UiElementId>(3), Id<UiElementId>(4), {}},
        UiCanvasMoveElement{Id<UiElementId>(4), Id<UiElementId>(5), {}},
        UiCanvasMoveElement{Id<UiElementId>(4), Id<UiElementId>(6), {}},
        UiCanvasMoveElement{Id<UiElementId>(4), Id<UiElementId>(3), Id<UiElementId>(5)},
        UiCanvasMoveElement{Id<UiElementId>(99), Id<UiElementId>(3), {}},
        UiCanvasCreateElement{Id<UiElementId>(4), Id<UiElementId>(3), {}, UiElementKind::Panel},
        UiCanvasCreateElement{Id<UiElementId>(7), Id<UiElementId>(3), {}, static_cast<UiElementKind>(999)},
    };
    for (const auto &command : invalid) {
        REQUIRE(document.Execute(state, command).HasError());
        REQUIRE(document.CurrentState() == state);
        REQUIRE(document.Selection() == selection);
        REQUIRE(SerializeUiDocument(document.Document()).Value() == bytes);
    }
    REQUIRE(document.Execute({state.value - 1}, UiCanvasCreateElement{Id<UiElementId>(7), Id<UiElementId>(3), {}, UiElementKind::Panel})
                .HasError());
    REQUIRE(document.Select(state, Id<UiElementId>(99)).HasError());
    REQUIRE(document.Hierarchy(std::string(257, 'x')).HasError());
    REQUIRE(document.Hierarchy(std::string(1, static_cast<char>(0xff))).HasError());
}

TEST_CASE("Canvas undo redo tracks saved content and fences notifications", "[unit][editor][ui_canvas]") {
    Fixture fixture;
    auto document = fixture.Open();
    Create(document, 4);
    const auto stale = document.CurrentState();
    REQUIRE(document.IsDirty());
    REQUIRE(document.Undo(document.CurrentState()).HasValue());
    REQUIRE_FALSE(document.IsDirty());
    REQUIRE(document.CurrentState() != stale);
    REQUIRE(document.Redo(stale).HasError());
    REQUIRE(document.Redo(document.CurrentState()).HasValue());
    REQUIRE(document.IsDirty());
    Create(document, 5);
    REQUIRE(document.Execute(document.CurrentState(), UiCanvasMoveElement{Id<UiElementId>(5), Id<UiElementId>(4), {}}).HasValue());
    REQUIRE(document.Undo(document.CurrentState()).HasValue());
    REQUIRE(document.Hierarchy().Value().back().parent == Id<UiElementId>(3));
    REQUIRE(document.Undo(document.CurrentState()).HasValue());
    Create(document, 6);
    REQUIRE_FALSE(document.CanRedo());
    REQUIRE(document.Document().Elements().size() == 3);
}

TEST_CASE("Legacy canvas roots materialize atomically and undo restores the original model", "[unit][editor][ui_canvas]") {
    Fixture fixture;
    auto document = fixture.Open(true);
    REQUIRE(document.Document().Elements().empty());
    REQUIRE(document.Hierarchy().Value().size() == 1);
    REQUIRE(document.Select(document.CurrentState(), Id<UiElementId>(3)).HasValue());
    Create(document, 4);
    REQUIRE(document.Document().Elements().size() == 2);
    REQUIRE(document.Undo(document.CurrentState()).HasValue());
    REQUIRE(document.Document().Elements().empty());
    REQUIRE_FALSE(document.IsDirty());
    REQUIRE(document.Redo(document.CurrentState()).HasValue());
    REQUIRE(document.Document().Elements().size() == 2);
}

TEST_CASE("Retained hierarchy save reload and shutdown preserve ownership and reject stale work", "[unit][editor][ui_canvas]") {
    Fixture fixture;
    auto document = fixture.Open();
    Create(document, 4);
    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    auto saved = document.Save(fixture.root, mutations, files);
    REQUIRE(saved.HasValue());
    REQUIRE(saved.Value().status == UiCanvasDocumentSaveStatus::Saved);
    const auto loaded = LoadUiCanvasDocument(fixture.Path());
    REQUIRE(loaded.HasValue());
    REQUIRE(std::ranges::equal(loaded.Value().document.Elements(), document.Document().Elements()));
    const auto stale = document.CurrentState();
    Create(document, 5);
    REQUIRE(document.Reload().HasError());
    REQUIRE(document.Reload(UiCanvasReloadPolicy::DiscardChanges).HasValue());
    REQUIRE_FALSE(document.CanUndo());
    REQUIRE_FALSE(document.Selection().IsValid());
    REQUIRE(document.Document().Elements().size() == 2);
    REQUIRE(document.Execute(stale, UiCanvasMoveElement{Id<UiElementId>(4), Id<UiElementId>(3), {}}).HasError());
    REQUIRE(document.Close().HasValue());
    REQUIRE(document.Execute(document.CurrentState(), UiCanvasCreateElement{}).HasError());
    REQUIRE(document.Select(document.CurrentState(), {}).HasError());
    REQUIRE(document.Hierarchy().HasError());
    REQUIRE(document.Undo(document.CurrentState()).HasError());
    REQUIRE(document.Redo(document.CurrentState()).HasError());
}

TEST_CASE("Canvas history and depth are explicitly bounded", "[unit][editor][ui_canvas]") {
    Fixture fixture;
    auto document = fixture.Open();
    for (std::uint8_t element = 4; element < 67; ++element)
        Create(document, element, element == 4 ? 3 : static_cast<std::uint8_t>(element - 1));
    const auto state = document.CurrentState();
    REQUIRE(document.Execute(state, UiCanvasCreateElement{Id<UiElementId>(67), Id<UiElementId>(66), {}, UiElementKind::Panel}).HasError());
    REQUIRE(document.CurrentState() == state);
    for (std::uint8_t element = 67; element < 132; ++element)
        Create(document, element);
    REQUIRE(
        document.Execute(document.CurrentState(), UiCanvasCreateElement{Id<UiElementId>(132), Id<UiElementId>(3), {}, UiElementKind::Panel})
            .HasError());
}

TEST_CASE("Canvas inspector presentation edits share history state fences and durable ownership", "[unit][editor][ui_canvas]") {
    Fixture fixture;
    auto document = fixture.Open();
    Create(document, 4);
    const auto element = Id<UiElementId>(4);
    const auto original = document.Document().Elements()[1].presentation;
    UiElementPresentation presentation;
    presentation.layout.width = UiLength::Percent(32);
    presentation.layout.height = UiLength::Dip(640);
    const auto stale = document.CurrentState();
    REQUIRE(document.Execute(stale, UiCanvasSetPresentation{element, presentation}).HasValue());
    REQUIRE(document.Document().Elements()[1].presentation == presentation);
    REQUIRE(document.Execute(stale, UiCanvasSetPresentation{element, {}}).HasError());
    REQUIRE(document.Undo(document.CurrentState()).HasValue());
    REQUIRE(document.Document().Elements()[1].presentation == original);
    REQUIRE(document.Redo(document.CurrentState()).HasValue());
    REQUIRE(document.Document().Elements()[1].presentation == presentation);
    presentation.layout.width = {static_cast<UiLengthKind>(255), 0};
    const auto state = document.CurrentState();
    REQUIRE(document.Execute(state, UiCanvasSetPresentation{element, presentation}).HasError());
    REQUIRE(document.CurrentState() == state);
    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    REQUIRE(document.Save(fixture.root, mutations, files).HasValue());
    const auto loaded = LoadUiCanvasDocument(fixture.Path());
    REQUIRE(loaded.HasValue());
    REQUIRE(loaded.Value().document.Elements()[1].presentation == document.Document().Elements()[1].presentation);
}

TEST_CASE("Moving a canvas owner closes the old command and history boundary", "[unit][editor][ui_canvas]") {
    Fixture fixture;
    auto source = fixture.Open();
    Create(source, 4);
    const auto state = source.CurrentState();
    auto destination = std::move(source);
    REQUIRE(source.IsClosed());
    REQUIRE(source.Undo(state).HasError());
    REQUIRE(source.Execute(state, UiCanvasSetPresentation{}).HasError());
    REQUIRE(destination.Undo(destination.CurrentState()).HasValue());
    REQUIRE_FALSE(destination.IsDirty());
}

TEST_CASE("Canvas publication fences reject undo ABA replacement and reopened session drafts", "[unit][editor][ui_canvas]") {
    Fixture fixture;
    auto document = fixture.Open();
    const auto original = document.CurrentState();
    Create(document, 4);
    REQUIRE(document.Undo(document.CurrentState()).HasValue());
    REQUIRE_FALSE(document.IsDirty());
    REQUIRE(document.Execute(original, UiCanvasCreateElement{Id<UiElementId>(5), Id<UiElementId>(3), {}, UiElementKind::Panel}).HasError());
    const auto beforeReplacement = document.CurrentState();
    const auto staleReplacement = document.Replace(Document());
    REQUIRE(staleReplacement.HasError());
    REQUIRE(staleReplacement.ErrorValue().code.Value() == UiErrors::RevisionStale.code.Value());
    REQUIRE(document.CurrentState() == beforeReplacement);
    REQUIRE(document.Replace(Document(false, document.Document().Revision().Value() + 1)).HasValue());
    REQUIRE(document.Select(beforeReplacement, Id<UiElementId>(3)).HasError());
    const auto beforeClose = document.CurrentState();
    REQUIRE(document.Close(UiCanvasClosePolicy::DiscardChanges).HasValue());
    REQUIRE(fixture.registry.Close(document.Identity().instance).HasValue());
    auto reopened = fixture.Open();
    REQUIRE(reopened.CurrentState().instance != beforeClose.instance);
    REQUIRE(
        reopened.Execute(beforeClose, UiCanvasCreateElement{Id<UiElementId>(5), Id<UiElementId>(3), {}, UiElementKind::Panel}).HasError());
    auto coincidentRevision = reopened.CurrentState();
    coincidentRevision.instance = beforeClose.instance;
    REQUIRE(reopened.Select(coincidentRevision, Id<UiElementId>(3)).HasError());
    Create(reopened, 4);
    UiElementPresentation layoutOnly;
    layoutOnly.layout.width = UiLength::Dip(640);
    REQUIRE(layoutOnly.inlineStyle.empty());
    REQUIRE_FALSE(layoutOnly.styleAsset.IsValid());
    REQUIRE(reopened.Execute(reopened.CurrentState(), UiCanvasSetPresentation{Id<UiElementId>(4), layoutOnly}).HasValue());
}

TEST_CASE("Visual layout commits one owner transaction and rejects gestures across undo reload and close", "[ui_canvas][manipulation]") {
    Fixture fixture;
    auto owner = fixture.Open();
    Create(owner, 4);
    auto element = owner.Document().Elements().back();
    element.presentation->layout.positioning = UiLayoutPositioning::Absolute;
    element.presentation->layout.width = UiLength::Dip(100 * 64);
    element.presentation->layout.height = UiLength::Dip(60 * 64);
    REQUIRE(owner.Execute(owner.CurrentState(), UiCanvasSetPresentation{element.id, element.presentation}).HasValue());
    UiCanvasPreviewState state;
    state.snap = false;
    state.tool = UiCanvasLayoutTool::Move;
    const auto preview = BuildUiCanvasPreview(owner.Document(), state, owner.Identity().instance);
    REQUIRE(preview.HasValue());
    const auto edit = PrepareUiCanvasLayoutGesture(element, preview.Value().boxes.back().bounds, {17, -23}, {800 * 64, 600 * 64}, state);
    REQUIRE(edit.HasValue());
    const auto start = owner.CurrentState();
    REQUIRE(owner.Execute(start, edit.Value()).HasValue());
    CHECK(owner.Document().Elements().back().presentation->layout.offsets.left == 17);
    REQUIRE(owner.Undo(owner.CurrentState()).HasValue());
    CHECK(owner.Document().Elements().back().presentation->layout.offsets.left == 0);
    CHECK(owner.Execute(start, edit.Value()).HasError());
    REQUIRE(owner.Redo(owner.CurrentState()).HasValue());
    CHECK(owner.Document().Elements().back().presentation->layout.offsets.left == 17);
    const auto beforeReload = owner.CurrentState();
    REQUIRE(owner.Reload(UiCanvasReloadPolicy::DiscardChanges).HasValue());
    CHECK(owner.Execute(beforeReload, edit.Value()).HasError());
    const auto beforeClose = owner.CurrentState();
    REQUIRE(owner.Close().HasValue());
    CHECK(owner.Execute(beforeClose, edit.Value()).HasError());
}

TEST_CASE("Visual layout selection fence rejects A to B to A after real canonical selection changes",
          "[ui_canvas][manipulation][selection]") {
    Fixture fixture;
    auto owner = fixture.Open();
    Create(owner, 4);
    Create(owner, 5);
    REQUIRE(owner.Select(owner.CurrentState(), Id<UiElementId>(4)).HasValue());
    const auto element = std::ranges::find(owner.Document().Elements(), Id<UiElementId>(4), &UiDocumentElement::id);
    REQUIRE(element != owner.Document().Elements().end());
    auto presentation = *element->presentation;
    presentation.layout.offsets.left = 17;
    const UiCanvasSetPresentation edit{element->id, presentation};
    const auto state = owner.CurrentState();
    const std::uint64_t capturedSelection = 10;
    REQUIRE(owner.Select(state, Id<UiElementId>(5)).HasValue());
    REQUIRE(owner.Select(state, Id<UiElementId>(4)).HasValue());
    CHECK(owner.CurrentState() == state);
    CHECK(owner.Selection() == edit.element);
    CHECK(ExecuteUiCanvasLayoutGesture(owner, state, capturedSelection, capturedSelection + 2, edit).HasError());
    CHECK(owner.CurrentState() == state);
    REQUIRE(ExecuteUiCanvasLayoutGesture(owner, state, capturedSelection + 2, capturedSelection + 2, edit).HasValue());
    CHECK(std::ranges::find(owner.Document().Elements(), edit.element, &UiDocumentElement::id)->presentation->layout.offsets.left == 17);
}
