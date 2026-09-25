/** @file AssetImportPreviewModal.cpp
 * @brief In-memory Asset Import scenarios kept outside the production workflow.
 */

#include "AssetImportPreviewModal.h"

#include "EditorUiPreviewCatalog.h"
#include "Horo/Assets/AssetImporter.h"

#include <array>

namespace Horo::Editor {
    namespace {
        struct PreviewFile final {
            const char *name;
            const char *extension;
            std::uintmax_t size;
        };

        constexpr std::array PreviewFiles{
            PreviewFile{"hero.fbx", "fbx", 12'400'000},
            PreviewFile{"hero_albedo.png", "png", 2'100'000},
            PreviewFile{"hero_normal.png", "png", 2'000'000},
            PreviewFile{"hero_theme.wav", "wav", 4'800'000},
        };
        constexpr PreviewFile UnsupportedPreviewFile{"free_1975_porsche_911_930_turbo.glb", "glb", 70'700'000};

        /** @brief Builds one read-only fixture item without touching the import workflow. */
        [[nodiscard]] Assets::AssetImportItem MakePreviewItem(const PreviewFile &file, const std::string &destination,
                                                              const Assets::AssetImporterCatalogSnapshot &catalog, const bool unsupported) {
            const auto source = destination + "/" + file.name;
            Assets::AssetImportItem item{
                .sourceFile = ProjectPath::Parse(source).Value(),
                .absoluteSourcePath = source,
                .sourceExtension = file.extension,
                .displayName = std::filesystem::path{file.name}.stem().string(),
                .destinationFolder = destination,
                .sourceByteSize = file.size,
            };
            if (const auto *contribution = catalog.FindContributionByExtension(file.extension)) {
                item.importerContributionId = contribution->contributionId;
                item.importerVersion = contribution->version;
            }
            if (item.sourceExtension == "fbx") {
                item.settings["settings.importMaterials"] = "true";
                item.settings["settings.importAnimations"] = "true";
            }
            if (item.sourceExtension == "wav") {
                item.diagnostics.push_back(Assets::ImportDiagnostic{
                    .severity = Assets::ImportDiagnostic::Severity::Warning,
                    .code = "ui-preview.sample-rate",
                    .message = "Audio sample rate will be converted.",
                });
            }
            if (unsupported) {
                item.diagnostics.push_back(Assets::ImportDiagnostic{
                    .severity = Assets::ImportDiagnostic::Severity::Error,
                    .code = "ui-preview.no-importer",
                    .message = "No importer registered for extension .glb",
                });
            }
            return item;
        }
    }  // namespace

    void AssetImportPreviewModal::SetScenario(const AssetImportPreviewScenario scenario) noexcept {
        scenario_ = scenario;
    }

    Result<void> AssetImportPreviewModal::OnOpen(EditorModalContext &context) {
        if (Result<void> opened = AssetImportModal::OnOpen(context); opened.HasError())
            return opened;
        PopulateScenario();
        return Result<void>::Success();
    }

    void AssetImportPreviewModal::PopulateScenario() {
        const bool unsupported = scenario_ == AssetImportPreviewScenario::Unsupported;
        const std::size_t fileCount = unsupported ? 1 : PreviewFiles.size();

        const bool advancedOpen = scenario_ == AssetImportPreviewScenario::Advanced || unsupported;
        std::string destination = unsupported ? "Assets/Scenes" : "Assets/Characters/Hero";
        Assets::AssetImportSnapshot snapshot{
            .operationId = "ui-preview.asset-import",
            .phase = Assets::AssetImportPhase::Selecting,
            .selectedItemIndex = 0,
        };
        std::vector<std::optional<std::uintmax_t>> sourceFileSizes;
        sourceFileSizes.reserve(fileCount);

        if (scenario_ == AssetImportPreviewScenario::Empty) {
            PresentReadOnlySnapshot(std::move(snapshot), std::move(sourceFileSizes), "Game", "Assets",
                                    {EditorUiPreviewSidebarWidth, EditorUiPreviewHeaderHeight}, advancedOpen);
            return;
        }

        for (std::size_t index = 0; index < fileCount; ++index) {
            const PreviewFile &file = unsupported ? UnsupportedPreviewFile : PreviewFiles[index];
            snapshot.items.push_back(MakePreviewItem(file, destination, Catalog(), unsupported));
            sourceFileSizes.push_back(file.size);
        }

        PresentReadOnlySnapshot(std::move(snapshot), std::move(sourceFileSizes), "Game", std::move(destination),
                                {EditorUiPreviewSidebarWidth, EditorUiPreviewHeaderHeight}, advancedOpen);
    }
}  // namespace Horo::Editor
