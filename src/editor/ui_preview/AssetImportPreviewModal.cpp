/** @file AssetImportPreviewModal.cpp
 * @brief In-memory Asset Import scenarios kept outside the production workflow.
 */

#include "AssetImportPreviewModal.h"

#include "EditorUiPreviewCatalog.h"
#include "Horo/Assets/AssetImporter.h"

#include <array>

namespace Horo::Editor {
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
        struct PreviewFile {
            const char *name;
            const char *extension;
            std::uintmax_t size;
        };

        static constexpr std::array files{
            PreviewFile{"hero.fbx", "fbx", 12'400'000},
            PreviewFile{"hero_albedo.png", "png", 2'100'000},
            PreviewFile{"hero_normal.png", "png", 2'000'000},
            PreviewFile{"hero_theme.wav", "wav", 4'800'000},
        };
        static constexpr PreviewFile unsupportedFile{"free_1975_porsche_911_930_turbo.glb", "glb", 70'700'000};
        const std::size_t fileCount = scenario_ == AssetImportPreviewScenario::Unsupported ? 1 : files.size();

        const bool advancedOpen = scenario_ == AssetImportPreviewScenario::Advanced || scenario_ == AssetImportPreviewScenario::Unsupported;
        std::string destination = scenario_ == AssetImportPreviewScenario::Unsupported ? "Assets/Scenes" : "Assets/Characters/Hero";
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
            const PreviewFile file = scenario_ == AssetImportPreviewScenario::Unsupported ? unsupportedFile : files[index];
            const auto source = destination + "/" + file.name;
            Assets::AssetImportItem item{
                .sourceFile = ProjectPath::Parse(source).Value(),
                .absoluteSourcePath = source,
                .sourceExtension = file.extension,
                .displayName = std::filesystem::path{file.name}.stem().string(),
                .destinationFolder = destination,
                .sourceByteSize = file.size,
            };
            if (const auto *contribution = Catalog().FindContributionByExtension(file.extension)) {
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
            if (scenario_ == AssetImportPreviewScenario::Unsupported) {
                item.diagnostics.push_back(Assets::ImportDiagnostic{
                    .severity = Assets::ImportDiagnostic::Severity::Error,
                    .code = "ui-preview.no-importer",
                    .message = "No importer registered for extension .glb",
                });
            }
            snapshot.items.push_back(std::move(item));
            sourceFileSizes.push_back(file.size);
        }

        PresentReadOnlySnapshot(std::move(snapshot), std::move(sourceFileSizes), "Game", std::move(destination),
                                {EditorUiPreviewSidebarWidth, EditorUiPreviewHeaderHeight}, advancedOpen);
    }
}  // namespace Horo::Editor
