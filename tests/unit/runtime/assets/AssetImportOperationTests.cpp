#include "../../support/AssetImportTestSupport.h"
#include "Horo/Assets/AssetImportOperation.h"
#include "Horo/Assets/AssetImporter.h"
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Foundation/JobSystem.h"

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Assets;

    using ScopedTempDirectory = Tests::ScopedAssetImportTempDirectory;

    [[nodiscard]] AssetImporterContribution BasicContribution() {
        return Tests::BasicAssetImporterContribution();
    }

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
            auto result = Tests::MakeBasicPreparedAssetImport(input);
            result.diagnostics.push_back({
                .severity = ImportDiagnostic::Severity::Warning,
                .code = "asset.import.warning",
                .message = "source used a compatibility path",
            });
            return Result<PreparedAssetImport>::Success(std::move(result));
        }
    };

    class BlockingReportingImporter final : public IAssetImporter {
    public:
        [[nodiscard]] Result<PreparedAssetImport> Import(const AssetImportInput &input,
                                                         const CancellationToken & /*cancellation*/) const override {
            input.progress.Report(1, 2, "blocked");
            {
                std::unique_lock lock{mutex_};
                providerEntered_ = true;
                changed_.notify_all();
                changed_.wait(lock, [this] {
                    return released_;
                });
            }
            return Result<PreparedAssetImport>::Success(Tests::MakeBasicPreparedAssetImport(input));
        }

        void WaitUntilProviderEntered() const {
            std::unique_lock lock{mutex_};
            changed_.wait(lock, [this] {
                return providerEntered_;
            });
        }

        void Release() {
            const std::scoped_lock lock{mutex_};
            released_ = true;
            changed_.notify_all();
        }

    private:
        mutable std::mutex mutex_;
        mutable std::condition_variable changed_;
        mutable bool providerEntered_{};
        bool released_{};
    };

    [[nodiscard]] AssetImporterContribution SettingsContribution(const std::shared_ptr<SettingsCapturingImporter> &importer) {
        return AssetImporterContribution{
            .contributionId = "test.obj.settings",
            .packageId = "test",
            .moduleId = "test",
            .moduleVersion = "1.0.0",
            .version = "1.0.0",
            .fileExtensions = {"obj"},
            .assetTypes = {AssetTypeId::Parse("core.mesh").Value()},
            .settings =
                {
                    ImportSettingDescriptor{.id = "optimize",
                                            .labelKey = "Optimize",
                                            .descriptionKey = "",
                                            .kind = ImportSettingKind::Boolean,
                                            .defaultValue = false},
                    ImportSettingDescriptor{.id = "quality",
                                            .labelKey = "Quality",
                                            .descriptionKey = "",
                                            .kind = ImportSettingKind::Integer,
                                            .defaultValue = std::int64_t{1}},
                },
            .strategy = importer,
        };
    }

}  // namespace

TEST_CASE("AssetImportOperation Start enters Selecting phase", "[native]") {
    JobSystem jobs;

    const auto catSnapshot = Tests::PublishAssetImporterCatalog(BasicContribution());
    REQUIRE(catSnapshot != nullptr);
    AssetImportOperation operation(jobs, catSnapshot);

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

    const auto catSnapshot = Tests::PublishAssetImporterCatalog(BasicContribution());
    REQUIRE(catSnapshot != nullptr);
    AssetImportOperation operation(jobs, catSnapshot);

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
    REQUIRE(catalog.Register(SettingsContribution(importer)).HasValue());
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

TEST_CASE("AssetImportOperation publishes worker progress and cancellation without data races", "[native][assets][importer]") {
    const ScopedTempDirectory temp{"horo-asset-import-concurrency"};
    const auto sourceFile = temp.Path() / "concurrent.raw";
    {
        std::ofstream source{sourceFile, std::ios::binary};
        source << "source";
    }

    JobSystem jobs;
    auto importer = std::make_shared<BlockingReportingImporter>();
    AssetImporterCatalog catalog;
    REQUIRE(catalog
                .Register(AssetImporterContribution{
                    .contributionId = "test.raw.concurrent",
                    .packageId = "test",
                    .moduleId = "test",
                    .moduleVersion = "1.0.0",
                    .version = "1.0.0",
                    .fileExtensions = {"raw"},
                    .assetTypes = {AssetTypeId::Parse("core.mesh").Value()},
                    .strategy = importer,
                })
                .HasValue());
    auto published = catalog.Publish();
    REQUIRE(published.HasValue());

    AssetImportOperation operation{jobs, published.Value()};
    CancellationToken cancellation;
    REQUIRE(operation.Start({.projectRoot = temp.Path(), .sourceFiles = {sourceFile}}, cancellation).HasValue());
    std::optional<Result<AssetImportSnapshot>> importResult;
    std::thread worker{[&] {
        importResult = operation.ImportSingleItem(0, cancellation);
    }};

    importer->WaitUntilProviderEntered();
    const auto progress = operation.Snapshot();
    CHECK(progress.items.front().progressCompletedUnits == 1);
    CHECK(progress.items.front().progressTotalUnits == 2);
    CHECK(progress.items.front().progressMessage == "blocked");
    operation.Cancel();
    CHECK(operation.Snapshot().phase == AssetImportPhase::Cancelled);
    importer->Release();
    worker.join();
    REQUIRE(importResult.has_value());
    CHECK(importResult->HasError());
}
