#include "Horo/Application/ProjectAssetImportCommitter.h"
#include "Horo/Assets/AssetSourcePublication.h"
#include "NativePublicationFiles.h"
#include "OwnedTestDirectory.h"
#include "terrain/TerrainAssetFormat.h"

#include <catch2/catch_test_macros.hpp>
#include <functional>

using namespace Horo;
using namespace Horo::Assets;

namespace {
    struct Ids final : IAssetIdGenerator {
        AssetId Generate() override {
            return AssetId::Parse("00000000-0000-0000-0000-000000000009").Value();
        }
    };

    class Files final : public TestSupport::NativePublicationFiles {
    public:
        int writes{}, promotions{}, failWrite{}, failPromotion{}, uncertainPromotion{};
        std::function<void()> beforePromotion;

        Result<void> WritePrivateDurable(const std::filesystem::path &path, std::span<const std::byte> bytes, bool &created,
                                         std::filesystem::perms permissions) override {
            if (++writes == failWrite)
                return Result<void>::Failure(MakeError(AssetSourceErrors::ReadFailed));
            return native.WritePrivateDurable(path, bytes, created, permissions);
        }

        Result<void> AtomicReplaceTracked(const std::filesystem::path &stage, const std::filesystem::path &path,
                                          AtomicFileReplacementReceipt &receipt) override {
            if (beforePromotion)
                beforePromotion();
            if (++promotions == failPromotion)
                return Result<void>::Failure(MakeError(AssetSourceErrors::ReadFailed));
            auto result = native.AtomicReplaceTracked(stage, path, receipt);
            if (receipt.WasCommitted() && promotions == uncertainPromotion)
                return Result<void>::Failure(MakeError(AssetSourceErrors::DurabilityUnconfirmed));
            return result;
        }
    };

    struct Project final {
        Tests::OwnedTestDirectory directory{"terrain-import-publication"};
        Files files;
        Editor::ProjectMutationCoordinator mutations{files};
        AssetRegistry registry;
        Application::ProjectAssetImportCommitter committer{mutations, files, &registry};
        Ids ids;
        PreparedAssetImportBatch batch{.destinationFolder = ProjectPath::Parse("assets/terrain").Value()};

        Project() {
            const auto contribution = Editor::TerrainRawImporterContribution();
            std::vector<std::uint8_t> bytes(9 * 9 * 4);
            const auto imported =
                contribution.strategy->Import({.sourceBytes = bytes,
                                               .sourceExtension = "terrainraw",
                                               .settings = {std::int64_t{9}, std::int64_t{9}, 1.0, 1.0, 123.0, -456.0, std::string{}}},
                                              {});
            REQUIRE(imported.HasValue());
            AssetImportItem item{.sourceFile = ProjectPath::Parse("original.terrainraw").Value()};
            item.absoluteSourcePath = directory.Path() / "original.terrainraw";
            item.importerContributionId = contribution.contributionId;
            item.importerVersion = contribution.version;
            item.importerPackageId = contribution.packageId;
            item.importerModuleId = contribution.moduleId;
            item.importerModuleVersion = contribution.moduleVersion;
            item.sourceExtension = "terrainraw";
            item.displayName = "land";
            item.result = imported.Value();
            item.sourceHash = HashAssetImportSource(bytes);
            item.sourceByteSize = bytes.size();
            item.settings = {{"settings.terrain-width", "9"},      {"settings.terrain-height", "9"},
                             {"settings.terrain-spacing-x", "1"},  {"settings.terrain-spacing-z", "1"},
                             {"settings.terrain-origin-x", "123"}, {"settings.terrain-origin-z", "-456"},
                             {"settings.terrain-crs", ""}};
            batch = {.operationId = "terrain-import-test",
                     .projectRoot = directory.Path(),
                     .destinationFolder = ProjectPath::Parse("assets/terrain").Value(),
                     .items = {std::move(item)}};
        }

        std::filesystem::path Payload() const {
            return directory.Path() / "assets/terrain/land.horoasset";
        }

        std::filesystem::path Sidecar() const {
            auto path = Payload();
            path += ".horo";
            return path;
        }
    };
}  // namespace

TEST_CASE("Explicit terrain importer publishes a complete real canonical asset pair", "[terrain][import]") {
    Project project;
    bool observed{};
    project.files.beforePromotion = [&] {
        observed = true;
        REQUIRE(project.registry.Snapshot().Records().empty());
        REQUIRE(project.files.writes >= 3);  // Both complete application stages precede the first publisher stage.
    };
    REQUIRE(project.committer.Commit(project.batch, project.ids, {}).HasValue());
    REQUIRE(observed);
    REQUIRE(project.committer.LastReceipt().status == Application::ProjectAssetImportStatus::Published);
    REQUIRE(project.committer.LastReceipt().registryPublished);
    const auto metadata = ReadAssetImportMetadata(project.Sidecar());
    REQUIRE(metadata.HasValue());
    const auto source = CaptureAssetSource(project.directory.Path(), project.Payload(), 4096);
    REQUIRE(source.HasValue());
    const auto canonical =
        Editor::DecodeTerrainAsset(metadata.Value(), source.Value().bytes, Terrain::TerrainSourceRevision::Create(1).Value());
    REQUIRE(canonical.HasValue());
    REQUIRE(canonical.Value().heightsMeters.size() == 81);
    REQUIRE(canonical.Value().coordinates.originX == 123);
    REQUIRE(canonical.Value().coordinates.originZ == -456);
    REQUIRE(project.registry.Snapshot().Find(project.ids.Generate()) != nullptr);
    const auto before = source.Value().identity;
    REQUIRE(project.committer.Commit(project.batch, project.ids, {}).HasError());
    REQUIRE(CaptureAssetSource(project.directory.Path(), project.Payload(), 4096).Value().identity == before);
}

TEST_CASE("Terrain initial import stages both files and compensates only its own new destinations", "[terrain][import]") {
    Project project;
    SECTION("Second private stage fails before any promotion") {
        project.files.failWrite = 2;
    }
    SECTION("Second native promotion fails after payload publication") {
        project.files.failPromotion = 2;
    }
    SECTION("Directory sync uncertainty remains explicit after compensation") {
        project.files.uncertainPromotion = 1;
    }
    REQUIRE(project.committer.Commit(project.batch, project.ids, {}).HasError());
    REQUIRE_FALSE(std::filesystem::exists(project.Payload()));
    REQUIRE_FALSE(std::filesystem::exists(project.Sidecar()));
    REQUIRE(project.registry.Snapshot().Records().empty());
    const auto &receipt = project.committer.LastReceipt();
    REQUIRE(receipt.promotedFiles == receipt.rolledBackFiles);
    if (project.files.uncertainPromotion)
        REQUIRE(receipt.status == Application::ProjectAssetImportStatus::ReconciliationRequired);
}

TEST_CASE("Derived index persistence failure retains published registry references and complete terrain pair", "[terrain][import]") {
    Project project;
    REQUIRE(std::filesystem::create_directories(project.directory.Path() / ".horo/asset_index.json"));
    REQUIRE(project.committer.Commit(project.batch, project.ids, {}).HasError());
    REQUIRE(project.committer.LastReceipt().registryPublished);
    REQUIRE(project.committer.LastReceipt().status == Application::ProjectAssetImportStatus::ReconciliationRequired);
    REQUIRE(std::filesystem::exists(project.Payload()));
    REQUIRE(ReadAssetImportMetadata(project.Sidecar()).HasValue());
    REQUIRE(project.registry.Snapshot().Find(project.ids.Generate()) != nullptr);
}

TEST_CASE("Persistent terrain imports cannot omit their authoritative identity and coordinate sidecar", "[terrain][import]") {
    Project project;
    SECTION("Creation disabled") {
        project.batch.items[0].createMetaSidecar = false;
    }
    SECTION("Provider does not support metadata") {
        project.batch.items[0].supportsMetaSidecar = false;
    }
    const auto result = project.committer.Commit(project.batch, project.ids, {});
    REQUIRE(result.HasError());
    REQUIRE(result.ErrorValue().code.Value() == "asset.import.terrain.sidecar_required");
    REQUIRE(project.files.writes == 0);
    REQUIRE(project.files.promotions == 0);
    REQUIRE_FALSE(std::filesystem::exists(project.Payload()));
    REQUIRE_FALSE(std::filesystem::exists(project.Sidecar()));
    REQUIRE(project.registry.Snapshot().Records().empty());
}

TEST_CASE("Other importers retain explicitly selected metadata-free payload publication", "[terrain][import]") {
    Project project;
    project.batch.items[0].result->type = AssetTypeId::Parse("core.mesh").Value();
    SECTION("Creation disabled") {
        project.batch.items[0].createMetaSidecar = false;
    }
    SECTION("Provider does not support metadata") {
        project.batch.items[0].supportsMetaSidecar = false;
    }
    REQUIRE(project.committer.Commit(project.batch, project.ids, {}).HasValue());
    REQUIRE(project.committer.LastReceipt().status == Application::ProjectAssetImportStatus::Published);
    REQUIRE(std::filesystem::exists(project.Payload()));
    REQUIRE_FALSE(std::filesystem::exists(project.Sidecar()));
    REQUIRE(project.registry.Snapshot().Records().empty());
}

TEST_CASE("Terrain importer refuses other extensions invalid bytes and unsupported retained settings", "[terrain][import]") {
    Project project;
    auto contribution = Editor::TerrainRawImporterContribution();
    std::vector<std::uint8_t> bytes(9 * 9 * 4);
    const std::vector<ImportSettingValue> settings{std::int64_t{9}, std::int64_t{9}, 1.0, 1.0, 0.0, 0.0, std::string{}};
    REQUIRE(contribution.strategy->Import({bytes, "raw", settings, {}}, {}).HasError());
    bytes.pop_back();
    REQUIRE(contribution.strategy->Import({bytes, "terrainraw", settings, {}}, {}).HasError());
    REQUIRE(project.committer.Commit(project.batch, project.ids, {}).HasValue());
    auto metadata = ReadAssetImportMetadata(project.Sidecar()).Value();
    metadata.importSettings.emplace("settings.unknown-channel", "weights");
    auto capture = CaptureAssetSource(project.directory.Path(), project.Payload(), 4096).Value();
    REQUIRE(Editor::DecodeTerrainAsset(metadata, capture.bytes, Terrain::TerrainSourceRevision::Create(1).Value()).HasError());
}
