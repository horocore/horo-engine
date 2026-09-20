#include "Horo/Editor/SourceFileOpenService.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

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
}  // namespace

TEST_CASE("Source policy classifies editable source and project text by extension", "[unit][editor][source]") {
    TemporaryProject project;
    SourceFileOpenService service{project.Root()};

    REQUIRE(service.Classify("Player.CPP").kind == SourceFileKind::NativeSource);
    REQUIRE(service.Classify("player.horo_script").kind == SourceFileKind::HoroScript);
    REQUIRE(service.Classify(".horo/project.JSON").kind == SourceFileKind::ProjectText);
    REQUIRE(service.Classify("README.bin").kind == SourceFileKind::Unsupported);
    REQUIRE(service.Classify(".gitignore").kind == SourceFileKind::ProjectText);

    SourceFilePolicy projectPolicy;
    projectPolicy.projectTextExtensions = {"project_source"};
    SourceFileOpenService customized{project.Root(), std::move(projectPolicy)};
    REQUIRE(customized.Classify("generated.PROJECT_SOURCE").kind == SourceFileKind::ProjectText);
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
    REQUIRE(symlink.Value().location.absolutePath == source);
    REQUIRE(symlink.Value().location.document == relative.Value().location.document);
}

TEST_CASE("Source opening preserves spaces and non-ASCII project-relative identities", "[unit][editor][source][paths]") {
    TemporaryProject project;
    project.Write("kaynak/oyuncu/Player Move ş.cpp");
    SourceFileOpenService service{project.Root()};

    const auto opened = service.Open(SourceOpenRequest{
        .path = "kaynak/oyuncu/Player Move ş.cpp",
        .origin = SourceOpenOrigin::Command,
    });
    REQUIRE(opened.HasValue());
    REQUIRE(opened.Value().location.document.Value() == "kaynak/oyuncu/Player Move ş.cpp");
    REQUIRE(opened.Value().location.absolutePath.filename() == "Player Move ş.cpp");
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
