#include "../../support/project/PlatformServicesMigrationPlanFixture.h"
#include "../../support/project/ProjectMigrationFailingFilesystem.h"
#include "Horo/Editor/ProjectMigrationTransaction.h"

namespace Horo::Tests::PlatformMigration {
    using namespace Editor;

    TEST_CASE("Interrupted real platform adoption reuses its staged salt during durable recovery", "[platform-adoption]") {
        Project project;
        const auto original = project.Read(".horo/project.json");
        MigrationFailure::FailingFilesystem files(project.root);
        SystemWallClock clock;
        ProjectMutationCoordinator mutations(files);
        JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 8});
        auto random = std::make_shared<Random>();
        ProjectMigrationTransactionService service(files, clock, mutations, jobs, {}, {.random = random});
        const auto *target = BuiltInReleaseCompatibilityRegistry().Find(CurrentEngineReleaseVersion());
        REQUIRE(target != nullptr);
        const auto committed = service.Execute({.projectRoot = project.root,
                                                .sourceMetadata = project.Metadata(),
                                                .sourceBaseline = {ParseHoroVersion("0.3.0").Value()},
                                                .targetDecision = *target,
                                                .plan = PlatformAdoptionPlan(project),
                                                .engineBuildIdentity = "test-platform-adoption"});
        REQUIRE(committed.HasError());
        CHECK(random->calls == 1);
        CHECK(project.Read(".horo/project.json") == original);
        REQUIRE(service.InspectPendingRecovery(project.root).action == MigrationRecoveryAction::ResumePublish);
        const auto ledgerBeforeRecovery = project.Read(PlatformServicesDocumentPaths::Ledger);
        random->fail = true;
        files.failRoot = false;
        REQUIRE(service.Recover(project.root).HasValue());
        CHECK(random->calls == 1);
        CHECK(project.Read(PlatformServicesDocumentPaths::Ledger) == ledgerBeforeRecovery);
        const auto root = DecodeProjectSourceDocument(project.Read(".horo/project.json"));
        REQUIRE(root.HasValue());
        CHECK(FormatHoroVersion(root.Value().metadata.horoVersion.value) == "0.4.0");
        REQUIRE(root.Value().metadata.migrationHistoryHead.has_value());
        const auto salt = ResolveProjectPlatformServicesSalt(root.Value());
        REQUIRE(salt.HasValue());
        REQUIRE(DecodePlatformServicesLedger(ledgerBeforeRecovery, root.Value().metadata.projectId, salt.Value()).HasValue());
        CHECK(project.Read("notes.txt") == "unrelated source bytes\n");
        CHECK(service.InspectPendingRecovery(project.root).action == MigrationRecoveryAction::None);
    }

    TEST_CASE("Lost forward evidence restores the original root and removes newly adopted platform files", "[platform-adoption]") {
        Project project;
        const auto original = project.Read(".horo/project.json");
        MigrationFailure::FailingFilesystem files(project.root);
        SystemWallClock clock;
        ProjectMutationCoordinator mutations(files);
        JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 8});
        auto random = std::make_shared<Random>();
        ProjectMigrationTransactionService service(files, clock, mutations, jobs, {}, {.random = random});
        const auto *target = BuiltInReleaseCompatibilityRegistry().Find(CurrentEngineReleaseVersion());
        REQUIRE(target != nullptr);
        const auto committed = service.Execute({.projectRoot = project.root,
                                                .sourceMetadata = project.Metadata(),
                                                .sourceBaseline = {ParseHoroVersion("0.3.0").Value()},
                                                .targetDecision = *target,
                                                .plan = PlatformAdoptionPlan(project),
                                                .engineBuildIdentity = "test-platform-adoption"});
        REQUIRE(committed.HasError());
        const auto recovery = service.InspectPendingRecovery(project.root);
        REQUIRE(recovery.operationId.has_value());
        const auto stagedRoot = project.root / ".horo/local/migration" / *recovery.operationId / "staging/.horo/project.json";
        REQUIRE(std::filesystem::remove(stagedRoot));
        REQUIRE(service.InspectPendingRecovery(project.root).action == MigrationRecoveryAction::RestoreOriginals);
        files.failRoot = false;
        random->fail = true;
        REQUIRE(service.Recover(project.root).HasValue());
        CHECK(random->calls == 1);
        CHECK(project.Read(".horo/project.json") == original);
        for (const auto path :
             {PlatformServicesDocumentPaths::Ledger, PlatformServicesDocumentPaths::Achievements, PlatformServicesDocumentPaths::Stats,
              PlatformServicesDocumentPaths::Leaderboards, PlatformServicesDocumentPaths::Presence})
            CHECK_FALSE(std::filesystem::exists(project.root / path));
        CHECK_FALSE(std::filesystem::exists(project.root / ".horo/migration_history.json"));
        CHECK(service.InspectPendingRecovery(project.root).action == MigrationRecoveryAction::None);
    }
}  // namespace Horo::Tests::PlatformMigration
