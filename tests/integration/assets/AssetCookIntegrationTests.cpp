#include "HeadlessMeshCooker.h"
#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetCookOutput.h"
#include "Horo/Assets/AssetCookService.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Assets/CookCatalog.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/JobSystem.h"
#include "assets/AssetCookPublicationFixture.h"

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

    struct TempDir {
        std::filesystem::path path;

        TempDir() {
            auto tmp = std::filesystem::temp_directory_path() / "horo_integration_test";
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

    /** @brief Fixture project directory path. */
    const auto kFixtureProject = std::filesystem::path{PROJECT_SOURCE_DIR} / "tests" / "fixtures" / "assets" / "headless_mesh";

}  // namespace

TEST_CASE("End-to-end headless cook pipeline: source to provider", "[native][integration][assets][cooking]") {
    REQUIRE((std::filesystem::exists(kFixtureProject)));

    TempDir cacheDir;
    TempDir cookedDir;

    JobSystem jobs;

    // 1. Build the asset registry from fixture sidecar
    auto candidate = PrepareAssetRegistryCandidate(kFixtureProject);
    REQUIRE((candidate.HasValue()));
    REQUIRE((candidate.Value().status == AssetRegistryBuildStatus::Complete));

    AssetRegistry registry;
    auto loadResult = LoadAssetRegistry(registry, kFixtureProject, AssetRegistryOpenMode::ReadOnly);
    REQUIRE((loadResult.HasValue()));
    REQUIRE((loadResult.Value().registeredAssets >= 1));

    auto snapshot = registry.Snapshot();
    auto records = snapshot.Records();
    REQUIRE((records.size() >= 1));

    // 2. Build the cooker catalog with headless mesh cooker
    CookerCatalog cookerCatalog;
    REQUIRE((RegisterHeadlessMeshCooker(cookerCatalog).HasValue()));
    auto catSnapshot = cookerCatalog.Publish();
    REQUIRE((catSnapshot.HasValue()));

    // 3. Run the cook service
    AssetCookService service(jobs, catSnapshot.Value());

    AssetCookRequest request{
        .sourceRoot = kFixtureProject,
        .cacheRoot = cacheDir.path,
        .cookedRoot = cookedDir.path,
        .registry = snapshot,
        .target = Target("headless-null"),
    };
    Horo::Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);

    CancellationToken cancellation;
    auto cookResult = service.Cook(request, cancellation);
    REQUIRE((cookResult.HasValue()));

    auto &report = cookResult.Value();
    REQUIRE((report.totalAssets == records.size()));
    REQUIRE((report.cookedAssets + report.cacheHits == records.size()));
    REQUIRE((std::filesystem::exists(cookedDir.path / "current.json")));

    // 4. Resolve the generation
    auto genResult = ResolveCurrentCookGeneration(cookedDir.path);
    REQUIRE((genResult.HasValue()));
    auto &gen = genResult.Value();
    REQUIRE((gen.target == Target("headless-null")));
    REQUIRE((gen.artifactCount == records.size()));

    // 5. Load through FilesystemAssetProvider
    FilesystemAssetProvider provider(gen.generationRoot);
    for (const auto &record : records) {
        auto existsResult = provider.Exists(record.id, cancellation);
        REQUIRE((existsResult.HasValue()));
        REQUIRE((existsResult.Value()));

        auto loadResult = provider.Load(record.id, cancellation);
        REQUIRE((loadResult.HasValue()));
        REQUIRE((!loadResult.Value().empty()));

        // Decode the artifact
        auto decoded = DecodeCookedArtifact(loadResult.Value());
        REQUIRE((decoded.HasValue()));
        auto &artifact = decoded.Value();
        REQUIRE((artifact.id == record.id));
        REQUIRE((artifact.type == record.type));
        REQUIRE((artifact.target == Target("headless-null")));
        REQUIRE((!artifact.payload.empty()));
    }
}

TEST_CASE("End-to-end cache hit on second run", "[native][integration][assets][cooking]") {
    REQUIRE((std::filesystem::exists(kFixtureProject)));

    TempDir cacheDir;
    TempDir cookedDir1;
    TempDir cookedDir2;

    JobSystem jobs;

    AssetRegistry registry;
    auto loadResult = LoadAssetRegistry(registry, kFixtureProject, AssetRegistryOpenMode::ReadOnly);
    REQUIRE((loadResult.HasValue()));
    auto snapshot = registry.Snapshot();

    CookerCatalog cookerCatalog;
    REQUIRE((RegisterHeadlessMeshCooker(cookerCatalog).HasValue()));
    auto catSnapshot = cookerCatalog.Publish();
    REQUIRE((catSnapshot.HasValue()));

    AssetCookService service(jobs, catSnapshot.Value());

    // First run — all misses
    {
        AssetCookRequest request{
            .sourceRoot = kFixtureProject,
            .cacheRoot = cacheDir.path,
            .cookedRoot = cookedDir1.path,
            .registry = snapshot,
            .target = Target("headless-null"),
        };
        Horo::Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);
        CancellationToken cancellation;
        auto result = service.Cook(request, cancellation);
        REQUIRE((result.HasValue()));
        REQUIRE((result.Value().cacheHits == 0));
        REQUIRE((result.Value().cookedAssets == snapshot.Records().size()));
    }

    // Second run — all hits
    {
        AssetCookRequest request{
            .sourceRoot = kFixtureProject,
            .cacheRoot = cacheDir.path,
            .cookedRoot = cookedDir2.path,
            .registry = snapshot,
            .target = Target("headless-null"),
        };
        Horo::Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(request);
        CancellationToken cancellation;
        auto result = service.Cook(request, cancellation);
        REQUIRE((result.HasValue()));
        REQUIRE((result.Value().cacheHits == snapshot.Records().size()));
        REQUIRE((result.Value().cookedAssets == 0));
    }
}
