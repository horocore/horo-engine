#include "Horo/Assets/AssetImportOperation.h"
#include "Horo/Assets/AssetImporter.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/JobSystem.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Assets;

    class ScopedTempDirectory final {
    public:
        explicit ScopedTempDirectory(const std::string_view label)
            : path_{std::filesystem::temp_directory_path() /  // NOSONAR(cpp:S5443) Unique test-only directory; no untrusted input.
                    std::format("{}-{}", label, std::chrono::steady_clock::now().time_since_epoch().count())} {
            std::filesystem::create_directories(path_);
        }

        ~ScopedTempDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }

        ScopedTempDirectory(const ScopedTempDirectory &) = delete;
        ScopedTempDirectory &operator=(const ScopedTempDirectory &) = delete;

        [[nodiscard]] const std::filesystem::path &Path() const noexcept {
            return path_;
        }

    private:
        std::filesystem::path path_;
    };

    class TestImporter final : public IAssetImporter {
    public:
        [[nodiscard]] Result<PreparedAssetImport> Import(const AssetImportInput &input,
                                                         const CancellationToken & /*cancellation*/) const override {
            PreparedAssetImport result;
            result.type = AssetTypeId::Parse("core.mesh").Value();
            result.editorPayload.assign(input.sourceBytes.begin(), input.sourceBytes.end());
            return Result<PreparedAssetImport>::Success(std::move(result));
        }
    };

    class SettingsCapturingImporter final : public IAssetImporter {
    public:
        [[nodiscard]] Result<PreparedAssetImport> Import(const AssetImportInput &input,
                                                         const CancellationToken & /*cancellation*/) const override {
            receivedSettings = input.settings;
            PreparedAssetImport result;
            result.type = AssetTypeId::Parse("core.mesh").Value();
            return Result<PreparedAssetImport>::Success(std::move(result));
        }

        mutable std::vector<ImportSettingValue> receivedSettings;
    };

    class ReportingImporter final : public IAssetImporter {
    public:
        [[nodiscard]] Result<PreparedAssetImport> Import(const AssetImportInput &input,
                                                         const CancellationToken & /*cancellation*/) const override {
            input.progress.Report(2, 3, "decode");
            PreparedAssetImport result;
            result.type = AssetTypeId::Parse("core.mesh").Value();
            result.editorPayload.assign(input.sourceBytes.begin(), input.sourceBytes.end());
            result.diagnostics.push_back({
                .severity = ImportDiagnostic::Severity::Warning,
                .code = "asset.import.warning",
                .message = "source used a compatibility path",
            });
            return Result<PreparedAssetImport>::Success(std::move(result));
        }
    };

}  // namespace

TEST_CASE("AssetImportOperation Start enters Selecting phase", "[native]") {
    JobSystem jobs;

    AssetImporterCatalog catalog;
    REQUIRE((catalog
                 .Register(AssetImporterContribution{
                     .contributionId = "test.obj",
                     .packageId = "test",
                     .moduleId = "test",
                     .moduleVersion = "1.0.0",
                     .version = "1.0.0",
                     .fileExtensions = {"obj"},
                     .assetTypes = {AssetTypeId::Parse("core.mesh").Value()},
                     .strategy = std::make_shared<const TestImporter>(),
                 })
                 .HasValue()));
    auto catSnapshot = catalog.Publish();
    REQUIRE((catSnapshot.HasValue()));

    AssetImportOperation operation(jobs, catSnapshot.Value());

    AssetImportRequest request{
        .projectRoot = "test_project",
        .sourceFiles = {"test_project/assets/cube.obj"},
    };

    CancellationToken cancellation;
    auto result = operation.Start(request, cancellation);
    REQUIRE((result.HasValue()));

    const auto &snap = result.Value();
    REQUIRE((snap.phase == AssetImportPhase::Selecting));
    REQUIRE((snap.items.size() == 1));
    REQUIRE((snap.canCancel));
}

TEST_CASE("AssetImportOperation diagnostics for unsupported extension", "[native]") {
    JobSystem jobs;

    AssetImporterCatalog catalog;
    auto catSnapshot = catalog.Publish();
    REQUIRE((catSnapshot.HasValue()));

    AssetImportOperation operation(jobs, catSnapshot.Value());

    AssetImportRequest request{
        .projectRoot = "test_project",
        .sourceFiles = {"test_project/assets/unknown.xyz"},
    };

    CancellationToken cancellation;
    auto result = operation.Start(request, cancellation);
    REQUIRE((result.HasValue()));

    auto &item = result.Value().items[0];
    REQUIRE((!item.diagnostics.empty()));
    REQUIRE((item.diagnostics[0].code == "asset.import.no_importer"));

    const auto importResult = operation.ImportSingleItem(0, cancellation);
    REQUIRE(importResult.HasValue());
    CHECK(importResult.Value().phase == AssetImportPhase::Failed);
    CHECK_FALSE(importResult.Value().canCommit);
    CHECK_FALSE(importResult.Value().canCancel);
}

TEST_CASE("AssetImportOperation honours cancellation", "[native]") {
    JobSystem jobs;

    AssetImporterCatalog catalog;
    REQUIRE((catalog
                 .Register(AssetImporterContribution{
                     .contributionId = "test.obj",
                     .packageId = "test",
                     .moduleId = "test",
                     .moduleVersion = "1.0.0",
                     .version = "1.0.0",
                     .fileExtensions = {"obj"},
                     .assetTypes = {AssetTypeId::Parse("core.mesh").Value()},
                     .strategy = std::make_shared<const TestImporter>(),
                 })
                 .HasValue()));
    auto catSnapshot = catalog.Publish();
    REQUIRE((catSnapshot.HasValue()));

    AssetImportOperation operation(jobs, catSnapshot.Value());

    CancellationSource cancelSource;
    cancelSource.RequestCancellation();

    AssetImportRequest request{
        .projectRoot = "test_project",
        .sourceFiles = {"test_project/assets/cube.obj"},
    };

    auto result = operation.Start(request, cancelSource.Token());
    REQUIRE((result.HasError()));
}

TEST_CASE("AssetImportOperation resolves queued importer settings", "[native]") {
    const ScopedTempDirectory temp{"horo-asset-import-settings"};
    const auto sourceFile = temp.Path() / "settings.obj";
    {
        std::ofstream source{sourceFile};
        source << "o settings";
    }

    JobSystem jobs;
    auto importer = std::make_shared<SettingsCapturingImporter>();
    AssetImporterCatalog catalog;
    REQUIRE((catalog
                 .Register(AssetImporterContribution{
                     .contributionId = "test.obj.settings",
                     .packageId = "test",
                     .moduleId = "test",
                     .moduleVersion = "1.0.0",
                     .version = "1.0.0",
                     .fileExtensions = {"obj"},
                     .assetTypes = {AssetTypeId::Parse("core.mesh").Value()},
                     .settings =
                         {
                             ImportSettingDescriptor{
                                 .id = "optimize",
                                 .labelKey = "Optimize",
                                 .descriptionKey = "",
                                 .kind = ImportSettingKind::Boolean,
                                 .defaultValue = false,
                             },
                             ImportSettingDescriptor{
                                 .id = "quality",
                                 .labelKey = "Quality",
                                 .descriptionKey = "",
                                 .kind = ImportSettingKind::Integer,
                                 .defaultValue = std::int64_t{1},
                             },
                         },
                     .strategy = importer,
                 })
                 .HasValue()));
    auto snapshot = catalog.Publish();
    REQUIRE((snapshot.HasValue()));

    AssetImportOperation operation(jobs, snapshot.Value());
    CancellationToken cancellation;
    auto start = operation.Start(
        AssetImportRequest{
            .projectRoot = sourceFile.parent_path(),
            .sourceFiles = {sourceFile},
        },
        cancellation);
    REQUIRE((start.HasValue()));
    REQUIRE((start.Value().items[0].displayName == sourceFile.stem().string()));
    REQUIRE((start.Value().items[0].settings.at("settings.optimize") == "false"));
    REQUIRE((start.Value().items[0].settings.at("settings.quality") == "1"));

    auto configured = operation.SetItemSettings(0, {{"settings.optimize", "true"}, {"settings.quality", "7"}});
    REQUIRE((configured.HasValue()));
    REQUIRE((operation.ImportSingleItem(0, cancellation).HasValue()));
    REQUIRE((importer->receivedSettings.size() == 2));
    REQUIRE((std::get<bool>(importer->receivedSettings[0])));
    REQUIRE((std::get<std::int64_t>(importer->receivedSettings[1]) == 7));
}

TEST_CASE("AssetImportOperation projects importer progress diagnostics and terminal readiness", "[native][assets][importer]") {
    const ScopedTempDirectory temp{"horo-asset-import-progress"};
    const auto sourceFile = temp.Path() / "progress.raw";
    {
        std::ofstream source{sourceFile, std::ios::binary};
        source << "source";
    }

    JobSystem jobs;
    AssetImporterCatalog catalog;
    REQUIRE((catalog
                 .Register(AssetImporterContribution{
                     .contributionId = "test.raw.reporting",
                     .packageId = "test",
                     .moduleId = "test",
                     .moduleVersion = "1.0.0",
                     .version = "1.0.0",
                     .fileExtensions = {"raw"},
                     .assetTypes = {AssetTypeId::Parse("core.mesh").Value()},
                     .strategy = std::make_shared<const ReportingImporter>(),
                 })
                 .HasValue()));
    auto published = catalog.Publish();
    REQUIRE(published.HasValue());

    AssetImportOperation operation{jobs, published.Value()};
    CancellationToken cancellation;
    REQUIRE(operation
                .Start(
                    AssetImportRequest{
                        .projectRoot = sourceFile.parent_path(),
                        .sourceFiles = {sourceFile},
                    },
                    cancellation)
                .HasValue());
    const auto imported = operation.ImportSingleItem(0, cancellation);
    REQUIRE(imported.HasValue());
    CHECK(imported.Value().phase == AssetImportPhase::ReadyToCommit);
    CHECK(imported.Value().canCommit);
    CHECK_FALSE(imported.Value().canCancel);
    const AssetImportItem &item = imported.Value().items.front();
    CHECK(item.progressCompletedUnits == 2);
    CHECK(item.progressTotalUnits == 3);
    CHECK(item.progressMessage == "decode");
    REQUIRE(item.diagnostics.size() == 1);
    CHECK(item.diagnostics.front().code == "asset.import.warning");

    std::error_code error;
    std::filesystem::remove(sourceFile, error);
}
