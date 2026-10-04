#include "Horo/Application/ProjectCompatibility.h"
#include "Horo/Editor/ProjectOpenService.h"
#include "Horo/Foundation/JobSystem.h"
#include "editor/project_model/RendererAvailability.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string_view>
#include <thread>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Editor;

    struct TempProject {
        std::filesystem::path root =
            std::filesystem::temp_directory_path() /
            ("horo-project-open-service-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        TempProject() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
            std::filesystem::create_directories(root / ".horo");
            std::filesystem::create_directories(root / "assets/scenes");
            const auto *decision = BuiltInReleaseCompatibilityRegistry().Find(CurrentEngineReleaseVersion());
            std::ofstream out(root / ".horo/project.json");
            out << "{\"horoVersion\":\"" << FormatHoroVersion(decision->release.value) << "\",\"persistentContract\":\""
                << FormatPersistentContractHash(decision->persistentContract)
                << "\",\"projectId\":\"test-project\",\"name\":\"Open Test\","
                   "\"projectVersion\":\"0.1.0\",\"createdAt\":\"2026-07-19T00:00:00Z\","
                   "\"settings\":{\"renderBackend\":\"opengl\","
                   "\"defaultScene\":\"assets/scenes/main.horo\"}}\n";
            std::ofstream(root / "assets/scenes/main.horo") << R"({"schemaVersion":1,"objects":[]})";
        }

        ~TempProject() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    };

    struct LegacyProject {
        std::filesystem::path root =
            std::filesystem::temp_directory_path() /
            ("horo-project-open-migration-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        LegacyProject() {
            const std::filesystem::path fixture =
                std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "tests/fixtures/projects/horo_0_0_1_compression";
            std::filesystem::copy(fixture, root, std::filesystem::copy_options::recursive);
            nlohmann::json metadata;
            {
                std::ifstream input(root / ".horo/project.json");
                input >> metadata;
            }
            metadata["settings"]["defaultScene"] = "assets/scenes/main.horo";
            std::ofstream(root / ".horo/project.json", std::ios::trunc) << metadata.dump(2) << '\n';
            std::filesystem::create_directories(root / "assets/scenes");
            std::ofstream(root / "assets/scenes/main.horo") << R"({"schemaVersion":1,"objects":[]})";
        }

        ~LegacyProject() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    };

    [[nodiscard]] nlohmann::json ReadJson(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return nlohmann::json::parse(input);
    }

    [[nodiscard]] std::string ReadText(const std::filesystem::path &path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }

    struct SlowPreparedState final : IPreparedProjectOpenDerivedState {
        explicit SlowPreparedState(std::thread::id &installThread) : installThread_(installThread) {}

        Result<std::string> Install() override {
            installThread_ = std::this_thread::get_id();
            return Result<std::string>::Success("7");
        }

        std::thread::id &installThread_;
    };

    struct SlowContributor final : IProjectOpenDerivedStateContributor {
        std::string_view Id() const noexcept override {
            return "test.slow";
        }

        Result<std::unique_ptr<IPreparedProjectOpenDerivedState>> Prepare(const std::filesystem::path &,
                                                                          const CancellationToken &) override {
            prepareThread = std::this_thread::get_id();
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
            return Result<std::unique_ptr<IPreparedProjectOpenDerivedState>>::Success(std::make_unique<SlowPreparedState>(installThread));
        }

        std::thread::id prepareThread;
        std::thread::id installThread;
    };

    ProjectOpenProgressSnapshot PumpToTerminal(ProjectOpenService &service, ProjectOpenOperationId id) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        float previousProgress = 0.0F;
        while (std::chrono::steady_clock::now() < deadline) {
            service.PumpOwnerThread();
            auto snapshot = service.Query(id);
            REQUIRE((snapshot.has_value()));
            REQUIRE((snapshot->progress >= previousProgress));
            previousProgress = snapshot->progress;
            if (snapshot->outcome != ProjectOpenOutcome::Running)
                return *snapshot;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE((false && "project open did not terminate"));
        return {};
    }
}  // namespace

TEST_CASE("Project Open Service Tests", "[unit][editor]") {
    TempProject project;
    NativeDurableFileSystem files;
    SystemWallClock clock;
    JobSystem jobs{{.workerCount = 3, .maxQueuedJobs = 32}};
    ProjectMutationCoordinator mutations{files};
    ProjectMigrationTransactionService transactions{files, clock, mutations, jobs};
    ProjectOpenPreflightService preflight{transactions};
    RendererAvailabilitySnapshot renderers{{{"opengl", "OpenGL", RendererAvailabilityState::Active, {}}}, "opengl"};
    ProjectOpenService service{jobs, files, preflight, mutations, transactions, renderers};

    auto started = service.Start({.projectRoot = project.root, .expectedProjectName = "Open Test", .engineBuildIdentity = "test"});
    REQUIRE((started.HasValue()));
    auto snapshot = PumpToTerminal(service, started.Value().Id());
    REQUIRE((snapshot.outcome == ProjectOpenOutcome::ReadyToActivate));
    REQUIRE((snapshot.progress == 1.0F));
    REQUIRE((snapshot.readySession.has_value()));
    const auto readySession = snapshot.readySession;
    REQUIRE((service.RequestCancel(started.Value().Id()).HasValue()));
    const auto afterTerminalCancel = service.Query(started.Value().Id());
    REQUIRE((afterTerminalCancel.has_value()));
    REQUIRE((afterTerminalCancel->outcome == ProjectOpenOutcome::ReadyToActivate));
    REQUIRE((afterTerminalCancel->readySession == readySession));

    {
        auto reservation = service.ReserveSession(*snapshot.readySession);
        REQUIRE((reservation.HasValue()));
        REQUIRE((reservation.Value().Candidate().projectRoot == project.root));
    }
    auto reservation = service.ReserveSession(*snapshot.readySession);
    REQUIRE((reservation.HasValue()));
    auto activation = std::move(reservation).Value();
    REQUIRE((activation.Commit().HasValue()));
    REQUIRE((service.ReserveSession(*snapshot.readySession).HasError()));

    auto retry = service.Start({.projectRoot = project.root, .expectedProjectName = "Open Test", .engineBuildIdentity = "test"});
    REQUIRE((retry.HasValue()));
    REQUIRE((service.RequestCancel(retry.Value().Id()).HasValue()));
    snapshot = PumpToTerminal(service, retry.Value().Id());
    REQUIRE((snapshot.outcome == ProjectOpenOutcome::Cancelled));
    service.Shutdown();
    service.Shutdown();

    SlowContributor slow;
    std::array<IProjectOpenDerivedStateContributor *, 1> contributors{&slow};
    ProjectOpenService asynchronous{jobs, files, preflight, mutations, transactions, renderers, contributors};
    auto asyncStarted =
        asynchronous.Start({.projectRoot = project.root, .expectedProjectName = "Open Test", .engineBuildIdentity = "test"});
    REQUIRE((asyncStarted.HasValue()));
    const auto pumpStart = std::chrono::steady_clock::now();
    asynchronous.PumpOwnerThread();
    const auto pumpDuration = std::chrono::steady_clock::now() - pumpStart;
    REQUIRE((pumpDuration < std::chrono::milliseconds(20)));
    const auto asyncSnapshot = PumpToTerminal(asynchronous, asyncStarted.Value().Id());
    REQUIRE((asyncSnapshot.outcome == ProjectOpenOutcome::ReadyToActivate));
    REQUIRE((slow.prepareThread != std::this_thread::get_id()));
    REQUIRE((slow.installThread == std::this_thread::get_id()));
    {
        auto asyncReservation = asynchronous.ReserveSession(*asyncSnapshot.readySession);
        REQUIRE((asyncReservation.HasValue()));
        REQUIRE((asyncReservation.Value().Candidate().derivedStateRevisions == std::vector<std::string>{"test.slow@7"}));
    }
    {
        std::ofstream changed(project.root / ".horo/project.json", std::ios::app);
        changed << ' ';
    }
    REQUIRE((asynchronous.ReserveSession(*asyncSnapshot.readySession).HasError()));
    asynchronous.Shutdown();

    LegacyProject legacy;
    SlowContributor legacyDerived;
    std::array<IProjectOpenDerivedStateContributor *, 1> legacyContributors{&legacyDerived};
    ProjectOpenService migrating{jobs, files, preflight, mutations, transactions, renderers, legacyContributors};
    auto migrationStarted =
        migrating.Start({.projectRoot = legacy.root, .expectedProjectName = "Legacy Migration Test", .engineBuildIdentity = "test"});
    REQUIRE((migrationStarted.HasValue()));
    auto migrationSnapshot = PumpToTerminal(migrating, migrationStarted.Value().Id());
    if (migrationSnapshot.outcome != ProjectOpenOutcome::ReadyToActivate)
        std::fprintf(stderr, "legacy project open failed in phase %u: %s\n", static_cast<unsigned>(migrationSnapshot.phase),
                     migrationSnapshot.diagnostic.has_value() ? migrationSnapshot.diagnostic->message.c_str() : "missing diagnostic");
    REQUIRE((migrationSnapshot.outcome == ProjectOpenOutcome::ReadyToActivate));
    REQUIRE((migrationSnapshot.readySession.has_value()));
    auto migratedRoot = ReadJson(legacy.root / ".horo/project.json");
    const auto current = CurrentEngineReleaseVersion();
    const auto *decision = BuiltInReleaseCompatibilityRegistry().Find(current);
    REQUIRE(decision != nullptr);
    REQUIRE((migratedRoot.at("horoVersion") == FormatHoroVersion(current.value)));
    REQUIRE((migratedRoot.at("persistentContract") == FormatPersistentContractHash(decision->persistentContract)));
    REQUIRE((migratedRoot.at("settings").at("assetCompression") == "lz4"));
    REQUIRE((migratedRoot.at("settings").at("textureCompression") == "bc7"));
    REQUIRE((migratedRoot.at("settings").at("unknownNested").at("label") == "preserved"));
    REQUIRE((migratedRoot.at("unknownRoot").at("enabled") == true));
    const auto history = ReadJson(legacy.root / ".horo/migration_history.json");
    REQUIRE((history.is_object()));
    REQUIRE((history.at("receipts").size() == 1));
    REQUIRE((history.at("receipts").front().at("definitions").front().at("id") == "core.project_settings.compression_defaults"));
    const auto activeMigrationRoot = legacy.root / ".horo/local/migration";
    REQUIRE((!std::filesystem::exists(activeMigrationRoot) || std::filesystem::is_empty(activeMigrationRoot)));
    auto migratedReservation = migrating.ReserveSession(*migrationSnapshot.readySession);
    REQUIRE((migratedReservation.HasValue()));
    REQUIRE((migratedReservation.Value().Candidate().derivedStateRevisions == std::vector<std::string>{"test.slow@7"}));
    auto migratedActivation = std::move(migratedReservation).Value();
    REQUIRE((migratedActivation.Commit().HasValue()));
    REQUIRE((migrating.ReserveSession(*migrationSnapshot.readySession).HasError()));

    auto secondOpen =
        migrating.Start({.projectRoot = legacy.root, .expectedProjectName = "Legacy Migration Test", .engineBuildIdentity = "test"});
    REQUIRE((secondOpen.HasValue()));
    migrationSnapshot = PumpToTerminal(migrating, secondOpen.Value().Id());
    REQUIRE((migrationSnapshot.outcome == ProjectOpenOutcome::ReadyToActivate));
    REQUIRE((ReadJson(legacy.root / ".horo/migration_history.json").at("receipts").size() == 1));
    migrating.Shutdown();

    LegacyProject invalidLegacy;
    auto invalidRoot = ReadJson(invalidLegacy.root / ".horo/project.json");
    invalidRoot["settings"]["assetCompression"] = "brotli";
    std::ofstream(invalidLegacy.root / ".horo/project.json", std::ios::binary | std::ios::trunc) << invalidRoot.dump(2) << '\n';
    const std::string invalidBefore = ReadText(invalidLegacy.root / ".horo/project.json");
    ProjectOpenService invalidOpen{jobs, files, preflight, mutations, transactions, renderers};
    auto invalidStarted = invalidOpen.Start(
        {.projectRoot = invalidLegacy.root, .expectedProjectName = "Legacy Migration Test", .engineBuildIdentity = "test"});
    REQUIRE((invalidStarted.HasValue()));
    const auto invalidSnapshot = PumpToTerminal(invalidOpen, invalidStarted.Value().Id());
    REQUIRE((invalidSnapshot.outcome == ProjectOpenOutcome::Failed));
    REQUIRE((invalidSnapshot.diagnostic.has_value()));
    REQUIRE((ReadText(invalidLegacy.root / ".horo/project.json") == invalidBefore));
    REQUIRE((!std::filesystem::exists(invalidLegacy.root / ".horo/migration_history.json")));
    invalidOpen.Shutdown();
    jobs.Shutdown(ShutdownPolicy::Drain);
}

TEST_CASE("Project open validates the default scene before workspace preparation", "[unit][editor][scene-preflight]") {
    TempProject project;
    NativeDurableFileSystem files;
    SystemWallClock clock;
    JobSystem jobs{{.workerCount = 3, .maxQueuedJobs = 32}};
    ProjectMutationCoordinator mutations{files};
    ProjectMigrationTransactionService transactions{files, clock, mutations, jobs};
    ProjectOpenPreflightService preflight{transactions};
    RendererAvailabilitySnapshot renderers{{{"opengl", "OpenGL", RendererAvailabilityState::Active, {}}}, "opengl"};
    ProjectOpenService service{jobs, files, preflight, mutations, transactions, renderers};

    const auto open = [&]() {
        auto started = service.Start({
            .projectRoot = project.root,
            .expectedProjectName = "Open Test",
            .engineBuildIdentity = "test",
        });
        REQUIRE((started.HasValue()));
        return PumpToTerminal(service, started.Value().Id());
    };
    const auto requireSceneFailure = [](const ProjectOpenProgressSnapshot &snapshot) {
        REQUIRE((snapshot.outcome == ProjectOpenOutcome::Failed));
        REQUIRE((snapshot.phase == ProjectOpenPhase::Failed));
        REQUIRE((snapshot.diagnostic.has_value()));
        REQUIRE((snapshot.diagnostic->code.Value() == "project.open.scene_preflight_failed"));
        REQUIRE((!snapshot.readySession.has_value()));
    };

    std::filesystem::remove(project.root / "assets/scenes/main.horo");
    requireSceneFailure(open());

    nlohmann::json metadata = ReadJson(project.root / ".horo/project.json");
    metadata["settings"]["defaultScene"] = "";
    std::ofstream(project.root / ".horo/project.json", std::ios::trunc) << metadata.dump(2) << '\n';
    const ProjectOpenProgressSnapshot emptyScene = open();
    REQUIRE((emptyScene.outcome == ProjectOpenOutcome::ReadyToActivate));
    REQUIRE((emptyScene.readySession.has_value()));
    auto emptySceneReservation = service.ReserveSession(*emptyScene.readySession);
    REQUIRE((emptySceneReservation.HasValue()));
    REQUIRE((std::move(emptySceneReservation).Value().Commit().HasValue()));

    metadata["settings"]["defaultScene"] = "assets/scenes/main.horo";
    std::ofstream(project.root / ".horo/project.json", std::ios::trunc) << metadata.dump(2) << '\n';

    std::ofstream(project.root / "assets/scenes/main.horo", std::ios::trunc) << "{ malformed";
    requireSceneFailure(open());

    std::ofstream(project.root / "assets/scenes/main.horo", std::ios::trunc) << R"({"schemaVersion":99,"objects":[]})";
    requireSceneFailure(open());

    constexpr std::string_view duplicateObjectScene =
        R"({"schemaVersion":1,"objects":[)"
        R"({"id":1,"parent":null,"name":"A","transform":{"translation":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},"primitiveMesh":null,"components":{}},)"
        R"({"id":1,"parent":null,"name":"B","transform":{"translation":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},"primitiveMesh":null,"components":{}}]})";
    std::ofstream(project.root / "assets/scenes/main.horo", std::ios::trunc) << duplicateObjectScene;
    requireSceneFailure(open());

    metadata = ReadJson(project.root / ".horo/project.json");
    metadata["settings"]["defaultScene"] = "../outside.horo";
    std::ofstream(project.root / ".horo/project.json", std::ios::trunc) << metadata.dump(2) << '\n';
    requireSceneFailure(open());

    metadata["settings"]["defaultScene"] = "assets/scenes/main.horo";
    std::ofstream(project.root / ".horo/project.json", std::ios::trunc) << metadata.dump(2) << '\n';
    std::ofstream(project.root / "assets/scenes/main.horo", std::ios::trunc) << R"({"schemaVersion":1,"objects":[]})";
    const ProjectOpenProgressSnapshot valid = open();
    REQUIRE((valid.outcome == ProjectOpenOutcome::ReadyToActivate));
    REQUIRE((valid.readySession.has_value()));

    service.Shutdown();
    jobs.Shutdown(ShutdownPolicy::Drain);
}
