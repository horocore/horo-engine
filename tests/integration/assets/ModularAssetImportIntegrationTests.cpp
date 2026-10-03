#include "HeadlessMeshCooker.h"
#include "Horo/Assets/AssetCook.h"
#include "Horo/Assets/AssetCookCache.h"
#include "Horo/Assets/AssetCookOutput.h"
#include "Horo/Assets/AssetCookService.h"
#include "Horo/Assets/AssetImportMetadata.h"
#include "Horo/Assets/AssetImportOperation.h"
#include "Horo/Assets/AssetImporter.h"
#include "Horo/Assets/AssetProvider.h"
#include "Horo/Assets/AssetRegistry.h"
#include "Horo/Assets/CookCatalog.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/JobSystem.h"
#include "ProjectAssetImportCommitter.h"
#include "assets/AssetCookPublicationFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
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
            auto tmp = std::filesystem::temp_directory_path() / "horo_full_pipe";
            std::filesystem::create_directories(tmp);
            auto unique = tmp / ("test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            std::filesystem::create_directories(unique);
            path = std::filesystem::canonical(unique);
        }

        ~TempDir() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    };

    void WriteFile(const std::filesystem::path &path, std::span<const std::uint8_t> bytes) {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    void WriteText(const std::filesystem::path &path, std::string_view text) {
        auto bytes = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
        WriteFile(path, bytes);
    }

    /** @brief A simple test importer that produces mesh payload from raw bytes. */
    class TestMeshImporter final : public IAssetImporter {
    public:
        [[nodiscard]] Result<PreparedAssetImport> Import(const AssetImportInput &input,
                                                         const CancellationToken & /*cancellation*/) const override {
            PreparedAssetImport result;
            result.type = AssetTypeId::Parse("core.mesh").Value();
            // Simple: wrap source bytes with a header
            result.editorPayload = {'M', 'E', 'S', 'H'};
            result.editorPayload.insert(result.editorPayload.end(), input.sourceBytes.begin(), input.sourceBytes.end());
            return Result<PreparedAssetImport>::Success(std::move(result));
        }
    };

    class FixedAssetIdGenerator final : public IAssetIdGenerator {
    public:
        [[nodiscard]] AssetId Generate() override {
            return Id("00112233-4455-6677-8899-aabbccddeeff");
        }
    };

}  // namespace

TEST_CASE("Full pipeline: create project, import, cook, load", "[native][integration][assets][pipeline]") {
    TempDir projectDir;
    TempDir cacheDir;
    TempDir cookedDir;

    JobSystem jobs;

    // 1. Set up project structure
    auto assetsDir = projectDir.path / "assets";
    std::filesystem::create_directories(assetsDir);

    // Create a source asset
    std::vector<std::uint8_t> sourceData = {0x01, 0x02, 0x03, 0x04};
    WriteFile(assetsDir / "test.obj", sourceData);

    // 2. Build importer catalog
    AssetImporterCatalog importerCatalog;
    REQUIRE((importerCatalog
                 .Register(AssetImporterContribution{
                     .contributionId = "test.mesh.obj",
                     .packageId = "test.pkg",
                     .moduleId = "test.mod",
                     .moduleVersion = "1.0.0",
                     .version = "1.0.0",
                     .fileExtensions = {"obj"},
                     .assetTypes = {Type("core.mesh")},
                     .strategy = std::make_shared<const TestMeshImporter>(),
                 })
                 .HasValue()));
    auto importerSnapshot = importerCatalog.Publish();
    REQUIRE((importerSnapshot.HasValue()));

    // 3. Run import operation
    AssetImportOperation importOp(jobs, importerSnapshot.Value());

    AssetImportRequest importRequest{
        .projectRoot = projectDir.path,
        .sourceFiles = {assetsDir / "test.obj"},
    };

    CancellationToken cancellation;
    auto startResult = importOp.Start(importRequest, cancellation);
    REQUIRE((startResult.HasValue()));
    REQUIRE((startResult.Value().items.size() == 1));

    auto prepareResult = importOp.ImportSingleItem(0, cancellation);
    REQUIRE((prepareResult.HasValue()));

    // 4. Commit the imported payload, its identity sidecar, and the registry revision.
    AssetRegistry registry;
    auto item = prepareResult.Value().items[0];
    REQUIRE((item.result.has_value()));
    REQUIRE((!item.result->editorPayload.empty()));
    item.displayName = "test_imported";
    item.destinationFolder = "assets/imported";
    item.createMetaSidecar = true;
    item.supportsMetaSidecar = true;

    ProjectAssetImportCommitter committer{&registry};
    FixedAssetIdGenerator idGenerator;
    auto destination = ProjectPath::Parse(item.destinationFolder);
    REQUIRE((destination.HasValue()));
    auto committed = committer.Commit(
        PreparedAssetImportBatch{
            .operationId = "integration-import",
            .projectRoot = projectDir.path,
            .destinationFolder = std::move(destination).Value(),
            .items = {item},
        },
        idGenerator, cancellation);
    REQUIRE((committed.HasValue()));

    const auto importedPath = projectDir.path / "assets/imported/test_imported.horoasset";
    REQUIRE((std::filesystem::exists(importedPath)));
    REQUIRE((std::filesystem::exists(importedPath.string() + ".horo")));
    auto provenance = ReadAssetImportMetadata(std::filesystem::absolute(importedPath.string() + ".horo"));
    REQUIRE(provenance.HasValue());
    REQUIRE(provenance.Value().importerContributionId == "test.mesh.obj");
    REQUIRE(provenance.Value().importerVersion == "1.0.0");
    REQUIRE(provenance.Value().importerModuleId == "test.mod");
    REQUIRE(provenance.Value().importerModuleVersion == "1.0.0");
    REQUIRE(provenance.Value().absoluteSourcePath.is_absolute());
    REQUIRE(!provenance.Value().sourceHash.empty());
    REQUIRE(provenance.Value().lastImportReasons == std::vector{AssetImportReason::InitialImport});
    const AssetRegistrySnapshot importedSnapshot = registry.Snapshot();
    REQUIRE((importedSnapshot.Records().size() == 1));
    REQUIRE((importedSnapshot.Records()[0].sourcePath.String() == "assets/imported/test_imported.horoasset"));
    REQUIRE((importedSnapshot.Records()[0].type.Value() == "core.mesh"));

    // 5. Cook integration: use the fixture project from Phase A
    const auto kFixtureProject = std::filesystem::path{PROJECT_SOURCE_DIR} / "tests" / "fixtures" / "assets" / "headless_mesh";

    if (std::filesystem::exists(kFixtureProject)) {
        AssetRegistry cookRegistry;
        auto loadResult = LoadAssetRegistry(cookRegistry, kFixtureProject, AssetRegistryOpenMode::ReadOnly);
        if (loadResult.HasValue() && loadResult.Value().registeredAssets >= 1) {
            auto snapshot = cookRegistry.Snapshot();

            CookerCatalog cookerCatalog;
            REQUIRE((RegisterHeadlessMeshCooker(cookerCatalog).HasValue()));
            auto catSnapshot = cookerCatalog.Publish();
            REQUIRE((catSnapshot.HasValue()));

            AssetCookService cookService(jobs, catSnapshot.Value());

            AssetCookRequest cookRequest{
                .sourceRoot = kFixtureProject,
                .cacheRoot = cacheDir.path,
                .cookedRoot = cookedDir.path,
                .registry = snapshot,
                .target = Target("headless-null"),
            };
            Horo::Assets::CookPublicationTestSupport::ConfigureNativeCookPublication(cookRequest);

            auto cookResult = cookService.Cook(cookRequest, cancellation);
            REQUIRE((cookResult.HasValue()));
            REQUIRE((cookResult.Value().totalAssets >= 1));

            // Resolve generation
            auto genResult = ResolveCurrentCookGeneration(cookedDir.path);
            REQUIRE((genResult.HasValue()));

            // Load through FilesystemAssetProvider
            FilesystemAssetProvider provider(genResult.Value().generationRoot);
            for (const auto &record : snapshot.Records()) {
                auto loadResult = provider.Load(record.id, cancellation);
                REQUIRE((loadResult.HasValue()));
            }
        }
    }
}
