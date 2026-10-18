#include "CharacterSceneProductFixture.h"
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Platform/ExternalProcess.h"

#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <filesystem>
#include <vector>

namespace Horo::Application::CharacterSceneProductTest {
    namespace {
        ExternalProcessResult RunCli(std::vector<std::string> arguments) {
            NativeExternalProcessRunner runner;
            std::string output;
            ExternalProcessRequest request{.executable = HORO_TEST_CHARACTER_CLI_PATH,
                                           .arguments = std::move(arguments),
                                           .timeout = std::chrono::seconds{30},
                                           .onOutput = [&output](ProcessOutputLine line) {
                output += line.text + '\n';
            }};
            const auto result = runner.Run(request, {});
            UNSCOPED_INFO(output);
            if (result.HasError())
                UNSCOPED_INFO(result.ErrorValue().message);
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().reason == ProcessTerminationReason::Exited);
            UNSCOPED_INFO(output);
            return result.Value();
        }

        std::string Utf8Path(const std::filesystem::path &path) {
            const auto value = path.generic_u8string();
            return {value.begin(), value.end()};
        }

        std::vector<std::string> SceneArguments(const Fixture &fixture, const std::string &scene, const std::string &mode) {
            return {"--run-network-product",
                    "standalone",
                    "2",
                    "--project",
                    Utf8Path(fixture.project.project.dir.path),
                    "--scene",
                    scene,
                    "--physics",
                    mode,
                    "--cooked-root",
                    Utf8Path(fixture.project.cooked.path),
                    "--cook-target",
                    fixture.project.request.assets.target.Value(),
                    "--load-timeout-ms",
                    "10000"};
        }
    }  // namespace

#if HORO_TEST_PHYSICS_NATIVE
    TEST_CASE("Actual terminal CLI activates authored and committed cooked Character Scenes",
              "[character][scene][cli][product][canonical]") {
        const auto extraObjects = GENERATE(std::size_t{0}, std::size_t{8192});
        CAPTURE(extraObjects);
        Fixture fixture{extraObjects};
        if (extraObjects != 0)
            REQUIRE(std::filesystem::file_size(fixture.project.project.dir.path / "assets/level.horo") > (1U << 20U));
        CHECK(RunCli(SceneArguments(fixture, "source:assets/level.horo", "canonical")).exitCode == 0);
        CHECK(RunCli(SceneArguments(fixture, "cooked:" + fixture.project.sceneId.ToString(), "canonical")).exitCode == 0);
    }
#endif

    TEST_CASE("Explicit Null terminal Scene host refuses enabled Character without fallback", "[character][scene][cli][product][null]") {
        Fixture fixture;
        CHECK(RunCli(SceneArguments(fixture, "source:assets/level.horo", "null")).exitCode == 3);
    }

    TEST_CASE("Terminal Scene options reject malformed incomplete and escaping input", "[character][scene][cli][product]") {
        CHECK(RunCli({"--run-network-product", "standalone", "1", "--project", "relative", "--scene", "source:level.scene"}).exitCode == 2);
        CHECK(RunCli({"--run-network-product", "standalone", "1", "--physics", "canonical"}).exitCode == 2);
        CHECK(RunCli({"--run-network-product", "standalone", "1", "--project", "/", "--scene", "untyped.scene"}).exitCode == 2);
        Fixture fixture;
        CHECK(RunCli(SceneArguments(fixture, "source:../escape.scene", "null")).exitCode == 3);
        CHECK(RunCli(SceneArguments(fixture, "cooked:not-an-asset-id", "null")).exitCode == 3);
    }

    TEST_CASE("Terminal Scene options reject duplicate and unbounded requests before project startup", "[character][scene][cli][product]") {
        const std::string root = Utf8Path(std::filesystem::absolute(std::filesystem::temp_directory_path()));
        const std::vector<std::vector<std::string>> invalidSuffixes{{"--scene"},
                                                                    {"--scene", ""},
                                                                    {"--unknown", "value"},
                                                                    {"--project", root},
                                                                    {"--scene", "source:unused.horo"},
                                                                    {"--cooked-root", root},
                                                                    {"--cook-target", "native"},
                                                                    {"--project", root, "--scene", "source:unused.horo", "--physics",
                                                                     "automatic"},
                                                                    {"--project", root, "--scene", "source:unused.horo", "--physics",
                                                                     "null", "--physics", "canonical"},
                                                                    {"--project", root, "--scene", "source:unused.horo", "--project", root},
                                                                    {"--project", root, "--scene", "source:unused.horo", "--cooked-root",
                                                                     "relative", "--cook-target", "native"}};
        for (const auto &suffix : invalidSuffixes) {
            CAPTURE(suffix);
            std::vector<std::string> arguments{"--run-network-product", "standalone", "1"};
            arguments.insert(arguments.end(), suffix.begin(), suffix.end());
            CHECK(RunCli(std::move(arguments)).exitCode == 2);
        }
        for (const std::string_view timeout : {"0", "-1", "60001", "1ms", "18446744073709551616"}) {
            CAPTURE(timeout);
            CHECK(RunCli({"--run-network-product", "standalone", "1", "--project", root, "--scene", "source:unused.horo",
                          "--load-timeout-ms", std::string{timeout}})
                      .exitCode == 2);
        }
    }
}  // namespace Horo::Application::CharacterSceneProductTest
