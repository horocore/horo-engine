#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Editor/ProjectMigrationTransaction.h"
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Editor/ProjectOpenService.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "ProjectMigrationTestFixture.h"
#include "editor/project_model/RendererAvailability.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <nlohmann/json.hpp>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Editor;
    using namespace Horo::Tests;

    constexpr auto ProductionDefinitionId = "core.project_settings.compression_defaults";

    class MigrationLogCapture final {
    public:
        explicit MigrationLogCapture(const std::filesystem::path &root) : path_(root / "migration-integration-test.jsonl") {
            Log::Logger::Shutdown();
            Log::Logger::Init(root.string(), "migration-integration-test");
            Log::Logger::SetLevel(Log::Level::Debug);
        }

        ~MigrationLogCapture() {
            static_cast<void>(Log::Logger::Flush());
            Log::Logger::Shutdown();
        }

        [[nodiscard]] std::vector<nlohmann::json> Records() const {
            static_cast<void>(Log::Logger::Flush());
            std::ifstream input(path_, std::ios::binary);
            REQUIRE((input.good()));
            std::vector<nlohmann::json> records;
            for (std::string line; std::getline(input, line);)
                if (!line.empty())
                    records.push_back(nlohmann::json::parse(line));
            return records;
        }

    private:
        std::filesystem::path path_;
    };

    [[nodiscard]] bool HasLogCategory(const std::vector<nlohmann::json> &records, const std::string_view category) {
        return std::ranges::any_of(records, [category](const nlohmann::json &record) {
            return record.value("category", "") == category;
        });
    }

    [[nodiscard]] bool HasLogMessagePart(const std::vector<nlohmann::json> &records, const std::string_view text) {
        return std::ranges::any_of(records, [text](const nlohmann::json &record) {
            return record.value("message", "").find(text) != std::string::npos;
        });
    }

    [[nodiscard]] nlohmann::json ReadJson(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        if (!input.good())
            FAIL("Unable to read JSON fixture: " + path.generic_string());
        return nlohmann::json::parse(input);
    }

    void WriteText(const std::filesystem::path &path, const std::string_view text) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        REQUIRE((output.good()));
        output << text;
        REQUIRE((output.good()));
    }

    struct BackendProjectOpen {
        BackendProjectOpen()
            : jobs({.workerCount = 3, .maxQueuedJobs = 32}), mutations(files), transactions(files, clock, mutations, jobs),
              preflight(transactions), renderers({{{"opengl", "OpenGL", RendererAvailabilityState::Active, {}}}, "opengl"}),
              service(jobs, files, preflight, mutations, transactions, renderers) {}

        ~BackendProjectOpen() {
            service.Shutdown();
            jobs.Shutdown(ShutdownPolicy::Drain);
        }

        NativeDurableFileSystem files;
        SystemWallClock clock;
        JobSystem jobs;
        ProjectMutationCoordinator mutations;
        ProjectMigrationTransactionService transactions;
        ProjectOpenPreflightService preflight;
        RendererAvailabilitySnapshot renderers;
        ProjectOpenService service;
    };

    [[nodiscard]] ProjectOpenProgressSnapshot OpenProject(BackendProjectOpen &backend, const ProjectMigrationTestFixture &project) {
        auto started = backend.service.Start({
            .projectRoot = project.Root(),
            .expectedProjectName = "Legacy Migration Test",
            .engineBuildIdentity = "integration-test",
        });
        REQUIRE((started.HasValue()));
        return PumpProjectOpenToTerminal(backend.service, started.Value().Id());
    }

    void VerifyMigratedPrefabDocuments(const ProjectMigrationTestFixture &project) {
        const auto prefab = ReadJson(project.Root() / "assets/prefabs/player.prefab");
        REQUIRE((prefab.at("projectVersion") == "0.1.0"));
        REQUIRE((prefab.at("assetId") == "00112233-4455-6677-8899-aabbccddeeff"));
        REQUIRE_FALSE((prefab.contains("prefabId")));
        REQUIRE((prefab.at("objects").front().at("components").front().at("payload").at("bytes") == nlohmann::json::array({0, 127, 255})));
        const auto scene = ReadJson(project.Root() / "assets/scenes/main.horo");
        REQUIRE((scene.at("prefabInstances").front().at("sourceAsset") == "00112233-4455-6677-8899-aabbccddeeff"));
        REQUIRE_FALSE((scene.at("prefabInstances").front().contains("sourcePath")));
    }

    void VerifyMigrationLogs(const std::vector<nlohmann::json> &records, const ProjectMigrationTestFixture &project,
                             const std::string &projectId) {
        REQUIRE((HasLogCategory(records, "application.project_migration.plan")));
        REQUIRE((HasLogCategory(records, "application.project_migration.execute")));
        REQUIRE((HasLogCategory(records, "editor.project_migration.transaction")));
        REQUIRE((HasLogCategory(records, "editor.project_open")));
        REQUIRE((HasLogMessagePart(records, "source=0.0.1")));
        REQUIRE((HasLogMessagePart(records, "target=0.1.0")));
        REQUIRE((HasLogMessagePart(records, ProductionDefinitionId)));
        REQUIRE((HasLogMessagePart(records, "operation=")));
        REQUIRE_FALSE((HasLogMessagePart(records, project.Root().string())));
        REQUIRE_FALSE((HasLogMessagePart(records, "Legacy Migration Test")));
        REQUIRE_FALSE((HasLogMessagePart(records, projectId)));
        REQUIRE((std::ranges::none_of(records, [](const nlohmann::json &record) {
            return record.value("message", "").find("unknownNested") != std::string::npos;
        })));
    }

    void RequireProjectOpenFailureWithoutMutation(ProjectMigrationTestFixture &project) {
        const std::string metadata = project.ReadProjectBytes();
        BackendProjectOpen backend;
        const auto opened = OpenProject(backend, project);
        REQUIRE((opened.outcome == ProjectOpenOutcome::Failed));
        REQUIRE((opened.diagnostic.has_value()));
        REQUIRE((opened.diagnostic->code.Value() == "project.migration.stage_failed"));
        REQUIRE_FALSE((opened.diagnostic->message.empty()));
        REQUIRE_FALSE((opened.readySession.has_value()));
        REQUIRE((project.ReadProjectBytes() == metadata));
        REQUIRE_FALSE((std::filesystem::exists(project.Root() / ".horo/migration_history.json")));
    }
}  // namespace

TEST_CASE("Legacy 0.0.1 project migrates to 0.1.0 through backend project open", "[integration][project][migration]") {
    REQUIRE((ComputeTestSha256("abc") == "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    ProjectMigrationTestFixture project;
    MigrationLogCapture logs(project.LogRoot());
    BackendProjectOpen backend;

    const auto preflight = backend.preflight.Inspect(project.Root());
    REQUIRE((preflight.compatibility.status == ProjectCompatibilityStatus::AutomaticMigrationRequired));
    REQUIRE((preflight.migrationPlan.has_value()));
    REQUIRE((preflight.migrationPlan->definitions.size() == 1));
    REQUIRE((preflight.migrationPlan->definitions.front().id.value == ProductionDefinitionId));

    const auto opened = OpenProject(backend, project);
    REQUIRE((opened.outcome == ProjectOpenOutcome::ReadyToActivate));
    REQUIRE((opened.readySession.has_value()));

    const auto migrated = project.ReadProjectJson();
    REQUIRE((migrated.at("horoVersion") == "0.1.0"));
    REQUIRE((migrated.at("settings").at("assetCompression") == "lz4"));
    REQUIRE((migrated.at("settings").at("textureCompression") == "bc7"));
    REQUIRE((migrated.at("settings").at("unknownNested").at("label") == "preserved"));
    REQUIRE((migrated.at("unknownRoot").at("enabled") == true));
    REQUIRE((migrated.contains("migrationHistoryHead")));
    REQUIRE((migrated.at("migrationHistoryHead").is_string()));
    REQUIRE_FALSE((migrated.at("migrationHistoryHead").get<std::string>().empty()));
    REQUIRE((migrated.at("migrationHistoryHead").get<std::string>() == ComputeTestSha256(project.ReadHistoryBytes())));

    VerifyMigratedPrefabDocuments(project);

    const auto history = project.ReadHistoryJson();
    REQUIRE((history.at("receipts").size() == 1));
    REQUIRE((history.at("receipts").front().at("definitions").size() == 1));
    REQUIRE((history.at("receipts").front().at("definitions").front().at("id") == ProductionDefinitionId));

    const auto activeMigrationRoot = project.Root() / ".horo/local/migration";
    REQUIRE((!std::filesystem::exists(activeMigrationRoot) || std::filesystem::is_empty(activeMigrationRoot)));

    auto reserved = backend.service.ReserveSession(*opened.readySession);
    REQUIRE((reserved.HasValue()));
    REQUIRE((reserved.Value().Candidate().projectRoot == project.Root()));
    auto activation = std::move(reserved).Value();
    REQUIRE((activation.Commit().HasValue()));
    REQUIRE((backend.service.ReserveSession(*opened.readySession).HasError()));

    const auto reopened = OpenProject(backend, project);
    REQUIRE((reopened.outcome == ProjectOpenOutcome::ReadyToActivate));
    REQUIRE((reopened.readySession.has_value()));
    REQUIRE((project.ReadHistoryJson().at("receipts").size() == 1));
    REQUIRE((backend.service.DiscardSession(*reopened.readySession).HasValue()));

    VerifyMigrationLogs(logs.Records(), project, migrated.at("projectId").get<std::string>());
}

TEST_CASE("Invalid legacy project fails without authoritative mutation", "[integration][project][migration]") {
    ProjectMigrationTestFixture project;
    MigrationLogCapture logs(project.LogRoot());
    auto invalid = project.ReadProjectJson();
    invalid["settings"]["assetCompression"] = "brotli";
    {
        std::ofstream output(project.Root() / ".horo/project.json", std::ios::binary | std::ios::trunc);
        REQUIRE((output.good()));
        output << invalid.dump(2) << '\n';
        REQUIRE((output.good()));
    }
    RequireProjectOpenFailureWithoutMutation(project);
    const auto records = logs.Records();
    REQUIRE((std::ranges::any_of(records, [](const nlohmann::json &record) {
        return record.value("level", "") == "error" &&
               record.value("message", "").find("project.migration.stage_failed") != std::string::npos;
    })));
}

TEST_CASE("Prefab migration rejects malformed and orphaned authored data transactionally", "[integration][project][migration][prefab]") {
    ProjectMigrationTestFixture project;

    SECTION("malformed stable identity") {
        WriteText(project.Root() / "assets/prefabs/player.prefab.horo",
                  R"({"schemaVersion":1,"assetId":"not-a-canonical-asset-id","assetType":"core.prefab"})");
    }
    SECTION("duplicate JSON key") {
        WriteText(
            project.Root() / "assets/prefabs/player.prefab",
            R"({"projectVersion":"0.0.1","prefabId":"00112233-4455-6677-8899-aabbccddeeff","prefabId":"00112233-4455-6677-8899-aabbccddeeff"})");
    }
    SECTION("orphaned prefab source") {
        std::error_code error;
        REQUIRE((std::filesystem::remove(project.Root() / "assets/prefabs/player.prefab.horo", error)));
        REQUIRE_FALSE((error));
    }

    RequireProjectOpenFailureWithoutMutation(project);
}

TEST_CASE("Prefab migration discovers portable uppercase authored extensions", "[integration][project][migration][prefab]") {
    ProjectMigrationTestFixture project;
    std::error_code error;
    std::filesystem::rename(project.Root() / "assets/prefabs/player.prefab", project.Root() / "assets/prefabs/player.PREFAB", error);
    REQUIRE_FALSE((error));
    std::filesystem::rename(project.Root() / "assets/prefabs/player.prefab.horo", project.Root() / "assets/prefabs/player.PREFAB.HORO",
                            error);
    REQUIRE_FALSE((error));

    BackendProjectOpen backend;
    const auto opened = OpenProject(backend, project);
    REQUIRE((opened.outcome == ProjectOpenOutcome::ReadyToActivate));
    REQUIRE((ReadJson(project.Root() / "assets/prefabs/player.PREFAB").at("assetId") == "00112233-4455-6677-8899-aabbccddeeff"));
}
