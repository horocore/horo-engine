#include "Horo/Platform/ExternalProcess.h"
#include "navigation/NavigationProjectBakeInputFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace Horo::Application {
    namespace {
        constexpr std::string_view Definition = "00000000-0000-0000-0000-000000000001";

        /** @brief Keeps native process argument arrays in the platform's canonical UTF-8 spelling. */
        std::string Utf8(const std::filesystem::path &path) {
            const auto value = path.generic_u8string();
            return {value.begin(), value.end()};
        }

        /** @brief Owns real project sources and shell-free child drainage through every assertion path. */
        struct NativeCli final {
            NavigationInputTestSupport::Project project;
            NativeExternalProcessRunner processes;

            std::filesystem::path Namespace(std::string_view kind) const {
                return project.source.directory.Path() / ".horo" / kind / "navigation" / Definition / "headless-null";
            }

            nlohmann::json Run(std::vector<std::string> options, int exitCode = 0) {
                std::vector<std::string> arguments{"navigation",   "bake",
                                                   "--project",    Utf8(project.source.directory.Path()),
                                                   "--definition", std::string{Definition},
                                                   "--output",     "json"};
                arguments.insert(arguments.end(), std::make_move_iterator(options.begin()), std::make_move_iterator(options.end()));
                std::string output;
                std::string diagnostics;
                bool truncated{};
                const ExternalProcessRequest request{.executable = HORO_TEST_ENGINE_EXECUTABLE,
                                                     .arguments = std::move(arguments),
                                                     .timeout = std::chrono::seconds{30},
                                                     .maximumLineBytes = 1U << 20U,
                                                     .onOutput = [&truncated, &output, &diagnostics](const ProcessOutputLine &line) {
                    truncated = truncated || line.truncated;
                    (line.stream == ProcessOutputStream::StandardOutput ? output : diagnostics) += line.text + '\n';
                }};
                const auto result = processes.Run(request, {});
                INFO(output);
                INFO(diagnostics);
                REQUIRE(result.HasValue());
                REQUIRE(result.Value().reason == ProcessTerminationReason::Exited);
                REQUIRE(result.Value().exitCode == exitCode);
                REQUIRE_FALSE(truncated);
                const auto envelope = nlohmann::json::parse(output);
                CHECK(envelope.at("exitCode") == exitCode);
                return envelope;
            }
        };
    }  // namespace

    TEST_CASE("Native CLI validates without output then builds and reuses the real project cache", "[navigation][cli][integration]") {
        NativeCli fixture;
        const auto validated = fixture.Run({"--validate-only"});
        CHECK(validated.at("result").at("validated") == true);
        CHECK(validated.at("result").at("published") == false);
        CHECK_FALSE(std::filesystem::exists(fixture.Namespace("cache")));
        CHECK_FALSE(std::filesystem::exists(fixture.Namespace("cooked")));
        const auto built = fixture.Run({});
        CHECK(built.at("result").at("published") == true);
        CHECK(built.at("result").at("rebuiltTiles").get<std::uint64_t>() > 0);
        CHECK(std::filesystem::is_regular_file(fixture.Namespace("cooked") / "current.json"));
        const auto selected = fixture.Run({"--surface", "00000000-0000-0000-0000-000000000002:10"});
        CHECK(selected.at("result").at("manifestDigest") == built.at("result").at("manifestDigest"));
        CHECK(selected.at("result").at("rebuiltTiles") == 0);
        CHECK(selected.at("result").at("reusedTiles").get<std::uint64_t>() > 0);
    }

    TEST_CASE("Native CLI returns canonical validation errors for malformed project source", "[navigation][cli][integration]") {
        NativeCli fixture;
        fixture.project.source.Write("Assets/first.horo", "{");
        const auto failed = fixture.Run({"--validate-only"}, 3);
        CHECK(failed.at("error").at("domain") == "horo.editor.scene_persistence");
        CHECK(failed.at("error").at("code") == "scene_persistence.invalid");
        CHECK_FALSE(std::filesystem::exists(fixture.Namespace("cache")));
        CHECK_FALSE(std::filesystem::exists(fixture.Namespace("cooked")));
    }

    TEST_CASE("Native CLI refuses an unknown Scene-qualified selection without publishing output", "[navigation][cli][integration]") {
        NativeCli fixture;
        const auto failed = fixture.Run({"--surface", "00000000-0000-0000-0000-000000000099:10"}, 3);
        CHECK(failed.at("status") == "failed");
        CHECK_FALSE(std::filesystem::exists(fixture.Namespace("cooked") / "current.json"));
    }

    TEST_CASE("Native CLI distinguishes unavailable output storage from source validation", "[navigation][cli][integration]") {
        NativeCli fixture;
        const auto target = fixture.Namespace("cooked");
        std::filesystem::create_directories(target.parent_path());
        std::ofstream blocked{target};
        blocked << "unchanged protected file";
        blocked.close();
        const auto failed = fixture.Run({}, 5);
        CHECK(failed.at("status") == "failed");
        CHECK(std::filesystem::is_regular_file(target));
        std::ifstream retained{target};
        std::string bytes{std::istreambuf_iterator<char>{retained}, std::istreambuf_iterator<char>{}};
        CHECK(bytes == "unchanged protected file");
    }

    TEST_CASE("Native CLI rejects redirected cache ancestors before any external write", "[navigation][cli][integration]") {
        NativeCli fixture;
        Horo::Tests::OwnedTestDirectory outside{"navigation outside output ü"};
        const auto parent = fixture.project.source.directory.Path() / ".horo/cache";
        std::filesystem::create_directories(parent);
        std::error_code error;
        std::filesystem::create_directory_symlink(outside.Path(), parent / "navigation", error);
        if (error)
            SKIP("Directory symlink creation is unavailable to this test account");
        const auto failed = fixture.Run({}, 6);
        CHECK(failed.at("error").at("code") == "file_storage.unsafe_object");
        CHECK(std::filesystem::is_empty(outside.Path()));
        CHECK_FALSE(std::filesystem::exists(fixture.Namespace("cooked")));
    }

    TEST_CASE("Native CLI rejects redirected publication ancestors through its retained writer", "[navigation][cli][integration]") {
        NativeCli fixture;
        Horo::Tests::OwnedTestDirectory outside{"navigation outside publication"};
        const auto parent = fixture.project.source.directory.Path() / ".horo/cooked";
        std::filesystem::create_directories(parent);
        std::error_code error;
        std::filesystem::create_directory_symlink(outside.Path(), parent / "navigation", error);
        if (error)
            SKIP("Directory symlink creation is unavailable to this test account");
        const auto failed = fixture.Run({}, 6);
        CHECK(failed.at("error").at("code") == "file_storage.unsafe_object");
        CHECK(std::filesystem::is_empty(outside.Path()));
        CHECK_FALSE(std::filesystem::exists(fixture.Namespace("cache")));
    }

    TEST_CASE("Native MCP publishes bound navigation tools while preserving exact mutation approval", "[navigation][mcp][integration]") {
        NativeCli fixture;
        std::string diagnostics;
        const auto result = fixture.processes.Run({.executable = HORO_TEST_PYTHON_EXECUTABLE,
                                                   .arguments = {HORO_TEST_NAVIGATION_MCP_SCRIPT, HORO_TEST_ENGINE_EXECUTABLE,
                                                                 Utf8(fixture.project.source.directory.Path()), std::string{Definition}},
                                                   .timeout = std::chrono::seconds{40},
                                                   .onOutput =
                                                       [&diagnostics](const ProcessOutputLine &line) {
            diagnostics += line.text + '\n';
        }},
                                                  {});
        INFO(diagnostics);
        REQUIRE(result.HasValue());
        CHECK(result.Value().reason == ProcessTerminationReason::Exited);
        CHECK(result.Value().exitCode == 0);
        CHECK_FALSE(std::filesystem::exists(fixture.Namespace("cache")));
        CHECK_FALSE(std::filesystem::exists(fixture.Namespace("cooked")));
    }
}  // namespace Horo::Application
