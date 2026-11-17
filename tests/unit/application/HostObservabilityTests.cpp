#include "Horo/Application/HostObservability.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Foundation/Telemetry/Operation.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {
    class TemporaryDirectory final {
    public:
        TemporaryDirectory()
            : path(std::filesystem::temp_directory_path() /
                   ("horo-host-observability-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
            std::filesystem::create_directories(path);
        }

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

        std::filesystem::path path;
    };

    [[nodiscard]] std::string ReadText(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    [[nodiscard]] Horo::Application::HostObservabilityConfiguration MakeConfiguration(const std::filesystem::path &directory,
                                                                                      const std::string_view role) {
        return {.logging = {.logDirectory = directory,
                            .baseName = "test-host",
                            .hostName = role == "editor" ? "HoroEditor" : "horo-engine",
                            .hostVersion = "0.1.0",
                            .queueCapacity = 64,
                            .maxFileBytes = 4096,
                            .maxRolledFiles = 2,
                            .echoToStderr = false},
                .identity = {.processRole = std::string{role},
                             .engineVersion = "0.1.0",
                             .buildConfiguration = "Debug",
                             .sourceRevision = "abc123",
                             .rendererBackend = role == "editor" ? "metal" : "",
                             .deviceName = role == "editor" ? "Test GPU" : "",
                             .projectId = "project-7",
                             .projectVersion = "3"}};
    }
}  // namespace

TEST_CASE("Graphical and headless hosts share one deterministic observability lifecycle", "[application][observability][host][lifecycle]") {
    Horo::Log::Logger::Shutdown();
    TemporaryDirectory temporary;

    auto editor = Horo::Application::HostObservabilitySession::Start(MakeConfiguration(temporary.path, "editor"));
    REQUIRE(editor != nullptr);
    REQUIRE(Horo::Application::HostObservabilitySession::Start(MakeConfiguration(temporary.path, "cli")) == nullptr);
    Horo::Log::Logger::Write("Editor.Project", Horo::Log::Level::Info, "editor lifecycle");
    {
        Horo::Telemetry::OperationSpan operation{"Editor.Project", "Project.Validate"};
        REQUIRE(operation.Complete(Horo::Telemetry::SpanStatus::Succeeded));
    }
    editor.reset();
    REQUIRE(std::filesystem::is_regular_file(temporary.path / "test-host.shutdown.json"));
    const auto history = temporary.path / "operations" / "test-host.jsonl";
    REQUIRE(std::filesystem::is_regular_file(history));
    CHECK(ReadText(history).find("Project.Validate") != std::string::npos);

    auto headless = Horo::Application::HostObservabilitySession::Start(MakeConfiguration(temporary.path, "cli"));
    REQUIRE(headless != nullptr);
    Horo::Log::Logger::Write("Foundation.Host", Horo::Log::Level::Info, "headless lifecycle");
    headless.reset();

    const std::string session = ReadText(temporary.path / "test-host.session.json");
    REQUIRE(session.find(R"("previousCleanShutdown":true)") != std::string::npos);
    const std::string log = ReadText(temporary.path / "test-host.jsonl");
    REQUIRE(log.find("Host identity snapshot") != std::string::npos);
    REQUIRE(log.find(R"("process.role":"cli")") != std::string::npos);
    REQUIRE(log.find(R"("build.configuration":"Debug")") != std::string::npos);
}

TEST_CASE("Host diagnostic bundles collect bounded logs history identity and approved summaries",
          "[application][observability][host][bundle]") {
    Horo::Log::Logger::Shutdown();
    TemporaryDirectory temporary;
    const auto packages = temporary.path / "packages.json";
    std::ofstream(packages) << R"({"packages":["example"]})";

    auto session = Horo::Application::HostObservabilitySession::Start(MakeConfiguration(temporary.path, "editor"));
    REQUIRE(session != nullptr);
    const std::array sensitiveFields{
        Horo::Telemetry::Field{.key = "auth.token", .value = std::string{"must-not-leave-process"}},
    };
    const std::uint64_t acceptedBefore = Horo::Log::Logger::Statistics().acceptedRecords;
    for (int attempt = 0; attempt < 1000 && Horo::Log::Logger::Statistics().acceptedRecords == acceptedBefore; ++attempt)
        Horo::Log::Logger::Write("Editor.Project", Horo::Log::Level::Info, "bundle-test-record at /Users/example/private-project",
                                 sensitiveFields);
    REQUIRE(Horo::Log::Logger::Statistics().acceptedRecords > acceptedBefore);
    const auto result = session->GenerateDiagnosticBundle(
        {.outputPath = temporary.path / "diagnostics.zip",
         .crashMetadata =
             Horo::Application::HostDiagnosticFile{.sourcePath = temporary.path / "missing-crash.json", .archivePath = "crash/report.json"},
         .packageSummary = Horo::Application::HostDiagnosticFile{.sourcePath = packages, .archivePath = "packages/loaded.json"},
         .metadata = {{"reason", "test"}}});
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().fileCount >= 3);
    REQUIRE(result.Value().missingOptionalCount >= 1);

    const std::string archive = ReadText(result.Value().outputPath);
    REQUIRE(archive.find("manifest.json") != std::string::npos);
    REQUIRE(archive.find("logs/test-host.jsonl") != std::string::npos);
    REQUIRE(archive.find("history/test-host.operations.jsonl") != std::string::npos);
    REQUIRE(archive.find("metadata/session.json") != std::string::npos);
    REQUIRE(archive.find("packages/loaded.json") != std::string::npos);
    REQUIRE(archive.find("application.name") != std::string::npos);
    REQUIRE(archive.find("renderer.backend") != std::string::npos);
    REQUIRE(archive.find("project.id") != std::string::npos);
    REQUIRE(archive.find("missing-crash.json") == std::string::npos);
    REQUIRE(archive.find("[REDACTED_PATH]") != std::string::npos);
    REQUIRE(archive.find("[REDACTED]") != std::string::npos);
    REQUIRE(archive.find("/Users/example/private-project") == std::string::npos);
    REQUIRE(archive.find("must-not-leave-process") == std::string::npos);
}

TEST_CASE("Invalid host composition fails without leaving an active runtime", "[application][observability][host][failure]") {
    Horo::Log::Logger::Shutdown();
    TemporaryDirectory temporary;
    auto configuration = MakeConfiguration(temporary.path, "cli");
    configuration.logging.baseName.clear();
    REQUIRE(Horo::Application::HostObservabilitySession::Start(std::move(configuration)) == nullptr);
    REQUIRE_FALSE(Horo::Telemetry::Runtime::IsEnabled());
}
