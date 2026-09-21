#include "Horo/Editor/UiCanvasDocument.h"
#include "Horo/Foundation/Platform.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Editor;
    using namespace Horo::Runtime::Ui;

    class TemporaryProject final {
    public:
        TemporaryProject() {
            const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
            root_ = std::filesystem::temp_directory_path() / ("horo-ui-canvas-" + std::to_string(nonce));
            std::filesystem::create_directories(root_ / "assets" / "ui");
        }

        ~TemporaryProject() {
            std::error_code error;
            std::filesystem::remove_all(root_, error);
        }

        [[nodiscard]] const std::filesystem::path &Root() const noexcept {
            return root_;
        }

        [[nodiscard]] std::filesystem::path DocumentPath() const {
            return root_ / "assets" / "ui" / "Hud.uicanvas";
        }

        void Write(const std::string &contents) const {
            std::ofstream output(DocumentPath(), std::ios::binary | std::ios::trunc);
            output << contents;
            REQUIRE(static_cast<bool>(output));
        }

    private:
        std::filesystem::path root_;
    };

    template <typename Id> [[nodiscard]] Id MakeUiId(const std::uint8_t value) {
        SerializedUiId bytes{};
        bytes.back() = value;
        return Id::Create(bytes).Value();
    }

    [[nodiscard]] UiDocument MakeDocument(const std::uint64_t revision, const std::uint8_t documentId = 1) {
        UiDocumentBuilder builder{MakeUiId<UiDocumentId>(documentId), UiDocumentRevision::Create(revision).Value()};
        static_cast<void>(builder.AddCanvas(UiCanvasDescriptor{
            .id = MakeUiId<UiCanvasId>(2),
            .rootElement = MakeUiId<UiElementId>(3),
            .renderMode = UiRenderMode::ScreenSpaceOverlay,
            .referenceResolution = {.width = 1920, .height = 1080},
            .scaleMode = UiScaleMode::ScaleWithScreenSize,
        }));
        return std::move(builder).Build().Value();
    }

    [[nodiscard]] DocumentIdentity OpenIdentity(DocumentIdentityRegistry &registry) {
        const SourceDocumentId source = SourceDocumentId::Parse("assets/ui/Hud.uicanvas").Value();
        return registry.Open(DocumentOpenKey{.kind = DocumentKind::UiCanvas, .source = source}).Value().identity;
    }

    [[nodiscard]] std::string ErrorCodeText(const Error &error) {
        return error.code.Value();
    }

    void SaveInitialDocument(const TemporaryProject &project, const UiDocument &document, ProjectMutationCoordinator &mutations,
                             DurableFileSystem &files) {
        const auto saved = SaveUiCanvasDocument(project.Root(), project.DocumentPath(),
                                                UiCanvasDocumentSnapshot{.document = document, .state = {1}}, {}, true, mutations, files);
        REQUIRE(saved.HasValue());
        REQUIRE(saved.Value().status == UiCanvasDocumentSaveStatus::Saved);
    }
}  // namespace

TEST_CASE("UI Canvas documents round trip through the typed runtime model", "[unit][editor][ui_canvas]") {
    TemporaryProject project;
    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    const UiDocument authored = MakeDocument(1);
    SaveInitialDocument(project, authored, mutations, files);

    const auto loaded = LoadUiCanvasDocument(project.DocumentPath());
    REQUIRE(loaded.HasValue());
    REQUIRE(loaded.Value().document.Id() == authored.Id());
    REQUIRE(loaded.Value().document.Revision() == authored.Revision());
    REQUIRE(loaded.Value().document.Canvases().size() == 1);
    REQUIRE(loaded.Value().document.Canvases().front() == authored.Canvases().front());

    DocumentIdentityRegistry registry;
    auto opened = UiCanvasDocument::Open(OpenIdentity(registry), project.DocumentPath());
    REQUIRE(opened.HasValue());
    UiCanvasDocument session = std::move(opened).Value();
    REQUIRE_FALSE(session.IsDirty());

    REQUIRE(session.Replace(MakeDocument(2)).HasValue());
    REQUIRE(session.IsDirty());
    const auto saved = session.Save(project.Root(), mutations, files);
    REQUIRE(saved.HasValue());
    REQUIRE(saved.Value().status == UiCanvasDocumentSaveStatus::Saved);
    REQUIRE_FALSE(session.IsDirty());

    const auto reopened = LoadUiCanvasDocument(project.DocumentPath());
    REQUIRE(reopened.HasValue());
    REQUIRE(reopened.Value().document.Revision().Value() == 2);
}

TEST_CASE("UI Canvas save detects external changes before replacing bytes", "[unit][editor][ui_canvas][persistence]") {
    TemporaryProject project;
    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    SaveInitialDocument(project, MakeDocument(1), mutations, files);

    DocumentIdentityRegistry registry;
    auto opened = UiCanvasDocument::Open(OpenIdentity(registry), project.DocumentPath());
    REQUIRE(opened.HasValue());
    UiCanvasDocument session = std::move(opened).Value();

    const auto externalFingerprint = InspectUiCanvasDocumentFingerprint(project.DocumentPath());
    REQUIRE(externalFingerprint.HasValue());
    const auto externalSave =
        SaveUiCanvasDocument(project.Root(), project.DocumentPath(), UiCanvasDocumentSnapshot{.document = MakeDocument(3), .state = {1}},
                             externalFingerprint.Value(), true, mutations, files);
    REQUIRE(externalSave.HasValue());

    REQUIRE(session.Replace(MakeDocument(2)).HasValue());
    const auto conflict = session.Save(project.Root(), mutations, files);
    REQUIRE(conflict.HasValue());
    REQUIRE(conflict.Value().status == UiCanvasDocumentSaveStatus::Conflict);
    REQUIRE(session.IsDirty());

    const auto overwritten = session.Save(project.Root(), mutations, files, true);
    REQUIRE(overwritten.HasValue());
    REQUIRE(overwritten.Value().status == UiCanvasDocumentSaveStatus::Saved);
    REQUIRE_FALSE(session.IsDirty());
}

TEST_CASE("UI Canvas sessions reject replacement or reload from another document identity", "[unit][editor][ui_canvas][identity]") {
    TemporaryProject project;
    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    SaveInitialDocument(project, MakeDocument(1), mutations, files);

    DocumentIdentityRegistry registry;
    auto opened = UiCanvasDocument::Open(OpenIdentity(registry), project.DocumentPath());
    REQUIRE(opened.HasValue());
    UiCanvasDocument session = std::move(opened).Value();

    const auto replacement = session.Replace(MakeDocument(2, 9));
    REQUIRE(replacement.HasError());
    REQUIRE(ErrorCodeText(replacement.ErrorValue()) == "editor.ui_canvas_document.invalid");
    REQUIRE_FALSE(session.IsDirty());

    const auto fingerprint = InspectUiCanvasDocumentFingerprint(project.DocumentPath());
    REQUIRE(fingerprint.HasValue());
    const auto external =
        SaveUiCanvasDocument(project.Root(), project.DocumentPath(), UiCanvasDocumentSnapshot{.document = MakeDocument(2, 9), .state = {1}},
                             fingerprint.Value(), true, mutations, files);
    REQUIRE(external.HasValue());

    const auto reload = session.Reload(UiCanvasReloadPolicy::DiscardChanges);
    REQUIRE(reload.HasError());
    REQUIRE(ErrorCodeText(reload.ErrorValue()) == "editor.ui_canvas_document.invalid");
    REQUIRE(session.Document().Id() == MakeDocument(1).Id());
}

TEST_CASE("UI Canvas reload and close enforce dirty and closed state policies", "[unit][editor][ui_canvas][lifecycle]") {
    TemporaryProject project;
    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    SaveInitialDocument(project, MakeDocument(1), mutations, files);

    DocumentIdentityRegistry registry;
    auto opened = UiCanvasDocument::Open(OpenIdentity(registry), project.DocumentPath());
    REQUIRE(opened.HasValue());
    UiCanvasDocument session = std::move(opened).Value();

    REQUIRE(session.Replace(MakeDocument(2)).HasValue());
    const auto reloadWhileDirty = session.Reload();
    REQUIRE(reloadWhileDirty.HasError());
    REQUIRE(ErrorCodeText(reloadWhileDirty.ErrorValue()) == "editor.ui_canvas_document.dirty");
    REQUIRE(session.Reload(UiCanvasReloadPolicy::DiscardChanges).HasValue());
    REQUIRE_FALSE(session.IsDirty());

    REQUIRE(session.Replace(MakeDocument(3)).HasValue());
    const auto closeWhileDirty = session.Close();
    REQUIRE(closeWhileDirty.HasError());
    REQUIRE(ErrorCodeText(closeWhileDirty.ErrorValue()) == "editor.ui_canvas_document.dirty");
    REQUIRE(session.Close(UiCanvasClosePolicy::DiscardChanges).HasValue());
    REQUIRE(session.IsClosed());
    const auto reloadClosed = session.Reload();
    REQUIRE(reloadClosed.HasError());
    REQUIRE(ErrorCodeText(reloadClosed.ErrorValue()) == "editor.ui_canvas_document.closed");
}

TEST_CASE("UI Canvas loading rejects missing malformed and future-version input", "[unit][editor][ui_canvas][validation]") {
    TemporaryProject project;
    const auto missing = LoadUiCanvasDocument(project.DocumentPath());
    REQUIRE(missing.HasError());
    REQUIRE(ErrorCodeText(missing.ErrorValue()) == "editor.ui_canvas_document.missing");

    project.Write("not-json");
    const auto malformed = LoadUiCanvasDocument(project.DocumentPath());
    REQUIRE(malformed.HasError());
    REQUIRE(ErrorCodeText(malformed.ErrorValue()) == "editor.ui_canvas_document.malformed");

    project.Write(R"({"format":"horo.ui_canvas","formatVersion":2})");
    const auto future = LoadUiCanvasDocument(project.DocumentPath());
    REQUIRE(future.HasError());
    REQUIRE(ErrorCodeText(future.ErrorValue()) == "editor.ui_canvas_document.version_unsupported");

    const auto relative = LoadUiCanvasDocument("assets/ui/Hud.uicanvas");
    REQUIRE(relative.HasError());
    REQUIRE(ErrorCodeText(relative.ErrorValue()) == "editor.ui_canvas_document.path_invalid");
}
