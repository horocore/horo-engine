/** @file AssetImportModalPreview.cpp
 * @brief Inert representative Asset Import state for the editor UI preview entry point.
 */

#include "Horo/Assets/AssetImporter.h"
#include "Horo/Editor/AssetImportModal.h"

#include <array>

namespace Horo::Editor {
    /** @copydoc AssetImportModal::LoadUiPreviewFixture */
    void AssetImportModal::LoadUiPreviewFixture(const UiPreviewFixture fixture) {
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

        m_uiPreviewMode = true;
        m_projectRoot = "Game";
        m_defaultDestinationFolder = "assets/Characters/Hero";
        m_snapshot = Assets::AssetImportSnapshot{
            .operationId = "ui-preview.asset-import",
            .phase = Assets::AssetImportPhase::Selecting,
            .selectedItemIndex = 0,
        };
        m_sourceFileSizes.clear();
        m_defaultPresetValues.clear();
        m_sourceFileSizes.reserve(files.size());
        m_defaultPresetValues.reserve(files.size());

        if (fixture == UiPreviewFixture::Empty)
            return;

        for (const auto &file : files) {
            const auto source = std::string{"assets/Characters/Hero/"} + file.name;
            Assets::AssetImportItem item{
                .sourceFile = ProjectPath::Parse(source).Value(),
                .absoluteSourcePath = source,
                .sourceExtension = file.extension,
                .displayName = std::filesystem::path{file.name}.stem().string(),
                .destinationFolder = m_defaultDestinationFolder,
                .sourceByteSize = file.size,
            };
            if (const auto *contribution = m_catalog->FindContributionByExtension(file.extension)) {
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
            m_snapshot.items.push_back(std::move(item));
            m_sourceFileSizes.push_back(file.size);
        }

        m_itemCompleted.assign(files.size(), false);
        m_includedItems.assign(files.size(), true);
        m_activePresetNames.assign(files.size(), "Default");
        for (const auto &item : m_snapshot.items) {
            m_defaultPresetValues.push_back(ImportPreset{
                .name = "Default",
                .destinationFolder = item.destinationFolder,
                .subfolderByType = item.subfolderByType,
                .assetIdStrategy = item.assetIdStrategy,
                .createMetaSidecar = item.createMetaSidecar,
                .overwriteWithoutPrompt = item.overwriteWithoutPrompt,
            });
        }
    }
}  // namespace Horo::Editor
