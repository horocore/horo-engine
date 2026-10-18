#include "../../support/MalformedNativeFileTestSupport.h"
#include "Horo/Editor/SourceFileOpenService.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace {
    using namespace Horo::Editor;

    class TemporaryProject final {
    public:
        TemporaryProject() {
            const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
            root_ = std::filesystem::temp_directory_path() / ("horo-source-open-" + std::to_string(nonce));
            std::filesystem::create_directories(root_ / "source" / "gameplay");
            std::filesystem::create_directories(root_ / "assets" / "scripts");
            std::filesystem::create_directories(root_ / "kaynak" / "oyuncu");
        }

        ~TemporaryProject() {
            std::error_code error;
            std::filesystem::remove_all(root_, error);
        }

        void Write(const std::filesystem::path &relative, const std::string &contents = "source") const {
            const std::filesystem::path path = root_ / relative;
            std::filesystem::create_directories(path.parent_path());
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << contents;
            REQUIRE(static_cast<bool>(output));
        }

        [[nodiscard]] const std::filesystem::path &Root() const noexcept {
            return root_;
        }

    private:
        std::filesystem::path root_;
    };

    [[nodiscard]] std::string ErrorCodeText(const Horo::Error &error) {
        return std::string{error.code.Value()};
    }

    /** @brief Requires typed rejection before document admission for every initialized native path. */
    void RequireUnsafeNativePath(SourceFileOpenService &service, DocumentIdentityRegistry &registry, const std::filesystem::path &input) {
        REQUIRE_FALSE(input.empty());
        const auto opened = service.Open({.path = input, .origin = SourceOpenOrigin::Command});
        REQUIRE(opened.HasError());
        CHECK(ErrorCodeText(opened.ErrorValue()) == "editor.source_open.unsafe");
        CHECK(registry.Size() == 0);
    }

}  // namespace

TEST_CASE("Source policy classifies editable source and project text by extension", "[unit][editor][source]") {
    TemporaryProject project;
    SourceFileOpenService service{project.Root()};

    REQUIRE(service.Classify("Player.CPP").kind == SourceFileKind::NativeSource);
    REQUIRE(service.Classify("player.horo_script").kind == SourceFileKind::HoroScript);
    REQUIRE(service.Classify("assets/ui/Hud.UICANVAS").kind == SourceFileKind::UiCanvas);
    REQUIRE(service.Classify("assets/cinematics/Intro.HSEQUENCE").kind == SourceFileKind::Sequence);
    REQUIRE(service.Classify(".horo/project.JSON").kind == SourceFileKind::ProjectText);
    REQUIRE(service.Classify("README.bin").kind == SourceFileKind::Unsupported);
    REQUIRE(service.Classify(".gitignore").kind == SourceFileKind::ProjectText);

    SourceFilePolicy projectPolicy;
    projectPolicy.projectTextExtensions = {"project_source"};
    SourceFileOpenService customized{project.Root(), std::move(projectPolicy)};
    REQUIRE(customized.Classify("generated.PROJECT_SOURCE").kind == SourceFileKind::ProjectText);
}

TEST_CASE("UI canvas opening uses a distinct persistent document kind and focuses existing tabs", "[unit][editor][source][ui]") {
    TemporaryProject project;
    project.Write("assets/ui/Hud.uicanvas");
    project.Write("assets/ui/Menu.uicanvas");

    DocumentIdentityRegistry registry;
    SourceFileOpenService service{project.Root(), registry};
    const auto hud = service.Open(SourceOpenRequest{
        .path = "assets/ui/Hud.uicanvas",
        .origin = SourceOpenOrigin::AssetActivation,
        .mode = SourceOpenMode::EmbeddedOnly,
    });
    REQUIRE(hud.HasValue());
    REQUIRE(hud.Value().classification.kind == SourceFileKind::UiCanvas);
    REQUIRE(hud.Value().document.has_value());
    REQUIRE(hud.Value().document->identity.key.kind == DocumentKind::UiCanvas);
    REQUIRE(hud.Value().document->disposition == DocumentOpenDisposition::Opened);

    const auto focused = service.Open(SourceOpenRequest{
        .path = project.Root() / "assets/ui/Hud.uicanvas",
        .origin = SourceOpenOrigin::Command,
        .mode = SourceOpenMode::EmbeddedOnly,
    });
    REQUIRE(focused.HasValue());
    REQUIRE(focused.Value().document->disposition == DocumentOpenDisposition::FocusExisting);
    REQUIRE(focused.Value().document->identity == hud.Value().document->identity);

    const auto menu = service.Open(SourceOpenRequest{
        .path = "assets/ui/Menu.uicanvas",
        .origin = SourceOpenOrigin::AssetActivation,
        .mode = SourceOpenMode::EmbeddedOnly,
    });
    REQUIRE(menu.HasValue());
    REQUIRE(menu.Value().document->identity.key.kind == DocumentKind::UiCanvas);
    REQUIRE(menu.Value().document->identity != hud.Value().document->identity);
    REQUIRE(registry.Size() == 2);
}

TEST_CASE("Source open service moves preserve borrowed and owned registry lifetimes", "[unit][editor][source][lifetime]") {
    TemporaryProject project;
    project.Write("source/gameplay/Player.cpp");

    const SourceOpenRequest request{
        .path = "source/gameplay/Player.cpp",
        .origin = SourceOpenOrigin::Command,
    };

    DocumentIdentityRegistry external;
    SourceFileOpenService borrowed{project.Root(), external};
    const auto borrowedOpen = borrowed.Open(request);
    REQUIRE(borrowedOpen.HasValue());
    const DocumentIdentity borrowedIdentity = borrowedOpen.Value().document->identity;

    SourceFileOpenService movedBorrowed{std::move(borrowed)};
    const auto movedBorrowedOpen = movedBorrowed.Open(request);
    REQUIRE(movedBorrowedOpen.HasValue());
    REQUIRE(movedBorrowedOpen.Value().document->disposition == DocumentOpenDisposition::FocusExisting);
    REQUIRE(movedBorrowedOpen.Value().document->identity == borrowedIdentity);
    REQUIRE(&movedBorrowed.DocumentRegistry() == &external);

    DocumentIdentityRegistry destination;
    SourceFileOpenService assignedBorrowed{project.Root(), destination};
    assignedBorrowed = std::move(movedBorrowed);
    const auto assignedBorrowedOpen = assignedBorrowed.Open(request);
    REQUIRE(assignedBorrowedOpen.HasValue());
    REQUIRE(assignedBorrowedOpen.Value().document->disposition == DocumentOpenDisposition::FocusExisting);
    REQUIRE(assignedBorrowedOpen.Value().document->identity == borrowedIdentity);
    REQUIRE(&assignedBorrowed.DocumentRegistry() == &external);
    REQUIRE(destination.Size() == 0);

    SourceFileOpenService owned{project.Root()};
    const auto ownedOpen = owned.Open(request);
    REQUIRE(ownedOpen.HasValue());
    const DocumentIdentity ownedIdentity = ownedOpen.Value().document->identity;

    SourceFileOpenService movedOwned{std::move(owned)};
    const auto movedOwnedOpen = movedOwned.Open(request);
    REQUIRE(movedOwnedOpen.HasValue());
    REQUIRE(movedOwnedOpen.Value().document->disposition == DocumentOpenDisposition::FocusExisting);
    REQUIRE(movedOwnedOpen.Value().document->identity == ownedIdentity);

    SourceFileOpenService assignedOwned{project.Root()};
    assignedOwned = std::move(movedOwned);
    const auto assignedOwnedOpen = assignedOwned.Open(request);
    REQUIRE(assignedOwnedOpen.HasValue());
    REQUIRE(assignedOwnedOpen.Value().document->disposition == DocumentOpenDisposition::FocusExisting);
    REQUIRE(assignedOwnedOpen.Value().document->identity == ownedIdentity);
    REQUIRE(assignedOwned.DocumentRegistry().Size() == 1);
}

TEST_CASE("Relative absolute and internal symlink paths focus one canonical source document", "[unit][editor][source][paths]") {
    TemporaryProject project;
    project.Write("source/gameplay/Player Move.cpp");
    const std::filesystem::path source = project.Root() / "source/gameplay/Player Move.cpp";
    const std::filesystem::path link = project.Root() / "assets/scripts/player-link.cpp";
    std::error_code symlinkError;
    std::filesystem::create_symlink(source, link, symlinkError);
    if (symlinkError)
        SKIP("This host does not permit source-file symlink creation: " << symlinkError.message());

    SourceFileOpenService service{project.Root()};
    const auto relative = service.Open(SourceOpenRequest{
        .path = "source/gameplay/Player Move.cpp",
        .origin = SourceOpenOrigin::AssetActivation,
    });
    REQUIRE(relative.HasValue());
    REQUIRE(relative.Value().route == SourceOpenRoute::EmbeddedWorkspace);
    REQUIRE(relative.Value().document.has_value());
    REQUIRE(relative.Value().document->disposition == DocumentOpenDisposition::Opened);
    REQUIRE(relative.Value().location.document.Value() == "source/gameplay/Player Move.cpp");

    const auto absolute = service.Open(SourceOpenRequest{
        .path = source,
        .origin = SourceOpenOrigin::DiagnosticNavigation,
        .line = 12,
        .column = 7,
    });
    REQUIRE(absolute.HasValue());
    REQUIRE(absolute.Value().document.has_value());
    REQUIRE(absolute.Value().document->disposition == DocumentOpenDisposition::FocusExisting);
    REQUIRE(absolute.Value().document->identity == relative.Value().document->identity);
    REQUIRE(absolute.Value().line == 12U);
    REQUIRE(absolute.Value().column == 7U);

    const auto symlink = service.Open(SourceOpenRequest{
        .path = link,
        .origin = SourceOpenOrigin::Command,
    });
    REQUIRE(symlink.HasValue());
    REQUIRE(symlink.Value().document.has_value());
    REQUIRE(symlink.Value().document->disposition == DocumentOpenDisposition::FocusExisting);
    REQUIRE(symlink.Value().location.absolutePath == relative.Value().location.absolutePath);
    REQUIRE(symlink.Value().location.document == relative.Value().location.document);
}

TEST_CASE("Source opening preserves spaces and non-ASCII project-relative identities", "[unit][editor][source][paths]") {
    TemporaryProject project;
    const std::filesystem::path relative{u8"kaynak/oyuncu/Player Move \u015f \u65e5\u672c \U0001f680.cpp"};
    project.Write(relative);
    SourceFileOpenService service{project.Root()};

    const auto opened = service.Open(SourceOpenRequest{
        .path = relative,
        .origin = SourceOpenOrigin::Command,
    });
    REQUIRE(opened.HasValue());
    const auto encoded = relative.generic_u8string();
    REQUIRE((opened.Value().location.document.Value() == std::string{encoded.begin(), encoded.end()}));
    CHECK(opened.Value().location.document.Value() == "kaynak/oyuncu/Player Move \xc5\x9f \xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x9a\x80.cpp");
    REQUIRE(opened.Value().location.absolutePath.filename() == relative.filename());
    const auto absolute = service.Open({.path = project.Root() / relative, .origin = SourceOpenOrigin::Command});
    REQUIRE(absolute.HasValue());
    REQUIRE(opened.Value().document.has_value());
    REQUIRE(absolute.Value().document.has_value());
    CHECK(absolute.Value().document->identity == opened.Value().document->identity);
    CHECK(absolute.Value().location.document == opened.Value().location.document);
}

TEST_CASE("Native Unicode source paths retain containment and malformed-path admission fences", "[unit][editor][source][paths]") {
    TemporaryProject project;
    DocumentIdentityRegistry registry;
    SourceFileOpenService service{project.Root(), registry};
    SECTION("Unicode traversal cannot escape") {
        RequireUnsafeNativePath(service, registry, std::filesystem::path{u8"../outside \u015f \u65e5\u672c.cpp"});
    }
    SECTION("embedded NUL cannot alias an existing source") {
        std::filesystem::path input{u8"source/gameplay/Present.cpp"};
        project.Write(input);
        input += std::filesystem::path::value_type{};
        input += "ignored.cpp";
        RequireUnsafeNativePath(service, registry, input);
    }
    SECTION("malformed native encoding cannot become a document identity") {
#ifdef _WIN32
        std::wstring text = L"source/gameplay/Bad";
        SECTION("unpaired leading surrogate") {
            text.push_back(static_cast<wchar_t>(0xd800));
        }
        SECTION("unpaired trailing surrogate") {
            text.push_back(static_cast<wchar_t>(0xdc00));
        }
        SECTION("leading surrogate followed by a non-surrogate") {
            text.push_back(static_cast<wchar_t>(0xdbff));
            text.push_back(L'A');
        }
        RequireUnsafeNativePath(service, registry, std::filesystem::path{text + L".cpp"});
#else
        const auto input = std::filesystem::path{std::string{"source/gameplay/Bad"} + "\xc0\xaf" + ".cpp"};
        constexpr std::array<std::uint8_t, 6> original{'s', 'o', 'u', 'r', 'c', 'e'};
        const bool created = Horo::TestSupport::WriteMalformedNativeFile(project.Root() / input, original);
        RequireUnsafeNativePath(service, registry, input);
        std::error_code existenceError;
        const bool remains = std::filesystem::exists(project.Root() / input, existenceError);
        CHECK(remains == created);
        if (remains) {
            std::ifstream retained{project.Root() / input, std::ios::binary};
            std::string bytes;
            retained >> bytes;
            REQUIRE(static_cast<bool>(retained));
            CHECK(bytes == "source");
        }
#endif
    }
}

TEST_CASE("Source opening rejects missing files and lexical escapes", "[unit][editor][source][security]") {
    TemporaryProject project;
    project.Write("source/gameplay/Present.cpp");
    SourceFileOpenService service{project.Root()};

    const auto missing = service.Open(SourceOpenRequest{
        .path = "source/gameplay/Missing.cpp",
        .origin = SourceOpenOrigin::DiagnosticNavigation,
    });
    REQUIRE(missing.HasError());
    REQUIRE(ErrorCodeText(missing.ErrorValue()) == "editor.source_open.missing");

    const auto escaped = service.Open(SourceOpenRequest{
        .path = project.Root() / ".." / "outside.cpp",
        .origin = SourceOpenOrigin::Command,
    });
    REQUIRE(escaped.HasError());
    REQUIRE(ErrorCodeText(escaped.ErrorValue()) == "editor.source_open.unsafe");
}

TEST_CASE("Source opening rejects symlinks that resolve outside the project", "[unit][editor][source][security]") {
    TemporaryProject project;
    const std::filesystem::path outside = project.Root().parent_path() / (project.Root().filename().string() + "-outside.cpp");
    {
        std::ofstream output(outside, std::ios::binary | std::ios::trunc);
        output << "outside";
    }
    const std::filesystem::path link = project.Root() / "source/gameplay/escape.cpp";
    std::error_code symlinkError;
    std::filesystem::create_symlink(outside, link, symlinkError);
    if (symlinkError) {
        std::error_code cleanupError;
        std::filesystem::remove(outside, cleanupError);
        SKIP("This host does not permit source-file symlink creation: " << symlinkError.message());
    }

    SourceFileOpenService service{project.Root()};
    const auto result = service.Open(SourceOpenRequest{
        .path = link,
        .origin = SourceOpenOrigin::DiagnosticNavigation,
    });
    REQUIRE(result.HasError());
    REQUIRE(ErrorCodeText(result.ErrorValue()) == "editor.source_open.unsafe");

    std::error_code cleanupError;
    std::filesystem::remove(outside, cleanupError);
}

TEST_CASE("Unsupported files use an explicit safe fallback route", "[unit][editor][source][routing]") {
    TemporaryProject project;
    project.Write("assets/model.bin");
    SourceFileOpenService service{project.Root()};

    const auto fallback = service.Open(SourceOpenRequest{
        .path = "assets/model.bin",
        .origin = SourceOpenOrigin::AssetActivation,
        .mode = SourceOpenMode::AllowExternalFallback,
    });
    REQUIRE(fallback.HasValue());
    REQUIRE(fallback.Value().route == SourceOpenRoute::ExternalEditorFallback);
    REQUIRE(fallback.Value().classification.kind == SourceFileKind::Unsupported);
    REQUIRE_FALSE(fallback.Value().document.has_value());

    const auto embeddedOnly = service.Open(SourceOpenRequest{
        .path = "assets/model.bin",
        .origin = SourceOpenOrigin::Command,
        .mode = SourceOpenMode::EmbeddedOnly,
    });
    REQUIRE(embeddedOnly.HasError());
    REQUIRE(ErrorCodeText(embeddedOnly.ErrorValue()) == "editor.source_open.unsupported");
}

TEST_CASE("Source opening reports unavailable editor capabilities explicitly", "[unit][editor][source][routing]") {
    TemporaryProject project;
    project.Write("source/gameplay/Player.cpp");
    SourceFilePolicy policy = SourceFilePolicy::Default();
    policy.embeddedEditorAvailable = false;
    policy.externalEditorAvailable = false;
    SourceFileOpenService service{project.Root(), std::move(policy)};

    const auto result = service.Open(SourceOpenRequest{
        .path = "source/gameplay/Player.cpp",
        .origin = SourceOpenOrigin::DiagnosticNavigation,
    });
    REQUIRE(result.HasError());
    REQUIRE(ErrorCodeText(result.ErrorValue()) == "editor.source_open.editor_unavailable");
}

TEST_CASE("Source opening rejects invalid roots and uses the explicit capability fallback", "[unit][editor][source][routing]") {
    TemporaryProject project;
    project.Write("source/gameplay/Player.cpp");

    SourceFileOpenService invalidRoot{project.Root() / "missing-project-root"};
    const auto invalidRootResult = invalidRoot.Open(SourceOpenRequest{
        .path = "source/gameplay/Player.cpp",
        .origin = SourceOpenOrigin::Command,
    });
    REQUIRE(invalidRootResult.HasError());
    REQUIRE(ErrorCodeText(invalidRootResult.ErrorValue()) == "editor.source_open.request_invalid");

    SourceFileOpenService service{project.Root()};
    const auto emptyRequest = service.Open(SourceOpenRequest{});
    REQUIRE(emptyRequest.HasError());
    REQUIRE(ErrorCodeText(emptyRequest.ErrorValue()) == "editor.source_open.request_invalid");

    const auto directory = service.Open(SourceOpenRequest{
        .path = "assets",
        .origin = SourceOpenOrigin::Command,
    });
    REQUIRE(directory.HasError());
    REQUIRE(ErrorCodeText(directory.ErrorValue()) == "editor.source_open.unsupported");

    SourceFilePolicy fallbackPolicy = SourceFilePolicy::Default();
    fallbackPolicy.embeddedEditorAvailable = false;
    SourceFileOpenService fallbackService{project.Root(), std::move(fallbackPolicy)};
    const auto fallback = fallbackService.Open(SourceOpenRequest{
        .path = "source/gameplay/Player.cpp",
        .origin = SourceOpenOrigin::DiagnosticNavigation,
        .mode = SourceOpenMode::AllowExternalFallback,
    });
    REQUIRE(fallback.HasValue());
    REQUIRE(fallback.Value().route == SourceOpenRoute::ExternalEditorFallback);
    REQUIRE_FALSE(fallback.Value().document.has_value());
}

TEST_CASE("Source policy can reject project-contained symlinks", "[unit][editor][source][security]") {
    TemporaryProject project;
    project.Write("source/gameplay/Player.cpp");
    const std::filesystem::path link = project.Root() / "assets/scripts/player-link.cpp";
    std::error_code symlinkError;
    std::filesystem::create_symlink(project.Root() / "source/gameplay/Player.cpp", link, symlinkError);
    if (symlinkError)
        SKIP("This host does not permit source-file symlink creation: " << symlinkError.message());

    SourceFilePolicy policy = SourceFilePolicy::Default();
    policy.allowSymlinkedFiles = false;
    SourceFileOpenService service{project.Root(), std::move(policy)};
    const auto result = service.Open(SourceOpenRequest{
        .path = link,
        .origin = SourceOpenOrigin::Command,
    });
    REQUIRE(result.HasError());
    REQUIRE(ErrorCodeText(result.ErrorValue()) == "editor.source_open.unsafe");
}
