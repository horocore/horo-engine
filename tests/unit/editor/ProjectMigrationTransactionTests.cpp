#include "../../support/OwnedTestDirectory.h"
#include "../../support/project/ProjectMigrationFailingFilesystem.h"
#include "Horo/Editor/ProjectMigrationTransaction.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <functional>
#include <limits>
#include <optional>
#include <span>
#include <string_view>

namespace {
    using namespace Horo;
    using namespace Horo::Application;
    using namespace Horo::Editor;

    using namespace Horo::Tests::MigrationFailure;

    std::vector<std::byte> Bytes(std::string_view text) {
        std::vector<std::byte> result(text.size());
        std::ranges::transform(text, result.begin(), [](char value) {
            return static_cast<std::byte>(value);
        });
        return result;
    }

    ContractBaselineVersion Version(const char *text) {
        auto parsed = ParseHoroVersion(text);
        REQUIRE((parsed.HasValue()));
        return {parsed.Value()};
    }

    PersistentContractHash Contract(std::uint8_t marker) {
        PersistentContractHash hash;
        hash.bytes.fill(marker);
        return hash;
    }

    class Append final : public IProjectMigrationDocumentStage {
    public:
        MigrationStageDescriptor Describe() const override {
            return {.id = {"test.append"}, .writeFamilies = {"other"}};
        }

        Result<MigrationDocumentChange> Execute(const ProjectDocumentView &source, const MigrationStageContext &,
                                                const CancellationToken &) const override {
            auto output = std::vector<std::byte>(source.bytes.begin(), source.bytes.end());
            output.push_back(std::byte{'!'});
            return Result<MigrationDocumentChange>::Success({source.handle, std::move(output)});
        }
    };

    class Valid final : public IProjectMigrationValidator {
    public:
        MigrationStageDescriptor Describe() const override {
            return {.id = {"test.valid"}};
        }

        Result<void> Validate(const ProjectMigrationContext &, const CancellationToken &) const override {
            return Result<void>::Success();
        }
    };

    class FinalCandidateValid final : public IProjectMigrationValidator {
    public:
        MigrationStageDescriptor Describe() const override {
            return {.id = {"test.final_candidate"}};
        }

        Result<void> Validate(const ProjectMigrationContext &context, const CancellationToken &) const override {
            bool sawTargetRoot = false;
            bool sawHistory = false;
            for (const auto &entry : context.ListDocuments({})) {
                const auto document = context.ReadDocument(entry.handle);
                if (document.HasError())
                    return Result<void>::Failure(document.ErrorValue());
                const auto contains = [&document](std::string_view needle) {
                    const auto expected = std::as_bytes(std::span{needle.data(), needle.size()});
                    const auto &bytes = document.Value().bytes;
                    return !std::ranges::search(bytes, expected).empty();
                };
                if (entry.path == ".horo/project.json")
                    sawTargetRoot = contains(R"("horoVersion": "0.1.0")") && contains("migrationHistoryHead");
                if (entry.path == ".horo/migration_history.json")
                    sawHistory = contains("test.migration");
            }
            return sawTargetRoot && sawHistory
                       ? Result<void>::Success()
                       : Result<void>::Failure(MakeError(InjectedFailure, "Final validator did not see transaction documents."));
        }
    };

    struct Project {
        Project() {
            std::filesystem::create_directories(root / ".horo/local");
            std::filesystem::create_directories(root / "assets");
            Write(
                ".horo/project.json",
                R"({"horoVersion":"0.0.1","persistentContract":"sha256:0101010101010101010101010101010101010101010101010101010101010101","projectId":"p1","name":"Test","projectVersion":"0.1.0","createdAt":"2026-07-18T00:00:00Z","settings":{"renderBackend":"opengl","unknown":7}})");
            Write("assets/data.txt", "value");
        }

        void Write(const std::filesystem::path &path, std::string_view text) const {
            std::filesystem::create_directories((root / path).parent_path());
            std::ofstream(root / path, std::ios::binary) << text;
        }

        std::string Read(const std::filesystem::path &path) const {
            std::ifstream stream(root / path, std::ios::binary);
            return {std::istreambuf_iterator<char>(stream), {}};
        }

        Horo::Tests::OwnedTestDirectory directory{"horo-transaction-test"};
        const std::filesystem::path root{directory.Path()};
    };

    ProjectMigrationPlan Plan() {
        auto builder = ProjectMigrationPipelineBuilder::Begin({"test.migration"});
        static_cast<void>(builder.AddForEach(MigrationDocumentQuery::Kind(MigrationDocumentKind::Other), std::make_shared<Append>()));
        static_cast<void>(builder.AddValidator(std::make_shared<Valid>()));
        auto pipeline = std::move(builder).Build();
        REQUIRE((pipeline.HasValue()));
        ProjectMigrationDefinition definition{.id = {"test.migration"},
                                              .from = Version("0.0.1"),
                                              .to = Version("0.1.0"),
                                              .sourceContract = Contract(1),
                                              .targetContract = Contract(2),
                                              .pipeline = pipeline.Value()};
        definition.storageEstimate = {.maximumOutputRatioPermille = 1000, .maximumAddedBytesPerDocument = 1};
        return {.source = Version("0.0.1"),
                .target = Version("0.1.0"),
                .definitions = {definition},
                .targetValidator = std::make_shared<FinalCandidateValid>()};
    }

    [[nodiscard]] ProjectMetadata SourceMetadata() {
        return {.horoVersion = {ParseHoroVersion("0.0.1").Value()},
                .persistentContract = Contract(1),
                .projectId = "p1",
                .name = "Test",
                .projectVersion = "0.1.0",
                .createdAt = "2026-07-18T00:00:00Z",
                .renderBackend = "opengl"};
    }

    [[nodiscard]] ReleaseCompatibilityDecision TargetDecision() {
        return {.release = {ParseHoroVersion("0.1.0").Value()}, .contractBaseline = Version("0.1.0"), .persistentContract = Contract(2)};
    }

    TEST_CASE("Transaction Publishes History And Preserves Unknown Metadata", "[unit][editor]") {
        Project project;
        NativeDurableFileSystem files;
        SystemWallClock clock;
        ProjectMutationCoordinator coordinator(files);
        JobSystem jobs({.workerCount = 2, .maxQueuedJobs = 8});
        ProjectMigrationTransactionService service(files, clock, coordinator, jobs);
        ProjectMetadata metadata{.horoVersion = {ParseHoroVersion("0.0.1").Value()},
                                 .persistentContract = Contract(1),
                                 .projectId = "p1",
                                 .name = "Test",
                                 .projectVersion = "0.1.0",
                                 .createdAt = "2026-07-18T00:00:00Z",
                                 .renderBackend = "opengl"};
        ReleaseCompatibilityDecision target{.release = {ParseHoroVersion("0.1.0").Value()},
                                            .contractBaseline = Version("0.1.0"),
                                            .persistentContract = Contract(2)};
        auto result = service.Execute({.projectRoot = project.root,
                                       .sourceMetadata = metadata,
                                       .sourceBaseline = Version("0.0.1"),
                                       .targetDecision = target,
                                       .plan = Plan(),
                                       .engineBuildIdentity = "test-build"});
        REQUIRE((result.HasValue()));
        REQUIRE((project.Read("assets/data.txt") == "value!"));
        const auto root = project.Read(".horo/project.json");
        REQUIRE((root.find(R"("horoVersion": "0.1.0")") != std::string::npos));
        REQUIRE((root.find(R"("unknown": 7)") != std::string::npos));
        REQUIRE((root.find("migrationHistoryHead") != std::string::npos));
        REQUIRE((project.Read(".horo/migration_history.json").find(result.Value().operationId) != std::string::npos));
        REQUIRE((project.Read(".horo/migration_history.json").find("\n  ") == std::string::npos));
        REQUIRE((service.InspectPendingRecovery(project.root).action == MigrationRecoveryAction::None));
        jobs.Shutdown(ShutdownPolicy::Drain);
    }

    TEST_CASE("Recovery Resumes After History Before Root Failure", "[unit][editor]") {
        Project project;
        FailingFilesystem files(project.root);
        SystemWallClock clock;
        ProjectMutationCoordinator coordinator(files);
        JobSystem jobs({.workerCount = 2, .maxQueuedJobs = 8});
        ProjectMigrationTransactionService service(files, clock, coordinator, jobs);
        ProjectMetadata metadata{.horoVersion = {ParseHoroVersion("0.0.1").Value()},
                                 .persistentContract = Contract(1),
                                 .projectId = "p1",
                                 .name = "Test",
                                 .projectVersion = "0.1.0",
                                 .createdAt = "2026-07-18T00:00:00Z",
                                 .renderBackend = "opengl"};
        ReleaseCompatibilityDecision target{.release = {ParseHoroVersion("0.1.0").Value()},
                                            .contractBaseline = Version("0.1.0"),
                                            .persistentContract = Contract(2)};
        auto failed = service.Execute({.projectRoot = project.root,
                                       .sourceMetadata = metadata,
                                       .sourceBaseline = Version("0.0.1"),
                                       .targetDecision = target,
                                       .plan = Plan(),
                                       .engineBuildIdentity = "test-build"});
        REQUIRE((failed.HasError()));
        REQUIRE((project.Read(".horo/project.json").find(R"("horoVersion":"0.0.1")") != std::string::npos));
        REQUIRE((project.Read(".horo/migration_history.json").find("test.migration") != std::string::npos));
        const auto recovery = service.InspectPendingRecovery(project.root);
        REQUIRE((recovery.action == MigrationRecoveryAction::ResumePublish));
        files.failRoot = false;
        REQUIRE((service.Recover(project.root).HasValue()));
        REQUIRE((project.Read(".horo/project.json").find(R"("horoVersion": "0.1.0")") != std::string::npos));
        REQUIRE((service.InspectPendingRecovery(project.root).action == MigrationRecoveryAction::None));
        jobs.Shutdown(ShutdownPolicy::Drain);
    }

    TEST_CASE("Recovery Restores Verified Originals When Forward Evidence Is Lost", "[unit][editor]") {
        Project project;
        FailingFilesystem files(project.root);
        SystemWallClock clock;
        ProjectMutationCoordinator coordinator(files);
        JobSystem jobs({.workerCount = 2, .maxQueuedJobs = 8});
        ProjectMigrationTransactionService service(files, clock, coordinator, jobs);
        ProjectMetadata metadata{.horoVersion = {ParseHoroVersion("0.0.1").Value()},
                                 .persistentContract = Contract(1),
                                 .projectId = "p1",
                                 .name = "Test",
                                 .projectVersion = "0.1.0",
                                 .createdAt = "2026-07-18T00:00:00Z",
                                 .renderBackend = "opengl"};
        ReleaseCompatibilityDecision target{.release = {ParseHoroVersion("0.1.0").Value()},
                                            .contractBaseline = Version("0.1.0"),
                                            .persistentContract = Contract(2)};
        auto failed = service.Execute({.projectRoot = project.root,
                                       .sourceMetadata = metadata,
                                       .sourceBaseline = Version("0.0.1"),
                                       .targetDecision = target,
                                       .plan = Plan(),
                                       .engineBuildIdentity = "test-build"});
        REQUIRE((failed.HasError()));
        const auto recovery = service.InspectPendingRecovery(project.root);
        REQUIRE((recovery.operationId.has_value()));
        const auto operationRoot = project.root / ".horo/local/migration" / recovery.operationId.value();
        std::error_code ignored;
        std::filesystem::remove(operationRoot / "staging/.horo/project.json", ignored);
        REQUIRE((service.InspectPendingRecovery(project.root).action == MigrationRecoveryAction::RestoreOriginals));
        files.failRoot = false;
        REQUIRE((service.Recover(project.root).HasValue()));
        REQUIRE((project.Read("assets/data.txt") == "value"));
        REQUIRE((project.Read(".horo/project.json").find(R"("horoVersion":"0.0.1")") != std::string::npos));
        REQUIRE((!std::filesystem::exists(project.root / ".horo/migration_history.json")));
        REQUIRE((service.InspectPendingRecovery(project.root).action == MigrationRecoveryAction::None));
        jobs.Shutdown(ShutdownPolicy::Drain);
    }

    TEST_CASE("Execute Revalidates Pending Recovery After Lease Acquisition", "[unit][editor]") {
        Project project;
        FailingFilesystem files(project.root);
        files.failRoot = false;
        SystemWallClock clock;
        ProjectMutationCoordinator coordinator(files);
        JobSystem jobs({.workerCount = 2, .maxQueuedJobs = 8});
        ProjectMigrationTransactionService service(files, clock, coordinator, jobs);
        const std::string operationId(32, 'd');
        const std::string contract = FormatMigrationRecoveryContractId(CurrentMigrationRecoveryContractId());
        files.onExclusiveAcquired = [&project, &operationId, &contract] {
            project.Write(".horo/local/migration/" + operationId + "/journal.json",
                          R"({"writerHoroVersion":")" + FormatHoroVersion(CurrentEngineReleaseVersion().value) +
                              R"(","recoveryContract":")" + contract + R"(","operationId":")" + operationId +
                              R"(","state":"Committed","sourceVersion":"0.0.1",)"
                              R"("targetVersion":"0.1.0","projectId":"p1",)"
                              R"("engineBuildIdentity":"test-build","records":[]})");
        };

        const auto result = service.Execute({.projectRoot = project.root,
                                             .sourceMetadata = SourceMetadata(),
                                             .sourceBaseline = Version("0.0.1"),
                                             .targetDecision = TargetDecision(),
                                             .plan = Plan(),
                                             .engineBuildIdentity = "test-build"});

        REQUIRE((result.HasError()));
        REQUIRE((result.ErrorValue().code.Value() == "project.migration.recovery_failed"));
        REQUIRE((service.InspectPendingRecovery(project.root).action == MigrationRecoveryAction::FinalizeCommittedMigration));
        jobs.Shutdown(ShutdownPolicy::Drain);
    }

    TEST_CASE("Recover Revalidates Cleared Evidence After Lease Acquisition", "[unit][editor]") {
        Project project;
        const std::string operationId(32, 'e');
        const std::string contract = FormatMigrationRecoveryContractId(CurrentMigrationRecoveryContractId());
        const auto operationRoot = project.root / ".horo/local/migration" / operationId;
        project.Write(".horo/local/migration/" + operationId + "/journal.json",
                      R"({"writerHoroVersion":")" + FormatHoroVersion(CurrentEngineReleaseVersion().value) + R"(","recoveryContract":")" +
                          contract + R"(","operationId":")" + operationId +
                          R"(","state":"Committed","sourceVersion":"0.0.1",)"
                          R"("targetVersion":"0.1.0","projectId":"p1",)"
                          R"("engineBuildIdentity":"test-build","records":[]})");
        FailingFilesystem files(project.root);
        files.failRoot = false;
        SystemWallClock clock;
        ProjectMutationCoordinator coordinator(files);
        JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 1});
        ProjectMigrationTransactionService service(files, clock, coordinator, jobs);
        files.onExclusiveAcquired = [&operationRoot] {
            std::error_code ignored;
            std::filesystem::remove_all(operationRoot, ignored);
            REQUIRE_FALSE((ignored));
        };

        REQUIRE((service.Recover(project.root).HasValue()));
        REQUIRE((service.InspectPendingRecovery(project.root).action == MigrationRecoveryAction::None));
        jobs.Shutdown(ShutdownPolicy::Drain);
    }

    TEST_CASE("Transaction Holds Mutation Lease Through Storage Admission", "[unit][editor]") {
        Project project;
        FailingFilesystem files(project.root);
        files.failRoot = false;
        SystemWallClock clock;
        ProjectMutationCoordinator coordinator(files);
        JobSystem jobs({.workerCount = 2, .maxQueuedJobs = 8});
        ProjectMigrationTransactionService service(files, clock, coordinator, jobs);
        bool competingWriterRejected = false;
        files.onAvailableBytes = [&coordinator, &project, &competingWriterRejected] {
            const auto competing = coordinator.TryAcquire({project.root, ProjectMutationOwner::Save, "competing-save"});
            competingWriterRejected = competing.HasError();
            REQUIRE((competing.HasError()));
            REQUIRE((competing.ErrorValue().code.Value() == "project.migration.locked"));
        };

        const auto result = service.Execute({.projectRoot = project.root,
                                             .sourceMetadata = SourceMetadata(),
                                             .sourceBaseline = Version("0.0.1"),
                                             .targetDecision = TargetDecision(),
                                             .plan = Plan(),
                                             .engineBuildIdentity = "test-build"});

        REQUIRE((result.HasValue()));
        REQUIRE((competingWriterRejected));
        files.onAvailableBytes = {};
        const auto afterTransaction = coordinator.TryAcquire({project.root, ProjectMutationOwner::Save, "save-after-migration"});
        REQUIRE((afterTransaction.HasValue()));
        jobs.Shutdown(ShutdownPolicy::Drain);
    }

    TEST_CASE("Storage Admission Rejects Insufficient Capacity Before Staging", "[unit][editor]") {
        Project project;
        const std::string rootBefore = project.Read(".horo/project.json");
        const std::string dataBefore = project.Read("assets/data.txt");
        FailingFilesystem files(project.root);
        files.failRoot = false;
        files.availableBytes = 0;
        SystemWallClock clock;
        ProjectMutationCoordinator coordinator(files);
        JobSystem jobs({.workerCount = 2, .maxQueuedJobs = 8});
        ProjectMigrationTransactionService service(files, clock, coordinator, jobs);

        const auto result = service.Execute({.projectRoot = project.root,
                                             .sourceMetadata = SourceMetadata(),
                                             .sourceBaseline = Version("0.0.1"),
                                             .targetDecision = TargetDecision(),
                                             .plan = Plan(),
                                             .engineBuildIdentity = "test-build"});

        REQUIRE((result.HasError()));
        REQUIRE((result.ErrorValue().code.Value() == "project.migration.capacity_insufficient"));
        REQUIRE((project.Read(".horo/project.json") == rootBefore));
        REQUIRE((project.Read("assets/data.txt") == dataBefore));
        REQUIRE((!std::filesystem::exists(project.root / ".horo/migration_history.json")));
        REQUIRE((service.InspectPendingRecovery(project.root).action == MigrationRecoveryAction::None));
        jobs.Shutdown(ShutdownPolicy::Drain);
    }

    TEST_CASE("Storage Admission Handles Zero Margin And Saturates Declared Bounds", "[unit][editor]") {
        Project project;
        NativeDurableFileSystem files;
        SystemWallClock clock;
        ProjectMutationCoordinator coordinator(files);
        JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 4});
        const ProjectMigrationStoragePolicy policy{.minimumSafetyMarginBytes = 0, .safetyMarginPermille = 0};
        ProjectMigrationTransactionService service(files, clock, coordinator, jobs, policy);
        ProjectMigrationTransactionRequest request{.projectRoot = project.root,
                                                   .sourceMetadata = SourceMetadata(),
                                                   .sourceBaseline = Version("0.0.1"),
                                                   .targetDecision = TargetDecision(),
                                                   .plan = Plan(),
                                                   .engineBuildIdentity = "test-build"};

        const auto zeroMargin = service.InspectStorageAdmission(request);
        REQUIRE((zeroMargin.HasValue()));
        REQUIRE((zeroMargin.Value().safetyMarginBytes == 0));

        request.limits.maxJournalBytes = std::numeric_limits<std::uint64_t>::max();
        request.limits.maxHistoryBytes = 1;
        const auto saturated = service.InspectStorageAdmission(request);
        REQUIRE((saturated.HasValue()));
        REQUIRE((saturated.Value().journalAndHistoryBytes == std::numeric_limits<std::uint64_t>::max()));
        REQUIRE((saturated.Value().RequiredBytes() == std::numeric_limits<std::uint64_t>::max()));

        request.limits.maxJournalBytes = ProjectMigrationLimits{}.maxJournalBytes;
        request.limits.maxHistoryBytes = ProjectMigrationLimits{}.maxHistoryBytes;
        request.limits.maxInputBytes = 1;
        const auto oversizedFile = service.InspectStorageAdmission(request);
        REQUIRE((oversizedFile.HasError()));
        REQUIRE((oversizedFile.ErrorValue().code.Value() == "project.migration.inventory_limit"));
        jobs.Shutdown(ShutdownPolicy::Drain);
    }

    TEST_CASE("Cancellation Before Publish Preserves Project And History", "[unit][editor]") {
        Project project;
        const std::string rootBefore = project.Read(".horo/project.json");
        const std::string dataBefore = project.Read("assets/data.txt");
        NativeDurableFileSystem files;
        SystemWallClock clock;
        ProjectMutationCoordinator coordinator(files);
        JobSystem jobs({.workerCount = 2, .maxQueuedJobs = 8});
        ProjectMigrationTransactionService service(files, clock, coordinator, jobs);
        CancellationSource cancellation;
        cancellation.RequestCancellation();

        const auto result = service.Execute({.projectRoot = project.root,
                                             .sourceMetadata = SourceMetadata(),
                                             .sourceBaseline = Version("0.0.1"),
                                             .targetDecision = TargetDecision(),
                                             .plan = Plan(),
                                             .engineBuildIdentity = "test-build",
                                             .cancellation = cancellation.Token()});

        REQUIRE((result.HasError()));
        REQUIRE((result.ErrorValue().code.Value() == "project.migration.cancelled"));
        REQUIRE((project.Read(".horo/project.json") == rootBefore));
        REQUIRE((project.Read("assets/data.txt") == dataBefore));
        REQUIRE((!std::filesystem::exists(project.root / ".horo/migration_history.json")));
        REQUIRE((service.InspectPendingRecovery(project.root).action == MigrationRecoveryAction::None));
        jobs.Shutdown(ShutdownPolicy::Drain);
    }

    TEST_CASE("Committed Journal Finalizes Cleanup Without Rewriting Project", "[unit][editor]") {
        Project project;
        const std::string operationId(32, 'c');
        const std::filesystem::path operationDirectory{operationId};
        const std::string rootBefore = project.Read(".horo/project.json");
        const std::string contract = FormatMigrationRecoveryContractId(CurrentMigrationRecoveryContractId());
        project.Write(std::filesystem::path{".horo/local/migration"} / operationDirectory / "journal.json",
                      R"({"writerHoroVersion":")" + FormatHoroVersion(CurrentEngineReleaseVersion().value) + R"(","recoveryContract":")" +
                          contract + R"(","operationId":")" + operationId +
                          R"(","state":"Committed","sourceVersion":"0.0.1",)"
                          R"("targetVersion":"0.1.0","projectId":"p1",)"
                          R"("engineBuildIdentity":"test-build","records":[]})");
        FailingFilesystem files(project.root);
        files.failRoot = false;
        SystemWallClock clock;
        ProjectMutationCoordinator coordinator(files);
        JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 1});
        ProjectMigrationTransactionService service(files, clock, coordinator, jobs);
        bool recoveryLeaseObserved{};
        files.onSyncDirectory = [&coordinator, &project, &recoveryLeaseObserved] {
            auto contender = coordinator.TryAcquire({project.root, ProjectMutationOwner::Save, "recovery-lease-probe"});
            recoveryLeaseObserved = contender.HasError();
        };

        REQUIRE((service.InspectPendingRecovery(project.root).action == MigrationRecoveryAction::FinalizeCommittedMigration));
        REQUIRE((service.Recover(project.root).HasValue()));
        REQUIRE((recoveryLeaseObserved));
        REQUIRE((project.Read(".horo/project.json") == rootBefore));
        REQUIRE((service.InspectPendingRecovery(project.root).action == MigrationRecoveryAction::None));
        REQUIRE((std::filesystem::exists(project.root / ".horo/local/migration-cleanup" / operationDirectory)));
        REQUIRE((service.CleanupCommittedMigrations(project.root).HasValue()));
        REQUIRE((!std::filesystem::exists(project.root / ".horo/local/migration-cleanup" / operationDirectory)));
        jobs.Shutdown(ShutdownPolicy::Drain);
    }

    TEST_CASE("Recovery Journal Rejects Duplicate Keys", "[unit][editor]") {
        Project project;
        const std::string operationId(32, 'a');
        const std::filesystem::path operationDirectory{operationId};
        const std::string writer = FormatHoroVersion(CurrentEngineReleaseVersion().value);
        project.Write(std::filesystem::path{".horo/local/migration"} / operationDirectory / "journal.json",
                      R"({"writerHoroVersion":")" + writer + R"(","operationId":")" + operationId +
                          R"(","state":"Preparing","state":"Committed","sourceVersion":"0.0.1",)" +
                          R"("targetVersion":"0.1.0","projectId":"p1","engineBuildIdentity":"test","records":[]})");
        NativeDurableFileSystem files;
        SystemWallClock clock;
        ProjectMutationCoordinator coordinator(files);
        JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 1});
        ProjectMigrationTransactionService service(files, clock, coordinator, jobs);
        const std::string rootBefore = project.Read(".horo/project.json");
        REQUIRE((service.InspectPendingRecovery(project.root).action == MigrationRecoveryAction::Unrecoverable));
        REQUIRE((service.Recover(project.root).HasError()));
        REQUIRE((project.Read(".horo/project.json") == rootBefore));
        REQUIRE((std::filesystem::exists(project.root / ".horo/local/migration" / operationDirectory / "journal.json")));
        jobs.Shutdown(ShutdownPolicy::Drain);
    }

    TEST_CASE("Supported Old Writer And Committed Cleanup Are Handled Separately", "[unit][editor]") {
        Project project;
        const std::string operationId(32, 'b');
        const std::filesystem::path operationDirectory{operationId};
        const std::string contract = FormatMigrationRecoveryContractId(CurrentMigrationRecoveryContractId());
        const std::string journal = R"({"writerHoroVersion":"0.0.0","recoveryContract":")" + contract + R"(","operationId":")" +
                                    operationId +
                                    R"(","state":"Preparing","sourceVersion":"0.0.0",)"
                                    R"("targetVersion":"0.0.1","projectId":"p1",)"
                                    R"("engineBuildIdentity":"old-test","records":[]})";
        project.Write(std::filesystem::path{".horo/local/migration"} / operationDirectory / "journal.json", journal);
        NativeDurableFileSystem files;
        SystemWallClock clock;
        ProjectMutationCoordinator coordinator(files);
        JobSystem jobs({.workerCount = 1, .maxQueuedJobs = 1});
        ProjectMigrationTransactionService service(files, clock, coordinator, jobs);
        REQUIRE((service.InspectPendingRecovery(project.root).action == MigrationRecoveryAction::DiscardUnpublishedStaging));
        REQUIRE((service.Recover(project.root).HasValue()));

        std::string committed = journal;
        const auto statePosition = committed.find("Preparing");
        committed.replace(statePosition, std::string("Preparing").size(), "Committed");
        project.Write(std::filesystem::path{".horo/local/migration-cleanup"} / operationDirectory / "journal.json", committed);
        REQUIRE((service.InspectPendingRecovery(project.root).action == MigrationRecoveryAction::None));
        REQUIRE((service.CleanupCommittedMigrations(project.root).HasValue()));
        REQUIRE((!std::filesystem::exists(project.root / ".horo/local/migration-cleanup" / operationDirectory)));
        jobs.Shutdown(ShutdownPolicy::Drain);
    }
}  // namespace
