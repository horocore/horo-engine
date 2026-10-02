#include "assets/AssetCookServiceFixture.h"

using namespace Horo;
using namespace Horo::Assets;
using namespace Horo::Assets::ServiceTestSupport;

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
    Horo::Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);
    CancellationToken cancellation;

    REQUIRE(service.Cook(request, cancellation).HasValue());
    const auto firstSnapshot = buildOutput.SnapshotIfChanged(0);
    REQUIRE(firstSnapshot.has_value());
    REQUIRE(service.Cook(request, cancellation).HasValue());
    const auto secondSnapshot = buildOutput.SnapshotIfChanged(firstSnapshot->revision);
    REQUIRE(secondSnapshot.has_value());
    AssertCachedCookOutput(*firstSnapshot, *secondSnapshot, project.sourceFile);
}

TEST_CASE("AssetCookService isolates cache entries by exact strategy settings", "[native]") {
    TestProject project;
    TempDir cacheDir;
    TempDir cookedDir;
    JobSystem jobs;
    AssetRegistry registry;
    REQUIRE(registry.Publish({TestMeshRecord()}).status == AssetRegistryBuildStatus::Complete);

    const auto target = Target("headless-null");
    AssetCookRequest request{
        .sourceRoot = project.dir.path,
        .cacheRoot = cacheDir.path,
        .cookedRoot = cookedDir.path,
        .registry = registry.Snapshot(),
        .target = target,
    };
    Horo::Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);
    const auto cookWithSetting = [&](const std::uint8_t setting) {
        CookerCatalog catalog;
        REQUIRE(catalog
                    .Register(CookerContribution{
                        .contributionId = "test.settings-cooker",
                        .assetType = Type("core.mesh"),
                        .targets = {target},
                        .strategy = std::make_shared<const SettingsCooker>(setting),
                    })
                    .HasValue());
        auto snapshot = catalog.Publish();
        REQUIRE(snapshot.HasValue());
        AssetCookService service(jobs, snapshot.Value());
        return service.Cook(request, CancellationToken{});
    };

    auto first = cookWithSetting(1);
    REQUIRE(first.HasValue());
    CHECK(first.Value().cookedAssets == 1);
    auto different = cookWithSetting(2);
    REQUIRE(different.HasValue());
    CHECK(different.Value().cookedAssets == 1);
    CHECK(different.Value().cacheHits == 0);
    auto sameAgain = cookWithSetting(2);
    REQUIRE(sameAgain.HasValue());
    CHECK(sameAgain.Value().cookedAssets == 0);
    CHECK(sameAgain.Value().cacheHits == 1);
    jobs.Shutdown(ShutdownPolicy::Drain);
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
    Horo::Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);
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
    Horo::Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);
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
    Horo::Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);
    CancellationToken cancellation;

    const auto result = service.Cook(request, cancellation);
    REQUIRE(result.HasError());
    REQUIRE(result.ErrorValue().code.Value() == "asset.cook.operation_admission_failed");
    REQUIRE_FALSE(output.SnapshotIfChanged(0).has_value());
    REQUIRE_FALSE(std::filesystem::exists(cookedDir.path / "current.json"));
}
