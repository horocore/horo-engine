#include "../../support/NativePublicationFiles.h"
#include "Horo/Editor/UiCanvasDocument.h"
#include "Horo/Foundation/Platform.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
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
            .presentation =
                {.safeArea = UiSafeAreaMode::Inset, .uiScale = {5, 4}, .fontScale = {3, 2}, .pixelSnap = UiPixelSnapMode::Edges},
        }));
        return std::move(builder).Build().Value();
    }

    [[nodiscard]] DocumentIdentity OpenIdentity(DocumentIdentityRegistry &registry) {
        const SourceDocumentId source = SourceDocumentId::Parse("assets/ui/Hud.uicanvas").Value();
        return registry.Open(DocumentOpenKey{.kind = DocumentKind::UiCanvas, .source = source}).Value().identity;
    }

    /** @brief Projects a canonical fixture into the deliberately separate legacy editor envelope. */
    [[nodiscard]] nlohmann::json LegacyEnvelope(const nlohmann::json &serialized) {
        nlohmann::json legacy{{"format", "horo.ui_canvas"},
                              {"formatVersion", 1},
                              {"documentId", serialized["document"]["documentId"]},
                              {"revision", serialized["document"]["revision"]},
                              {"canvases", serialized["document"]["canvases"]},
                              {"dependencies", serialized["document"]["dependencies"]}};
        legacy["canvases"][0]["renderMode"] = "screen_space_overlay";
        legacy["canvases"][0]["scaleMode"] = "scale_with_screen_size";
        return legacy;
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
    REQUIRE(loaded.Value().fingerprint.exists);
    REQUIRE(loaded.Value().fingerprint.byteSize > 0);

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

TEST_CASE("Legacy UI Canvas loading accepts uppercase hexadecimal identity digits", "[unit][editor][ui_canvas][validation]") {
    TemporaryProject project;
    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    SaveInitialDocument(project, MakeDocument(1, 0xab), mutations, files);
    std::ifstream input(project.DocumentPath(), std::ios::binary);
    REQUIRE(static_cast<bool>(input));
    const auto serialized = nlohmann::json::parse(std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>{}));
    input.close();
    auto legacy = LegacyEnvelope(serialized);
    legacy["documentId"] = std::string(30U, '0') + "AB";
    project.Write(legacy.dump());
    const auto loaded = LoadUiCanvasDocument(project.DocumentPath());
    REQUIRE(loaded.HasValue());
    REQUIRE(loaded.Value().document.Id() == MakeDocument(1, 0xab).Id());
}

TEST_CASE("Canonical UI Canvas source rejects noncanonical uppercase identity digits", "[unit][editor][ui_canvas][validation]") {
    TemporaryProject project;
    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    SaveInitialDocument(project, MakeDocument(1, 0xab), mutations, files);
    std::ifstream input(project.DocumentPath(), std::ios::binary);
    REQUIRE(static_cast<bool>(input));
    auto serialized = nlohmann::json::parse(std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>{}));
    input.close();
    REQUIRE(serialized["document"]["documentId"] == std::string(30U, '0') + "ab");
    serialized["document"]["documentId"] = std::string(30U, '0') + "AB";
    project.Write(serialized.dump());
    const auto loaded = LoadUiCanvasDocument(project.DocumentPath());
    REQUIRE(loaded.HasError());
    REQUIRE(ErrorCodeText(loaded.ErrorValue()) == "runtime_ui.document.serialization_invalid");
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

TEST_CASE("UI Canvas save preserves a late external write and rejects reentrant commands", "[unit][editor][ui_canvas][persistence]") {
    TemporaryProject project;
    NativeDurableFileSystem initial;
    ProjectMutationCoordinator mutations(initial);
    SaveInitialDocument(project, MakeDocument(1), mutations, initial);

    DocumentIdentityRegistry registry;
    auto opened = UiCanvasDocument::Open(OpenIdentity(registry), project.DocumentPath());
    REQUIRE(opened.HasValue());
    auto session = std::move(opened).Value();
    REQUIRE(session.Replace(MakeDocument(2)).HasValue());
    const auto before = session.CurrentState();

    struct Interference final : Horo::TestSupport::NativePublicationFiles {
        TemporaryProject &project;
        UiCanvasDocument &canvas;
        bool blocked{};

        Interference(TemporaryProject &project, UiCanvasDocument &canvas) : project(project), canvas(canvas) {}

        Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes, bool &created,
                                         std::filesystem::perms permissions) override {
            const auto written = native.WritePrivateDurable(path, bytes, created, permissions);
            if (written.HasValue()) {
                blocked = canvas.Replace(MakeDocument(3)).HasError();
                project.Write("external bytes after staging");
            }
            return written;
        }
    } files{project, session};

    const auto receipt = session.Save(project.Root(), mutations, files);
    REQUIRE(receipt.HasValue());
    CHECK(receipt.Value().status == UiCanvasDocumentSaveStatus::Conflict);
    CHECK(files.blocked);
    CHECK(session.CurrentState() == before);
    CHECK(session.IsDirty());
    const auto current = initial.ReadFileBounded(project.DocumentPath(), 128);
    REQUIRE(current.HasValue());
    CHECK(std::string(current.Value().begin(), current.Value().end()) == "external bytes after staging");
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

TEST_CASE("UI Canvas recovery checkpoints survive close and fence external source and newer owners",
          "[unit][editor][ui_canvas][recovery]") {
    TemporaryProject project;
    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    DocumentRecoveryService recovery(project.Root(), mutations, files);
    SaveInitialDocument(project, MakeDocument(1), mutations, files);
    const auto firstOwner = Assets::AssetId::Parse("12345678-1234-4234-8234-123456789abc").Value();
    const auto nextOwner = Assets::AssetId::Parse("12345678-1234-4234-8234-123456789def").Value();

    DocumentIdentityRegistry registry;
    auto original = UiCanvasDocument::Open(OpenIdentity(registry), project.DocumentPath());
    REQUIRE(original.HasValue());
    auto first = std::move(original).Value();
    REQUIRE(first.Replace(MakeDocument(2)).HasValue());
    REQUIRE(first.Autosave(firstOwner, recovery).HasValue());
    REQUIRE(LoadUiCanvasDocument(project.DocumentPath()).Value().document.Revision().Value() == 1);
    const auto inspected = first.InspectRecovery(recovery);
    REQUIRE(inspected.HasValue());
    REQUIRE(inspected.Value());
    REQUIRE(recovery.Discard(*inspected.Value()).HasValue());
    REQUIRE(first.Autosave(firstOwner, recovery).HasValue());
    const auto recaptured = first.InspectRecovery(recovery);
    REQUIRE(recaptured.HasValue());
    REQUIRE(recaptured.Value());
    const auto checkpoint = *recaptured.Value();
    REQUIRE(first.Close(UiCanvasClosePolicy::DiscardChanges).HasValue());
    REQUIRE(registry.Close(first.Identity().instance).HasValue());

    auto reopened = UiCanvasDocument::Open(OpenIdentity(registry), project.DocumentPath());
    REQUIRE(reopened.HasValue());
    auto document = std::move(reopened).Value();
    const auto clean = document.CurrentState();
    CHECK(clean.instance != first.Identity().instance);
    SECTION("external edit prevents applying the recovery") {
        const auto updated = SaveUiCanvasDocument(project.Root(), project.DocumentPath(), {.document = MakeDocument(8), .state = {1}}, {},
                                                  true, mutations, files);
        REQUIRE(updated.HasValue());
        CHECK(document.Recover(clean, checkpoint, recovery).HasError());
        CHECK_FALSE(document.IsDirty());
        CHECK(document.InspectRecovery(recovery).Value() == std::optional{checkpoint});
        CHECK(LoadUiCanvasDocument(project.DocumentPath()).Value().document.Revision().Value() == 8);
    }
    SECTION("explicit restore transfers recovery only after editing the new owner") {
        CHECK(document.Recover(first.CurrentState(), checkpoint, recovery).HasError());
        REQUIRE(document.Recover(clean, checkpoint, recovery).HasValue());
        CHECK(document.IsDirty());
        CHECK(document.Document().Revision().Value() == 2);
        REQUIRE(document.Replace(MakeDocument(3)).HasValue());
        REQUIRE(document.Autosave(nextOwner, recovery).HasValue());
        const auto newest = document.InspectRecovery(recovery);
        REQUIRE(newest.HasValue());
        REQUIRE(newest.Value());
        CHECK(newest.Value()->sessionOwner == nextOwner);
        CHECK(document.DiscardRecovery(nextOwner, recovery, &checkpoint).HasError());
        REQUIRE(document.Save(project.Root(), mutations, files).Value().status == UiCanvasDocumentSaveStatus::Saved);
        REQUIRE(document.DiscardRecovery(nextOwner, recovery).HasValue());
        CHECK_FALSE(document.InspectRecovery(recovery).Value());
        CHECK(LoadUiCanvasDocument(project.DocumentPath()).Value().document.Revision().Value() == 3);
    }
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

    project.Write(R"({"format":"horo.ui_canvas","formatVersion":3})");
    const auto future = LoadUiCanvasDocument(project.DocumentPath());
    REQUIRE(future.HasError());
    REQUIRE(ErrorCodeText(future.ErrorValue()) == "editor.ui_canvas_document.version_unsupported");

    const auto relative = LoadUiCanvasDocument("assets/ui/Hud.uicanvas");
    REQUIRE(relative.HasError());
    REQUIRE(ErrorCodeText(relative.ErrorValue()) == "editor.ui_canvas_document.path_invalid");
}

TEST_CASE("UI Canvas loading rejects malformed presentation policy", "[unit][editor][ui_canvas][presentation]") {
    TemporaryProject project;
    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    SaveInitialDocument(project, MakeDocument(1), mutations, files);

    std::ifstream input(project.DocumentPath(), std::ios::binary);
    REQUIRE(static_cast<bool>(input));
    auto serialized = nlohmann::json::parse(std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>{}));
    auto legacy = LegacyEnvelope(serialized);
    legacy["canvases"][0].erase("presentation");
    project.Write(legacy.dump(2) + '\n');
    const auto loadedLegacy = LoadUiCanvasDocument(project.DocumentPath());
    REQUIRE(loadedLegacy.HasValue());
    REQUIRE(loadedLegacy.Value().document.Canvases().front().presentation == UiCanvasPresentationPolicy{});

    serialized["document"]["canvases"][0]["presentation"]["fontScale"]["denominator"] = 0;
    project.Write(serialized.dump(2) + '\n');

    const auto loaded = LoadUiCanvasDocument(project.DocumentPath());
    REQUIRE(loaded.HasError());
    REQUIRE(ErrorCodeText(loaded.ErrorValue()) == "runtime_ui.document.serialization_invalid");
}

TEST_CASE("UI Canvas envelope rejects duplicate fields before canonical source decoding", "[unit][editor][ui_canvas]") {
    TemporaryProject project;
    NativeDurableFileSystem files;
    ProjectMutationCoordinator mutations(files);
    SaveInitialDocument(project, MakeDocument(1), mutations, files);
    std::ifstream input(project.DocumentPath(), std::ios::binary);
    const std::string encoded{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    input.close();
    REQUIRE(LoadUiCanvasDocument(project.DocumentPath()).HasValue());
    for (const auto field : {"document", "revision"}) {
        auto duplicate = encoded;
        const auto marker = std::string{"\""} + field + "\":";
        const auto position = duplicate.find(marker);
        REQUIRE(position != std::string::npos);
        duplicate.insert(position, marker + (std::string_view{field} == "document" ? "{}," : "999,"));
        project.Write(duplicate);
        const auto rejected = LoadUiCanvasDocument(project.DocumentPath());
        REQUIRE(rejected.HasError());
        REQUIRE(ErrorCodeText(rejected.ErrorValue()) == "editor.ui_canvas_document.malformed");
    }
    project.Write(encoded);
    REQUIRE(LoadUiCanvasDocument(project.DocumentPath()).HasValue());
}
