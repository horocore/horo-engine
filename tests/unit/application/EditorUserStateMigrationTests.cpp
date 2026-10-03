#include "EditorUserStateMigration.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

using namespace Horo::Editor;

namespace {
    class TemporaryState final {
    public:
        TemporaryState()
            : root(std::filesystem::temp_directory_path() /
                   ("horo-editor-user-state-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::filesystem::create_directories(root / "state");
            std::filesystem::create_directories(root / "cache");
            std::filesystem::create_directories(root / "project");
        }

        ~TemporaryState() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }

        std::filesystem::path root;
    };

    void Write(const std::filesystem::path &path, const std::string &bytes) {
        std::ofstream output(path, std::ios::binary);
        output << bytes;
    }

    [[nodiscard]] std::string Read(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }
}  // namespace

TEST_CASE("Editor startup upgrades legacy preferences and recent-project records without rewriting projects", "[release][user-state]") {
    TemporaryState state;
    Horo::NativeDurableFileSystem files;
    Write(state.root / "state" / "editor_settings.json", R"({"editor":{"languageTag":"tr-TR"},"unknown":42})");
    Write(state.root / "state" / "recent_projects.json", R"([{"name":"Example","rootPath":"/project"}])");
    Write(state.root / "project" / "project.json", "project-data");
    auto result = MigrateLegacyEditorUserState(state.root / "state", state.root / "cache", files);
    REQUIRE(result.HasValue());
    CHECK(result.Value().transformed == 2U);
    const auto settings = nlohmann::json::parse(Read(state.root / "state" / "editor_settings.json"));
    CHECK(settings["schemaVersion"] == 1);
    CHECK(settings["editor"]["languageTag"] == "tr-TR");
    CHECK(settings["unknown"] == 42);
    const auto recent = nlohmann::json::parse(Read(state.root / "state" / "recent_projects.json"));
    CHECK(recent["schemaVersion"] == 1);
    CHECK(recent["entries"].size() == 1U);
    CHECK(Read(state.root / "state" / "editor_settings.json.horo-backup-v0") == R"({"editor":{"languageTag":"tr-TR"},"unknown":42})");
    CHECK(Read(state.root / "project" / "project.json") == "project-data");
    auto repeat = MigrateLegacyEditorUserState(state.root / "state", state.root / "cache", files);
    REQUIRE(repeat.HasValue());
    CHECK(repeat.Value().transformed == 0U);
}

TEST_CASE("Malformed or future editor user state remains untouched for explicit repair", "[release][user-state]") {
    TemporaryState state;
    Horo::NativeDurableFileSystem files;
    Write(state.root / "state" / "editor_settings.json", R"({"schemaVersion":2,"editor":{}})");
    Write(state.root / "state" / "recent_projects.json", R"([{"name":"Example"}])");
    CHECK(MigrateLegacyEditorUserState(state.root / "state", state.root / "cache", files).HasError());
    CHECK(Read(state.root / "state" / "editor_settings.json") == R"({"schemaVersion":2,"editor":{}})");
    CHECK(Read(state.root / "state" / "recent_projects.json") == R"([{"name":"Example"}])");
}
