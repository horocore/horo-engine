#include "assets/AssetCookServiceFixture.h"

using namespace Horo;
using namespace Horo::Assets;
using namespace Horo::Assets::ServiceTestSupport;

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
    Horo::Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);

    CancellationToken cancellation;
    auto result = service.Cook(request, cancellation);
    REQUIRE((result.HasValue()));

    auto &report = result.Value();
    REQUIRE((report.totalAssets == 0));
    REQUIRE((report.cookedAssets == 0));
    REQUIRE((report.cacheHits == 0));
    const auto current = ResolveCurrentCookGeneration(request.cookedRoot, request.limits);
    REQUIRE(current.HasValue());
    CHECK(current.Value().manifestDigest == report.generation.manifestDigest);
    CHECK(current.Value().artifactCount == 0);
    const auto contents = ReadCookGenerationContents(current.Value(), request.limits.maximumArtifactBytes, request.limits);
    REQUIRE(contents.HasValue());
    CHECK(contents.Value().entries.empty());
    CHECK(contents.Value().artifacts.empty());
    AssertEmptyCookOutputScopes(buildOutput, operations);
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
    Horo::Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);

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
        Horo::Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);

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

TEST_CASE("AssetCookService rejects missing host publication authority before writing", "[native]") {
    for (const bool missingFiles : {false, true}) {
        EmptyCookPublicationFixture fixture;
        if (missingFiles)
            fixture.request.publicationFiles.reset();
        else
            fixture.request.newPublicationOperationId = {};
        const auto result = fixture.service.Cook(fixture.request, {});
        REQUIRE(result.HasError());
        CHECK_FALSE(std::filesystem::exists(fixture.cooked.path / "current.json"));
        CHECK(std::filesystem::is_empty(fixture.cache.path));
    }
}

TEST_CASE("AssetCookService honors the same native writer lock as partial generation publishers", "[native]") {
    EmptyCookPublicationFixture fixture;
    {
        auto lock = fixture.request.publicationFiles->TryAcquireExclusive(fixture.cooked.path / ".cook-writer.lock", "another publisher");
        REQUIRE(lock.HasValue());
        const auto blocked = fixture.service.Cook(fixture.request, {});
        REQUIRE(blocked.HasError());
        CHECK_FALSE(std::filesystem::exists(fixture.cooked.path / "current.json"));
    }
    REQUIRE(fixture.service.Cook(fixture.request, {}).HasValue());
    REQUIRE(ResolveCurrentCookGeneration(fixture.cooked.path).HasValue());
}

TEST_CASE("AssetCookService publication cancellation preserves true commit outcome and durability diagnostic", "[native]") {
    for (const bool committed : {false, true}) {
        EmptyCookPublicationFixture fixture;
        CancellationSource cancellation;
        auto files = std::make_shared<CookPublicationFiles>();
        files->cancellation = &cancellation;
        files->cancelAfterCommit = committed;
        files->failCommittedSync = committed;
        fixture.request.publicationFiles = files;
        OperationStore operations{4, 4};
        fixture.request.operationStore = &operations;
        const auto result = fixture.service.Cook(fixture.request, cancellation.Token());
        CHECK(cancellation.Token().IsCancellationRequested());
        CHECK(result.HasValue() == committed);
        const auto snapshot = operations.SnapshotIfChanged(0);
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->operations.front().state == (committed ? OperationState::Succeeded : OperationState::Cancelled));
        if (committed) {
            REQUIRE(result.HasValue());
            REQUIRE(result.Value().generation.durabilityError.has_value());
            CHECK(result.Value().generation.durabilityError->code.Value() == "test.cook.publication_sync_failed");
            REQUIRE(ResolveCurrentCookGeneration(fixture.cooked.path).HasValue());
        } else {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == "asset.cook.cancelled");
            CHECK(ResolveCurrentCookGeneration(fixture.cooked.path).HasError());
        }
    }
}

TEST_CASE("AssetCookService rejects corrupt current authority instead of replacing it", "[native]") {
    EmptyCookPublicationFixture fixture;
    const std::array<std::uint8_t, 3> malformed{'b', 'a', 'd'};
    WriteFile(fixture.cooked.path / "current.json", malformed);
    const auto result = fixture.service.Cook(fixture.request, {});
    REQUIRE(result.HasError());
    CHECK(std::filesystem::file_size(fixture.cooked.path / "current.json") == malformed.size());
}

TEST_CASE("AssetCookService contains optional history exceptions before and after pointer commit", "[native]") {
    for (const bool committed : {false, true}) {
        for (const bool nonstandard : {false, true}) {
            EmptyCookPublicationFixture fixture;
            CancellationSource cancellation;
            auto files = std::make_shared<CookPublicationFiles>();
            files->cancellation = &cancellation;
            files->cancelAfterCommit = committed;
            fixture.request.publicationFiles = files;
            auto sink = std::make_shared<ThrowingCookHistorySink>();
            sink->nonstandard = nonstandard;
            OperationStore operations{4, 4, sink};
            fixture.request.operationStore = &operations;
            const auto result = fixture.service.Cook(fixture.request, cancellation.Token());
            CHECK(result.HasValue() == committed);
            CHECK(sink->attempted == 1);
            const auto snapshot = operations.SnapshotIfChanged(0);
            REQUIRE(snapshot.has_value());
            CHECK(snapshot->operations.front().state == (committed ? OperationState::Succeeded : OperationState::Cancelled));
            CHECK(ResolveCurrentCookGeneration(fixture.cooked.path).HasValue() == committed);
        }
    }
}
