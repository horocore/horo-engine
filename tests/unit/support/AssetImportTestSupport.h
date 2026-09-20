#pragma once

#include "Horo/Assets/AssetImporter.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <string_view>

namespace Horo::Tests {
    [[nodiscard]] inline Assets::PreparedAssetImport MakeBasicPreparedAssetImport(const Assets::AssetImportInput &input) {
        Assets::PreparedAssetImport result;
        result.type = Assets::AssetTypeId::Parse("core.mesh").Value();
        result.editorPayload.assign(input.sourceBytes.begin(), input.sourceBytes.end());
        return result;
    }

    class ScopedAssetImportTempDirectory final {
    public:
        explicit ScopedAssetImportTempDirectory(const std::string_view label)
            : path_{std::filesystem::temp_directory_path() /  // NOSONAR(cpp:S5443) Unique test-only directory; no untrusted input.
                    std::format("{}-{}", label, std::chrono::steady_clock::now().time_since_epoch().count())} {
            std::filesystem::create_directories(path_);
        }

        ~ScopedAssetImportTempDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }

        ScopedAssetImportTempDirectory(const ScopedAssetImportTempDirectory &) = delete;
        ScopedAssetImportTempDirectory &operator=(const ScopedAssetImportTempDirectory &) = delete;

        [[nodiscard]] const std::filesystem::path &Path() const noexcept {
            return path_;
        }

    private:
        std::filesystem::path path_;
    };

    class BasicAssetImporter final : public Assets::IAssetImporter {
    public:
        [[nodiscard]] Result<Assets::PreparedAssetImport> Import(const Assets::AssetImportInput &input,
                                                                 const CancellationToken & /*cancellation*/) const override {
            return Result<Assets::PreparedAssetImport>::Success(MakeBasicPreparedAssetImport(input));
        }
    };

    [[nodiscard]] inline Assets::AssetImporterContribution BasicAssetImporterContribution() {
        return Assets::AssetImporterContribution{
            .contributionId = "test.obj",
            .packageId = "test",
            .moduleId = "test",
            .moduleVersion = "1.0.0",
            .version = "1.0.0",
            .fileExtensions = {"obj"},
            .assetTypes = {Assets::AssetTypeId::Parse("core.mesh").Value()},
            .strategy = std::make_shared<const BasicAssetImporter>(),
        };
    }

    [[nodiscard]] inline std::shared_ptr<const Assets::AssetImporterCatalogSnapshot> PublishAssetImporterCatalog(
        Assets::AssetImporterContribution contribution) {
        Assets::AssetImporterCatalog catalog;
        if (catalog.Register(std::move(contribution)).HasError())
            return {};
        auto published = catalog.Publish();
        return published.HasValue() ? std::move(published).Value() : nullptr;
    }
}  // namespace Horo::Tests
