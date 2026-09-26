#include "HeadlessMeshCooker.h"
#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetCookService.h"
#include "Horo/Assets/CookCatalog.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/JobSystem.h"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Assets;

    AssetId Id(const std::string_view value) {
        auto parsed = AssetId::Parse(value);
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    AssetTypeId Type(const std::string_view value) {
        auto parsed = AssetTypeId::Parse(value);
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    AssetCookTargetId Target(const std::string_view value) {
        auto parsed = AssetCookTargetId::Parse(value);
        REQUIRE((parsed.HasValue()));
        return parsed.Value();
    }

    AssetRecord TestMeshRecord() {
        const auto sourcePath = ProjectPath::Parse("assets/test_mesh.fbx");
        const auto metadataPath = ProjectPath::Parse("assets/test_mesh.fbx.horo");
        REQUIRE(sourcePath.HasValue());
        REQUIRE(metadataPath.HasValue());
        return AssetRecord{.id = Id("00000000-0000-0000-0000-0000000000a1"),
                           .type = Type("core.mesh"),
                           .sourcePath = sourcePath.Value(),
                           .metadataPath = metadataPath.Value()};
    }

    struct TempDir {
        std::filesystem::path path;

        TempDir() {
            auto tmp = std::filesystem::temp_directory_path() / "horo_service_test";
            std::filesystem::create_directories(tmp);
            auto unique = tmp / ("test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(unique);
            path = unique;
        }

        ~TempDir() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    /** @brief Creates a minimal file with given content. */
    void WriteFile(const std::filesystem::path &path, std::span<const std::uint8_t> bytes) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    /** @brief Creates a fake .horo sidecar so the registry picks up the file. */
    std::string SidecarJson(std::string_view assetId, std::string_view assetType) {
        return std::string("{\"schemaVersion\":1,\"assetId\":\"") + std::string(assetId) + "\",\"assetType\":\"" + std::string(assetType) +
               "\"}";
    }

    /**
     * @brief Sets up a minimal project directory structure with one source asset and sidecar.
     */
    struct TestProject {
        TempDir dir;
        std::filesystem::path assetsDir;
        std::filesystem::path sourceFile;

        TestProject() {
            assetsDir = dir.path / "assets";
            std::filesystem::create_directories(assetsDir);

            // Create a minimal source file
            sourceFile = assetsDir / "test_mesh.fbx";
            std::vector<std::uint8_t> data = {0x01, 0x02, 0x03, 0x04, 0x05};
            WriteFile(sourceFile, data);

            // Create sidecar
            auto sidecarJson = SidecarJson("00000000-0000-0000-0000-0000000000a1", "core.mesh");
            auto sidecarBytes =
                std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(sidecarJson.data()), sidecarJson.size());
            WriteFile(std::string(sourceFile.string()) + ".horo", sidecarBytes);
        }
    };

    /** @brief Adds a second distinct source asset for concurrent cancellation checks. */
    AssetRecord AddSecondMesh(TestProject &project) {
        const std::filesystem::path sourceFile = project.assetsDir / "second_mesh.fbx";
        std::filesystem::copy_file(project.sourceFile, sourceFile);
        const std::string sidecarJson = SidecarJson("00000000-0000-0000-0000-0000000000a2", "core.mesh");
        const auto sidecarBytes =
            std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(sidecarJson.data()), sidecarJson.size());
        WriteFile(std::string(sourceFile.string()) + ".horo", sidecarBytes);
        AssetRecord record = TestMeshRecord();
        record.id = Id("00000000-0000-0000-0000-0000000000a2");
        record.sourcePath = ProjectPath::Parse("assets/second_mesh.fbx").Value();
        record.metadataPath = ProjectPath::Parse("assets/second_mesh.fbx.horo").Value();
        return record;
    }

    /** @brief Requests cancellation while returning either an acknowledged cook cancellation or a real failure. */
    class CancellingCooker final : public ICookerStrategy {
    public:
        CancellingCooker(CancellationSource &source, const bool fail) : source_(source), fail_(fail) {}

        [[nodiscard]] Result<CookOutputSink> Cook(const CookSourceView &, const CancellationToken &) const override {
            source_.RequestCancellation();
            return Result<CookOutputSink>::Failure(Error{ErrorCode{fail_ ? "test.cook.failed" : "asset.cook.cancelled"}});
        }

    private:
        CancellationSource &source_;
        bool fail_;
    };

    /** @brief Verifies that the initial cook and later cache hit retain source attribution. */
    void AssertCachedCookOutput(const BuildOutputSnapshot &first, const BuildOutputSnapshot &second,
                                const std::filesystem::path &sourcePath) {
        const auto cached = std::ranges::find_if(second.records, [](const BuildOutputRecord &record) {
            return record.code.Value() == "asset.cook.cache_hit";
        });
        REQUIRE((cached != second.records.end()));
        REQUIRE((cached->result == BuildOutputResult::Cached));
        REQUIRE(cached->source.has_value());
        REQUIRE(cached->source->absolutePath == sourcePath.string());
        REQUIRE(cached->sessionId.has_value());
        REQUIRE((cached->sessionId == second.records.back().sessionId));
        REQUIRE((second.records.back().result == BuildOutputResult::Succeeded));

        const auto cooked = std::ranges::find_if(first.records, [](const BuildOutputRecord &record) {
            return record.code.Value() == "asset.cook.asset_cooked";
        });
        REQUIRE(cooked != first.records.end());
        REQUIRE(cooked->result == BuildOutputResult::Succeeded);
        REQUIRE(cooked->source.has_value());
        REQUIRE(cooked->source->absolutePath == sourcePath.string());
    }

    /** @brief Checks that both a failed or cancelled cook and its cancelled sibling retain source attribution. */
    void AssertCancelledSiblingOutput(const BuildOutputSnapshot &snapshot, const OperationRecord &operation, const TestProject &project,
                                      const bool fail) {
        REQUIRE(snapshot.records.back().result == (fail ? BuildOutputResult::Failed : BuildOutputResult::Cancelled));
        const auto assetFailure = std::ranges::find_if(snapshot.records, [](const BuildOutputRecord &record) {
            return record.source.has_value();
        });
        REQUIRE(assetFailure != snapshot.records.end());
        REQUIRE(assetFailure->result == (fail ? BuildOutputResult::Failed : BuildOutputResult::Cancelled));
        REQUIRE(assetFailure->source->absolutePath == project.sourceFile.string());
        REQUIRE(assetFailure->operationId == operation.id);
        REQUIRE(assetFailure->sessionId == snapshot.records.back().sessionId);
        const std::filesystem::path secondSource = project.assetsDir / "second_mesh.fbx";
        REQUIRE(std::ranges::count_if(snapshot.records, [&](const BuildOutputRecord &record) {
            return record.source.has_value() && record.source->absolutePath == secondSource.string() &&
                   record.result == BuildOutputResult::Cancelled;
        }) == 1);
    }

}  // namespace

TEST_CASE("AssetCookService empty registry publishes empty generation", "[native]") {
    TestProject project;
    TempDir cacheDir;
    TempDir cookedDir;

    JobSystem jobs;

    // Build an empty registry
    AssetRegistry registry;
    auto emptySnapshot = registry.Snapshot();

    // Build catalog with headless mesh cooker
    CookerCatalog catalog;
    REQUIRE((RegisterHeadlessMeshCooker(catalog).HasValue()));
    auto catSnapshot = catalog.Publish();
    REQUIRE((catSnapshot.HasValue()));

    AssetCookService service(jobs, catSnapshot.Value());
    BuildOutputStore buildOutput{8};
    OperationStore operations{4, 4};

    AssetCookRequest request{
        .sourceRoot = project.dir.path,
        .cacheRoot = cacheDir.path,
        .cookedRoot = cookedDir.path,
        .registry = emptySnapshot,
        .target = Target("headless-null"),
        .buildOutputStore = &buildOutput,
        .operationStore = &operations,
    };

    CancellationToken cancellation;
    auto result = service.Cook(request, cancellation);
    REQUIRE((result.HasValue()));

    auto &report = result.Value();
    REQUIRE((report.totalAssets == 0));
    REQUIRE((report.cookedAssets == 0));
    REQUIRE((report.cacheHits == 0));
    const auto buildSnapshot = buildOutput.SnapshotIfChanged(0);
    REQUIRE(buildSnapshot.has_value());
    REQUIRE((buildSnapshot->records.size() == 2U));
    REQUIRE((buildSnapshot->records.back().result == BuildOutputResult::Succeeded));
    REQUIRE((buildSnapshot->records.back().code.Value() == "asset.cook.succeeded"));
    REQUIRE((std::ranges::count_if(buildSnapshot->records, [](const BuildOutputRecord &record) {
        return record.result != BuildOutputResult::None;
    }) == 1));
    const auto operationSnapshot = operations.SnapshotIfChanged(0);
    REQUIRE(operationSnapshot.has_value());
    REQUIRE((operationSnapshot->operations.size() == 1));
    REQUIRE((operationSnapshot->operations.front().state == OperationState::Succeeded));
    for (const BuildOutputRecord &record : buildSnapshot->records) {
        REQUIRE(record.sessionId.has_value());
        REQUIRE(record.sessionId->IsValid());
        REQUIRE((record.sessionId == buildSnapshot->records.front().sessionId));
        REQUIRE((record.operationId == operationSnapshot->operations.front().id));
    }
}

TEST_CASE("AssetCookService honours cancellation before work", "[native]") {
    TestProject project;
    TempDir cacheDir;
    TempDir cookedDir;

    JobSystem jobs;

    AssetRegistry registry;
    auto snapshot = registry.Snapshot();

    CookerCatalog catalog;
    REQUIRE((RegisterHeadlessMeshCooker(catalog).HasValue()));
    auto catSnapshot = catalog.Publish();
    REQUIRE((catSnapshot.HasValue()));

    AssetCookService service(jobs, catSnapshot.Value());

    CancellationSource cancelSource;
    cancelSource.RequestCancellation();
    auto cancellation = cancelSource.Token();

    AssetCookRequest request{
        .sourceRoot = project.dir.path,
        .cacheRoot = cacheDir.path,
        .cookedRoot = cookedDir.path,
        .registry = snapshot,
        .target = Target("headless-null"),
    };

    auto result = service.Cook(request, cancellation);
    REQUIRE((result.HasError()));
}

TEST_CASE("AssetCookService keeps cook cancellation separate from concurrent failure", "[native]") {
    for (const bool fail : {false, true}) {
        TestProject project;
        TempDir cacheDir;
        TempDir cookedDir;
        JobSystem jobs{JobSystemConfig{.workerCount = 1, .maxQueuedJobs = 4}};
        CancellationSource source;

        AssetRegistry registry;
        REQUIRE(registry.Publish({TestMeshRecord(), AddSecondMesh(project)}).status == AssetRegistryBuildStatus::Complete);

        CookerCatalog catalog;
        REQUIRE(catalog
                    .Register(CookerContribution{.contributionId = "test.cancelling",
                                                 .assetType = Type("core.mesh"),
                                                 .targets = {Target("headless-null")},
                                                 .strategy = std::make_shared<const CancellingCooker>(source, fail)})
                    .HasValue());
        const auto catalogSnapshot = catalog.Publish();
        REQUIRE(catalogSnapshot.HasValue());
        AssetCookService service(jobs, catalogSnapshot.Value());
        BuildOutputStore output{16};
        OperationStore operations{4, 4};
        AssetCookRequest request{.sourceRoot = project.dir.path,
                                 .cacheRoot = cacheDir.path,
                                 .cookedRoot = cookedDir.path,
                                 .registry = registry.Snapshot(),
                                 .target = Target("headless-null"),
                                 .buildOutputStore = &output,
                                 .operationStore = &operations};

        const auto result = service.Cook(request, source.Token());
        REQUIRE(result.HasError());
        REQUIRE(result.ErrorValue().code.Value() == (fail ? "test.cook.failed" : "asset.cook.cancelled"));
        if (!fail)
            REQUIRE(ErrorChainContains(result.ErrorValue(), ErrorDomainId{"horo.foundation.jobs"}, ErrorCode{"job.cancelled"}));
        const auto operationSnapshot = operations.SnapshotIfChanged(0);
        REQUIRE(operationSnapshot.has_value());
        REQUIRE(operationSnapshot->operations.front().state == (fail ? OperationState::Failed : OperationState::Cancelled));
        const auto outputSnapshot = output.SnapshotIfChanged(0);
        REQUIRE(outputSnapshot.has_value());
        AssertCancelledSiblingOutput(*outputSnapshot, operationSnapshot->operations.front(), project, fail);
        jobs.Shutdown(ShutdownPolicy::Drain);
    }
}

TEST_CASE("AssetCookService publishes cache hits as cached scoped results", "[native]") {
    TestProject project;
    TempDir cacheDir;
    TempDir cookedDir;
    JobSystem jobs;

    AssetRegistry registry;
    const auto sourcePath = ProjectPath::Parse("assets/test_mesh.fbx");
    const auto metadataPath = ProjectPath::Parse("assets/test_mesh.fbx.horo");
    REQUIRE(sourcePath.HasValue());
    REQUIRE(metadataPath.HasValue());
    const AssetRegistryBuildReport registryBuild = registry.Publish({AssetRecord{.id = Id("00000000-0000-0000-0000-0000000000a1"),
                                                                                 .type = Type("core.mesh"),
                                                                                 .sourcePath = sourcePath.Value(),
                                                                                 .metadataPath = metadataPath.Value()}});
    REQUIRE((registryBuild.status == AssetRegistryBuildStatus::Complete));

    CookerCatalog catalog;
    REQUIRE((RegisterHeadlessMeshCooker(catalog).HasValue()));
    const auto catalogSnapshot = catalog.Publish();
    REQUIRE(catalogSnapshot.HasValue());
    AssetCookService service(jobs, catalogSnapshot.Value());
    BuildOutputStore buildOutput{16};
    AssetCookRequest request{
        .sourceRoot = project.dir.path,
        .cacheRoot = cacheDir.path,
        .cookedRoot = cookedDir.path,
        .registry = registry.Snapshot(),
        .target = Target("headless-null"),
        .buildOutputStore = &buildOutput,
    };
    CancellationToken cancellation;

    REQUIRE(service.Cook(request, cancellation).HasValue());
    const auto firstSnapshot = buildOutput.SnapshotIfChanged(0);
    REQUIRE(firstSnapshot.has_value());
    REQUIRE(service.Cook(request, cancellation).HasValue());
    const auto secondSnapshot = buildOutput.SnapshotIfChanged(firstSnapshot->revision);
    REQUIRE(secondSnapshot.has_value());
    AssertCachedCookOutput(*firstSnapshot, *secondSnapshot, project.sourceFile);
}

TEST_CASE("AssetCookService reports source admission failures with navigable diagnostics", "[native]") {
    TestProject project;
    TempDir cacheDir;
    TempDir cookedDir;
    JobSystem jobs;
    AssetRegistry registry;
    REQUIRE(registry.Publish({TestMeshRecord()}).status == AssetRegistryBuildStatus::Complete);
    CookerCatalog catalog;
    const auto catalogSnapshot = catalog.Publish();
    REQUIRE(catalogSnapshot.HasValue());
    AssetCookService service(jobs, catalogSnapshot.Value());
    BuildOutputStore output{8};
    OperationStore operations{4, 4};
    AssetCookRequest request{.sourceRoot = project.dir.path,
                             .cacheRoot = cacheDir.path,
                             .cookedRoot = cookedDir.path,
                             .registry = registry.Snapshot(),
                             .target = Target("headless-null"),
                             .buildOutputStore = &output,
                             .operationStore = &operations};
    CancellationToken cancellation;

    const auto result = service.Cook(request, cancellation);
    REQUIRE(result.HasError());
    REQUIRE(result.ErrorValue().code.Value() == "asset.cook.cooker_missing");
    const auto snapshot = output.SnapshotIfChanged(0);
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->records.size() == 3);
    REQUIRE(snapshot->records[1].code.Value() == "asset.cook.cooker_missing");
    REQUIRE(snapshot->records[1].source.has_value());
    REQUIRE(snapshot->records[1].source->absolutePath == project.sourceFile.string());
    REQUIRE(snapshot->records[1].result == BuildOutputResult::Failed);
    REQUIRE(snapshot->records[2].result == BuildOutputResult::Failed);
}

TEST_CASE("AssetCookService reports unreadable source paths without publishing a generation", "[native]") {
    TestProject project;
    TempDir cacheDir;
    TempDir cookedDir;
    JobSystem jobs;
    AssetRegistry registry;
    REQUIRE(registry.Publish({TestMeshRecord()}).status == AssetRegistryBuildStatus::Complete);
    CookerCatalog catalog;
    REQUIRE(RegisterHeadlessMeshCooker(catalog).HasValue());
    const auto catalogSnapshot = catalog.Publish();
    REQUIRE(catalogSnapshot.HasValue());
    AssetCookService service(jobs, catalogSnapshot.Value());
    BuildOutputStore output{8};
    AssetCookRequest request{.sourceRoot = project.dir.path,
                             .cacheRoot = cacheDir.path,
                             .cookedRoot = cookedDir.path,
                             .registry = registry.Snapshot(),
                             .target = Target("headless-null"),
                             .buildOutputStore = &output};
    REQUIRE(std::filesystem::remove(project.sourceFile));
    CancellationToken cancellation;

    const auto result = service.Cook(request, cancellation);
    REQUIRE(result.HasError());
    const auto snapshot = output.SnapshotIfChanged(0);
    REQUIRE(snapshot.has_value());
    REQUIRE(snapshot->records.size() == 3);
    REQUIRE(snapshot->records[1].code.Value() == result.ErrorValue().code.Value());
    REQUIRE(snapshot->records[1].source.has_value());
    REQUIRE(snapshot->records[1].source->absolutePath == project.sourceFile.string());
    REQUIRE_FALSE(std::filesystem::exists(cookedDir.path / "current.json"));
}

TEST_CASE("AssetCookService rejects cook when operation admission is full", "[native]") {
    TestProject project;
    TempDir cacheDir;
    TempDir cookedDir;
    JobSystem jobs;
    AssetRegistry registry;
    CookerCatalog catalog;
    REQUIRE(RegisterHeadlessMeshCooker(catalog).HasValue());
    const auto catalogSnapshot = catalog.Publish();
    REQUIRE(catalogSnapshot.HasValue());
    AssetCookService service(jobs, catalogSnapshot.Value());
    BuildOutputStore output{8};
    OperationStore operations{1, 4};
    const auto occupied = operations.Begin(OperationDescriptor{.kind = OperationKind::Cook, .title = "Other cook"});
    REQUIRE(occupied.has_value());
    AssetCookRequest request{.sourceRoot = project.dir.path,
                             .cacheRoot = cacheDir.path,
                             .cookedRoot = cookedDir.path,
                             .registry = registry.Snapshot(),
                             .target = Target("headless-null"),
                             .buildOutputStore = &output,
                             .operationStore = &operations};
    CancellationToken cancellation;

    const auto result = service.Cook(request, cancellation);
    REQUIRE(result.HasError());
    REQUIRE(result.ErrorValue().code.Value() == "asset.cook.operation_admission_failed");
    REQUIRE_FALSE(output.SnapshotIfChanged(0).has_value());
    REQUIRE_FALSE(std::filesystem::exists(cookedDir.path / "current.json"));
}
