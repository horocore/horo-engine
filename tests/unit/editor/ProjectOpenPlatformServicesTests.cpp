#include "../../support/project/ProjectOpenFixture.h"

using namespace Horo::Tests::ProjectOpen;

TEST_CASE("Ordinary current project open rejects missing platform authority without migration or repair", "[unit][editor][platform-open]") {
    TempProject project;
    std::string expected;
    SECTION("Required root salt") {
        auto metadata = ReadJson(project.root / ".horo/project.json");
        metadata.erase("platformServicesIdSalt");
        std::ofstream(project.root / ".horo/project.json", std::ios::binary | std::ios::trunc) << metadata.dump();
        expected = PlatformServices::StableIdErrors::InvalidSalt.code.Value();
    }
    SECTION("Required stat document") {
        REQUIRE(std::filesystem::remove(project.root / PlatformServicesDocumentPaths::Stats));
        expected = Assets::AssetSourceErrors::ReadFailed.code.Value();
    }
    const auto original = ReadText(project.root / ".horo/project.json");
    JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 8});
    NativeDurableFileSystem files;
    SystemWallClock clock;
    ProjectMutationCoordinator mutations(files);
    auto random = std::make_shared<Tests::PlatformMigration::Random>();
    random->fail = true;
    ProjectMigrationTransactionService transactions{files, clock, mutations, jobs, {}, {.random = random}};
    ProjectOpenPreflightService preflight(transactions);
    REQUIRE(preflight.Inspect(project.root).compatibility.status == ProjectCompatibilityStatus::Current);
    REQUIRE_FALSE(preflight.Inspect(project.root).migrationPlan.has_value());
    RendererAvailabilitySnapshot renderers({{{"opengl", "OpenGL", RendererAvailabilityState::Active, {}}}, "opengl"});
    ProjectOpenService service(jobs, files, preflight, mutations, transactions, renderers);
    auto operation = service.Start({.projectRoot = project.root});
    REQUIRE(operation.HasValue());
    const auto terminal = PumpToTerminal(service, operation.Value().Id());
    REQUIRE(terminal.outcome == ProjectOpenOutcome::Failed);
    REQUIRE(terminal.diagnostic.has_value());
    CHECK(terminal.diagnostic->code.Value() == expected);
    CHECK_FALSE(terminal.readySession.has_value());
    CHECK(ReadText(project.root / ".horo/project.json") == original);
    CHECK(random->calls == 0);
}

TEST_CASE("Any platform source edit rejects session publication before derived-state installation", "[unit][editor][platform-open]") {
    TempProject project;
    SourceMutationContributor mutation;
    SECTION("Root") {
        mutation.path = ".horo/project.json";
    }
    SECTION("Ledger") {
        mutation.path = PlatformServicesDocumentPaths::Ledger;
    }
    SECTION("Achievement") {
        mutation.path = PlatformServicesDocumentPaths::Achievements;
    }
    SECTION("Stat") {
        mutation.path = PlatformServicesDocumentPaths::Stats;
    }
    SECTION("Leaderboard") {
        mutation.path = PlatformServicesDocumentPaths::Leaderboards;
    }
    SECTION("Presence") {
        mutation.path = PlatformServicesDocumentPaths::Presence;
    }
    JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 8});
    NativeDurableFileSystem files;
    SystemWallClock clock;
    ProjectMutationCoordinator mutations(files);
    ProjectMigrationTransactionService transactions{files, clock, mutations, jobs};
    ProjectOpenPreflightService preflight(transactions);
    RendererAvailabilitySnapshot renderers({{{"opengl", "OpenGL", RendererAvailabilityState::Active, {}}}, "opengl"});
    const std::array<IProjectOpenDerivedStateContributor *, 1> contributors{&mutation};
    ProjectOpenService service(jobs, files, preflight, mutations, transactions, renderers, contributors);
    auto operation = service.Start({.projectRoot = project.root});
    REQUIRE(operation.HasValue());
    const auto terminal = PumpToTerminal(service, operation.Value().Id());
    REQUIRE(terminal.outcome == ProjectOpenOutcome::Failed);
    REQUIRE(terminal.diagnostic.has_value());
    CHECK(terminal.diagnostic->code.Value() == "project.open.session_stale");
    CHECK_FALSE(terminal.readySession.has_value());
    CHECK_FALSE(mutation.installed);
}

TEST_CASE("Platform source edits invalidate both a ready reservation and its final activation commit", "[unit][editor][platform-open]") {
    TempProject project;
    JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 8});
    NativeDurableFileSystem files;
    SystemWallClock clock;
    ProjectMutationCoordinator mutations(files);
    ProjectMigrationTransactionService transactions{files, clock, mutations, jobs};
    ProjectOpenPreflightService preflight(transactions);
    RendererAvailabilitySnapshot renderers({{{"opengl", "OpenGL", RendererAvailabilityState::Active, {}}}, "opengl"});
    ProjectOpenService service(jobs, files, preflight, mutations, transactions, renderers);
    auto operation = service.Start({.projectRoot = project.root});
    REQUIRE(operation.HasValue());
    const auto terminal = PumpToTerminal(service, operation.Value().Id());
    REQUIRE(terminal.outcome == ProjectOpenOutcome::ReadyToActivate);
    REQUIRE(terminal.readySession.has_value());
    SECTION("Before reservation") {
        std::ofstream(project.root / PlatformServicesDocumentPaths::Presence, std::ios::binary | std::ios::app) << ' ';
        CHECK(service.ReserveSession(*terminal.readySession).HasError());
    }
    SECTION("After reservation") {
        auto reservation = service.ReserveSession(*terminal.readySession);
        REQUIRE(reservation.HasValue());
        REQUIRE(reservation.Value().Candidate().platformServices != nullptr);
        std::ofstream(project.root / PlatformServicesDocumentPaths::Stats, std::ios::binary | std::ios::app) << ' ';
        CHECK(reservation.Value().Commit().HasError());
    }
    SECTION("Shutdown revokes activation while retained CPU models stay owned") {
        auto reservation = service.ReserveSession(*terminal.readySession);
        REQUIRE(reservation.HasValue());
        const auto retained = reservation.Value().Candidate().platformServices;
        REQUIRE(retained != nullptr);
        service.Shutdown();
        CHECK(reservation.Value().Commit().HasError());
        CHECK(retained->Stats().Definitions().empty());
        CHECK(retained->Configuration().UsesNullProvider());
    }
}
