#include "../../support/project/ProjectMigrationFailingFilesystem.h"
#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Application/ProjectMigrationCatalog.h"
#include "Horo/Editor/ProjectMigrationTransaction.h"
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Editor/ProjectOpenService.h"
#include "Horo/Foundation/JobSystem.h"
#include "Horo/Foundation/Logging/Logger.h"
#include "Horo/Navigation/NavigationDataSerialization.h"
#include "ProjectMigrationTestFixture.h"
#include "editor/project_model/RendererAvailability.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Editor;
    using namespace Horo::Tests;

    constexpr auto ProductionDefinitionId = "core.project_settings.compression_defaults";
    constexpr auto AuthoringDefinitionId = "core.authoring.navigation_network";

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

    [[nodiscard]] std::string ReadBytes(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        REQUIRE((input.good()));
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    struct BackendProjectOpen {
        explicit BackendProjectOpen(DurableFileSystem *injected = nullptr)
            : jobs({.workerCount = 2, .maxQueuedJobs = 32}), mutations(injected ? *injected : files),
              transactions(injected ? *injected : files, clock, mutations, jobs), preflight(transactions),
              renderers({{{"opengl", "OpenGL", RendererAvailabilityState::Active, {}}}, "opengl"}),
              service(jobs, injected ? *injected : files, preflight, mutations, transactions, renderers) {}

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

    /** @brief Leave a failed publication pending and return its recovery operation identity. */
    [[nodiscard]] std::string PreparePendingMigration(const ProjectMigrationTestFixture &project,
                                                      MigrationFailure::FailingFilesystem &files, const std::string &originalRoot) {
        BackendProjectOpen backend(&files);
        REQUIRE(OpenProject(backend, project).outcome != ProjectOpenOutcome::ReadyToActivate);
        REQUIRE(project.ReadProjectBytes() == originalRoot);
        const auto recovery = backend.transactions.InspectPendingRecovery(project.Root());
        REQUIRE(recovery.action == MigrationRecoveryAction::ResumePublish);
        REQUIRE(recovery.operationId.has_value());
        return *recovery.operationId;
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

    /** @brief Verify the recorded definition identities and immutable catalog hash. */
    void VerifyMigrationReceipt(const ProjectMigrationTestFixture &project) {
        const auto history = project.ReadHistoryJson();
        REQUIRE((history.at("receipts").size() == 1));
        REQUIRE((history.at("receipts").front().at("definitions").size() == 2));
        REQUIRE((history.at("receipts").front().at("definitions").front().at("id") == ProductionDefinitionId));
        REQUIRE((history.at("receipts").front().at("definitions").back().at("id") == AuthoringDefinitionId));
        const auto catalog = BuildBuiltInProjectMigrationCatalog();
        REQUIRE(catalog.HasValue());
        const auto definition = std::ranges::find(catalog.Value(), std::string{ProductionDefinitionId}, [](const auto &entry) {
            return entry.id.value;
        });
        REQUIRE(definition != catalog.Value().end());
        constexpr std::string_view digits = "0123456789abcdef";
        std::string definitionHash = "sha256:";
        for (const auto byte : definition->hash.bytes) {
            definitionHash.push_back(digits[byte >> 4]);
            definitionHash.push_back(digits[byte & 15]);
        }
        REQUIRE(history.at("receipts").front().at("definitions").front().at("hash") == definitionHash);
    }

    void VerifyMigrationLogs(const std::vector<nlohmann::json> &records, const ProjectMigrationTestFixture &project,
                             const std::string &projectId) {
        REQUIRE((HasLogCategory(records, "application.project_migration.plan")));
        REQUIRE((HasLogCategory(records, "application.project_migration.execute")));
        REQUIRE((HasLogCategory(records, "editor.project_migration.transaction")));
        REQUIRE((HasLogCategory(records, "editor.project_open")));
        REQUIRE((HasLogMessagePart(records, "source=0.0.1")));
        REQUIRE((HasLogMessagePart(records, "target=0.2.0")));
        REQUIRE((HasLogMessagePart(records, ProductionDefinitionId)));
        REQUIRE((HasLogMessagePart(records, "operation=")));
        REQUIRE_FALSE((HasLogMessagePart(records, project.Root().string())));
        REQUIRE_FALSE((HasLogMessagePart(records, "Legacy Migration Test")));
        REQUIRE_FALSE((HasLogMessagePart(records, projectId)));
        REQUIRE((std::ranges::none_of(records, [](const nlohmann::json &record) {
            return record.value("message", "").find("unknownNested") != std::string::npos;
        })));
    }

    void RequireProjectOpenFailureWithoutMutation(ProjectMigrationTestFixture &project,
                                                  const std::string_view expectedCode = "project.migration.stage_failed") {
        const std::string metadata = project.ReadProjectBytes();
        const auto prefabPath = project.Root() / "assets/prefabs/player.prefab";
        const auto sidecarPath = project.Root() / "assets/prefabs/player.prefab.horo";
        const auto scenePath = project.Root() / "assets/scenes/main.horo";
        const std::string prefab = ReadBytes(prefabPath);
        const std::string scene = ReadBytes(scenePath);
        const bool hasSidecar = std::filesystem::exists(sidecarPath);
        const std::string sidecar = hasSidecar ? ReadBytes(sidecarPath) : std::string{};
        BackendProjectOpen backend;
        const auto opened = OpenProject(backend, project);
        REQUIRE((opened.outcome == ProjectOpenOutcome::Failed));
        REQUIRE((opened.diagnostic.has_value()));
        INFO(opened.diagnostic->code.Value());
        REQUIRE((opened.diagnostic->code.Value() == expectedCode));
        REQUIRE_FALSE((opened.diagnostic->message.empty()));
        REQUIRE_FALSE((opened.readySession.has_value()));
        REQUIRE((project.ReadProjectBytes() == metadata));
        REQUIRE((ReadBytes(prefabPath) == prefab));
        REQUIRE((ReadBytes(scenePath) == scene));
        REQUIRE((std::filesystem::exists(sidecarPath) == hasSidecar));
        if (hasSidecar)
            REQUIRE((ReadBytes(sidecarPath) == sidecar));
        REQUIRE_FALSE((std::filesystem::exists(project.Root() / ".horo/migration_history.json")));
    }
}  // namespace

TEST_CASE("Legacy 0.0.1 project migrates through immutable 0.1.0 to 0.2.0 through backend project open",
          "[integration][project][migration]") {
    REQUIRE((ComputeTestSha256("abc") == "sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    ProjectMigrationTestFixture project;
    MigrationLogCapture logs(project.LogRoot());
    BackendProjectOpen backend;

    const auto preflight = backend.preflight.Inspect(project.Root());
    REQUIRE((preflight.compatibility.status == ProjectCompatibilityStatus::AutomaticMigrationRequired));
    REQUIRE((preflight.migrationPlan.has_value()));
    REQUIRE((preflight.migrationPlan->definitions.size() == 2));
    REQUIRE((preflight.migrationPlan->definitions.front().id.value == ProductionDefinitionId));
    REQUIRE((preflight.migrationPlan->definitions.back().id.value == AuthoringDefinitionId));

    const auto opened = OpenProject(backend, project);
    REQUIRE((opened.outcome == ProjectOpenOutcome::ReadyToActivate));
    REQUIRE((opened.readySession.has_value()));

    const auto migrated = project.ReadProjectJson();
    REQUIRE((migrated.at("horoVersion") == "0.2.0"));
    REQUIRE_FALSE((migrated.at("settings").contains("network")));
    REQUIRE((migrated.at("settings").at("assetCompression") == "lz4"));
    REQUIRE((migrated.at("settings").at("textureCompression") == "bc7"));
    REQUIRE((migrated.at("settings").at("unknownNested").at("label") == "preserved"));
    REQUIRE((migrated.at("unknownRoot").at("enabled") == true));
    REQUIRE((migrated.contains("migrationHistoryHead")));
    REQUIRE((migrated.at("migrationHistoryHead").is_string()));
    REQUIRE_FALSE((migrated.at("migrationHistoryHead").get<std::string>().empty()));
    REQUIRE((migrated.at("migrationHistoryHead").get<std::string>() == ComputeTestSha256(project.ReadHistoryBytes())));

    VerifyMigratedPrefabDocuments(project);

    VerifyMigrationReceipt(project);

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

TEST_CASE("0.2 migration classifies Navigation definitions by committed identity even when bytes are corrupt",
          "[integration][project][migration][authoring-migration]") {
    ProjectMigrationTestFixture project;
    const auto sourcePath = project.Root() / "assets/navigation.HOROASSET";
    WriteText(sourcePath.string() + ".horo",
              R"({"schemaVersion":1,"assetId":"10213243-5465-7687-98a9-bacbdcedfe0f","assetType":"core.navmesh.definition"})");
    // Both corrupt fixtures are shorter than the canonical envelope's minimum framing.
    // ValidateDecodeInput rejects them as envelope_invalid before checksum or header parsing.
    std::string_view expectedCode = "navigation.source.envelope_invalid";
    SECTION("corrupt magic") {
        WriteText(sourcePath, "BAD!corrupted-navigation");
    }
    SECTION("truncated magic") {
        WriteText(sourcePath, "HN");
    }
    SECTION("valid envelope missing required typed definition") {
        expectedCode = "project.migration.stage_failed";
        const auto records = Navigation::NavigationSourceRecords::Create(Navigation::CurrentNavigationSourceSchemaVersion, {},
                                                                         Navigation::NavigationSourceLoadContext{});
        REQUIRE(records.HasValue());
        const auto encoded = Navigation::SerializeNavigationSourceRecords(records.Value());
        REQUIRE(encoded.HasValue());
        WriteText(sourcePath, {reinterpret_cast<const char *>(encoded.Value().data()), encoded.Value().size()});
    }
    const auto original = ReadBytes(sourcePath);
    RequireProjectOpenFailureWithoutMutation(project, expectedCode);
    REQUIRE(ReadBytes(sourcePath) == original);
}

TEST_CASE("0.2 migration leaves non Navigation generic asset payloads untouched",
          "[integration][project][migration][authoring-migration]") {
    ProjectMigrationTestFixture project;
    const auto sourcePath = project.Root() / "assets/custom.horoasset";
    WriteText(sourcePath, "plugin-owned bytes, not a Navigation envelope");
    WriteText(sourcePath.string() + ".horo",
              R"({"schemaVersion":1,"assetId":"10213243-5465-7687-98a9-bacbdcedfe0f","assetType":"plugin.custom"})");
    const auto original = ReadBytes(sourcePath);
    BackendProjectOpen backend;
    REQUIRE(OpenProject(backend, project).outcome == ProjectOpenOutcome::ReadyToActivate);
    REQUIRE(ReadBytes(sourcePath) == original);
}

TEST_CASE("0.2 production migration recovery survives host restart without rewriting the journal contract",
          "[integration][project][migration][authoring-migration][recovery]") {
    ProjectMigrationTestFixture project;
    const auto originalRoot = project.ReadProjectBytes();
    const auto prefabPath = project.Root() / "assets/prefabs/player.prefab";
    const auto scenePath = project.Root() / "assets/scenes/main.horo";
    const auto originalPrefab = ReadBytes(prefabPath);
    const auto originalScene = ReadBytes(scenePath);
    const auto sidecarPath = project.Root() / "assets/prefabs/player.prefab.horo";
    const auto originalSidecar = ReadBytes(sidecarPath);
    // Derived registry/cook generations belong to their hosts and must not enter authored migration publication.
    const auto registryPath = project.Root() / ".horo/cache/registry.snapshot";
    std::filesystem::create_directories(registryPath.parent_path());
    WriteText(registryPath, "prior-valid-registry-generation");
    const auto registry = ReadBytes(registryPath);
    MigrationFailure::FailingFilesystem files(project.Root());
    const auto operation = PreparePendingMigration(project, files, originalRoot);
    const auto recoverAfterRestart = [&](const MigrationRecoveryAction expectedAction) {
        files.failRoot = false;
        BackendProjectOpen restarted(&files);
        REQUIRE(restarted.transactions.InspectPendingRecovery(project.Root()).action == expectedAction);
        REQUIRE(restarted.transactions.Recover(project.Root()).HasValue());
        REQUIRE(restarted.transactions.InspectPendingRecovery(project.Root()).action == MigrationRecoveryAction::None);
    };
    SECTION("resume verified target after restart") {
        recoverAfterRestart(MigrationRecoveryAction::ResumePublish);
        REQUIRE(ReadBytes(sidecarPath) == originalSidecar);
        REQUIRE(ReadBytes(registryPath) == registry);
        REQUIRE(project.ReadProjectJson()["horoVersion"] == "0.2.0");
        REQUIRE(project.ReadProjectJson()["migrationHistoryHead"] == ComputeTestSha256(project.ReadHistoryBytes()));
        REQUIRE(project.ReadHistoryJson()["receipts"].front()["definitions"].back()["id"] == AuthoringDefinitionId);
    }
    SECTION("restore verified originals when forward root evidence is lost") {
        std::error_code error;
        REQUIRE(std::filesystem::remove(project.Root() / ".horo/local/migration" / operation / "staging/.horo/project.json", error));
        REQUIRE_FALSE(error);
        recoverAfterRestart(MigrationRecoveryAction::RestoreOriginals);
        REQUIRE(project.ReadProjectBytes() == originalRoot);
        REQUIRE(ReadBytes(prefabPath) == originalPrefab);
        REQUIRE(ReadBytes(scenePath) == originalScene);
        REQUIRE(ReadBytes(sidecarPath) == originalSidecar);
        REQUIRE(ReadBytes(registryPath) == registry);
        REQUIRE_FALSE(std::filesystem::exists(project.Root() / ".horo/migration_history.json"));
    }
}

TEST_CASE("0.2 production transaction rejects tampered or missing authoritative history after restart",
          "[integration][project][migration][authoring-migration][recovery]") {
    ProjectMigrationTestFixture project;
    auto metadata = project.ReadProjectJson();
    metadata["migrationHistoryHead"] = ComputeTestSha256("original audited history bytes");
    WriteText(project.Root() / ".horo/project.json", metadata.dump(2) + "\n");
    SECTION("missing authoritative history") {}
    SECTION("tampered authoritative history") {
        WriteText(project.Root() / ".horo/migration_history.json", R"({"receipts":[]})");
    }
    const auto originalRoot = project.ReadProjectBytes();
    const auto historyPath = project.Root() / ".horo/migration_history.json";
    const auto originalHistory = std::filesystem::exists(historyPath) ? ReadBytes(historyPath) : std::string{};
    for (int restart = 0; restart < 2; ++restart) {
        BackendProjectOpen backend;
        REQUIRE(OpenProject(backend, project).outcome != ProjectOpenOutcome::ReadyToActivate);
        REQUIRE(project.ReadProjectBytes() == originalRoot);
        if (!originalHistory.empty())
            REQUIRE(ReadBytes(historyPath) == originalHistory);
        else
            REQUIRE_FALSE(std::filesystem::exists(historyPath));
    }
}
