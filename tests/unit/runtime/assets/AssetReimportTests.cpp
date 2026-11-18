#include "Horo/Assets/AssetImportMetadata.h"
#include "Horo/Assets/AssetReimport.h"
#include "Horo/Foundation/Platform.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>

namespace {
    using namespace Horo;
    using namespace Horo::Assets;

    class VersionedTestImporter final : public IAssetImporter {
    public:
        [[nodiscard]] Result<PreparedAssetImport> Import(const AssetImportInput &input, const CancellationToken &) const override {
            PreparedAssetImport output{
                .type = AssetTypeId::Parse("core.mesh").Value(),
                .editorPayload = {'v', '2', ':'},
            };
            output.editorPayload.insert(output.editorPayload.end(), input.sourceBytes.begin(), input.sourceBytes.end());
            return Result<PreparedAssetImport>::Success(std::move(output));
        }
    };

    struct TemporaryProject {
        std::filesystem::path root = std::filesystem::temp_directory_path() /
                                     ("horo_reimport_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

        TemporaryProject() {
            std::filesystem::create_directories(root / "assets");
        }

        ~TemporaryProject() {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    };

    void WriteText(const std::filesystem::path &path, const std::string_view text) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
    }
}  // namespace

TEST_CASE("Asset reimport preserves identity and records all detected reasons", "[native][assets]") {
    TemporaryProject project;
    const auto source = project.root / std::filesystem::path{u8"\u0130stanbul source.obj"};
    const std::filesystem::path asset = project.root / "assets/model.horoasset";
    const std::filesystem::path sidecar = asset.string() + ".horo";
    WriteText(source, "old");
    WriteText(asset, "old-payload");

    const AssetId id = AssetId::Parse("00112233-4455-6677-8899-aabbccddeeff").Value();
    auto oldBytes = ReadAssetImportSource(source);
    REQUIRE(oldBytes.HasValue());
    AssetImportMetadata metadata{
        .assetId = id,
        .assetType = AssetTypeId::Parse("core.mesh").Value(),
        .importerContributionId = "test.obj",
        .importerVersion = "1.0.0",
        .importerPackageId = "test.package",
        .importerModuleId = "test.module",
        .importerModuleVersion = "1.0.0",
        .absoluteSourcePath = std::filesystem::absolute(source),
        .sourceExtension = "obj",
        .sourceHash = HashAssetImportSource(oldBytes.Value()),
        .sourceByteSize = oldBytes.Value().size(),
        .importSettings = {},
        .lastImportReasons = {AssetImportReason::InitialImport},
        .importedAtUtc = CurrentImportTimestampUtc(),
    };
    auto serialized = SerializeAssetImportMetadata(metadata);
    REQUIRE(serialized.HasValue());
    WriteText(sidecar, serialized.Value());

    WriteText(source, "new-source");

    AssetImporterCatalog catalog;
    REQUIRE(catalog
                .Register(AssetImporterContribution{
                    .contributionId = "test.obj",
                    .packageId = "test.package",
                    .moduleId = "test.module",
                    .moduleVersion = "2.0.0",
                    .version = "1.1.0",
                    .fileExtensions = {"obj"},
                    .assetTypes = {AssetTypeId::Parse("core.mesh").Value()},
                    .strategy = std::make_shared<const VersionedTestImporter>(),
                })
                .HasValue());
    auto snapshot = catalog.Publish();
    REQUIRE(snapshot.HasValue());

    AssetRegistry registry;
    auto initialRegistry = RebuildAssetRegistry(registry, project.root, AssetRegistryOpenMode::Edit);
    REQUIRE(initialRegistry.HasValue());

    NativeDurableFileSystem files;
    auto reimported = ReimportProjectAsset(
        AssetReimportRequest{
            .absoluteProjectRoot = std::filesystem::absolute(project.root),
            .absoluteAssetPath = std::filesystem::absolute(asset),
            .importerCatalog = snapshot.Value().get(),
            .registry = &registry,
            .files = &files,
        },
        CancellationToken{});
    REQUIRE(reimported.HasValue());
    REQUIRE(reimported.Value().assetId == id);
    REQUIRE(reimported.Value().reasons.size() == 3);
    REQUIRE(reimported.Value().reasons[0] == AssetImportReason::SourceChanged);
    REQUIRE(reimported.Value().reasons[1] == AssetImportReason::ImporterChanged);
    REQUIRE(reimported.Value().reasons[2] == AssetImportReason::ModuleChanged);

    auto updated = ReadAssetImportMetadata(std::filesystem::absolute(sidecar));
    REQUIRE(updated.HasValue());
    REQUIRE(updated.Value().assetId == id);
    CHECK(updated.Value().absoluteSourcePath == metadata.absoluteSourcePath);
    REQUIRE(updated.Value().importerVersion == "1.1.0");
    REQUIRE(updated.Value().importerModuleVersion == "2.0.0");
    REQUIRE(updated.Value().sourceHash == reimported.Value().sourceHash);
    REQUIRE(registry.Snapshot().Find(id) != nullptr);

    std::ifstream payload(asset, std::ios::binary);
    const std::string contents{std::istreambuf_iterator<char>{payload}, std::istreambuf_iterator<char>{}};
    REQUIRE(contents == "v2:new-source");
}
