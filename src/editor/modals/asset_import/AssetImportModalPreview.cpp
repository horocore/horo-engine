/** @file AssetImportModalPreview.cpp
 * @brief Inert representative Asset Import state for the editor UI preview entry point.
 */

#include "Horo/Assets/AssetImporter.h"
#include "Horo/Editor/AssetImportModal.h"

#include <array>

namespace Horo::Editor {
    namespace {
        struct PreviewFile {
            std::string_view name;
            std::string_view extension;
            std::uintmax_t size;
        };

        constexpr std::array PreviewFiles{
            PreviewFile{"hero.fbx", "fbx", 12'400'000},
            PreviewFile{"hero_albedo.png", "png", 2'100'000},
            PreviewFile{"hero_normal.png", "png", 2'000'000},
            PreviewFile{"hero_theme.wav", "wav", 4'800'000},
        };
    }  // namespace

    /** @copydoc AssetImportModal::LoadUiPreviewFixture */
    void AssetImportModal::LoadUiPreviewFixture(const UiPreviewFixture fixture) {
        m_uiPreviewMode = true;
        m_projectRoot = "Game";
        m_defaultDestinationFolder = "assets/Characters/Hero";
        m_snapshot = Assets::AssetImportSnapshot{
            .operationId = "ui-preview.asset-import",
            .phase = Assets::AssetImportPhase::Selecting,
            .selectedItemIndex = 0,
        };
        m_itemCompleted.clear();
        m_includedItems.clear();
        m_activePresetNames.clear();
        m_sourceFileSizes.clear();
        m_defaultPresetValues.clear();
        m_sourceFileSizes.reserve(PreviewFiles.size());
        m_defaultPresetValues.reserve(PreviewFiles.size());

        if (fixture == UiPreviewFixture::Empty)
            return;

        for (const auto &file : PreviewFiles)
            AppendUiPreviewFile(file.name, file.extension, file.size);
        FinalizeUiPreviewFixture();
    }

    void AssetImportModal::AppendUiPreviewFile(const std::string_view name, const std::string_view extension, const std::uintmax_t size) {
        const auto source = std::string{"assets/Characters/Hero/"} + std::string{name};
        Assets::AssetImportItem item{
            .sourceFile = ProjectPath::Parse(source).Value(),
            .absoluteSourcePath = source,
            .sourceExtension = extension,
            .displayName = std::filesystem::path{std::string{name}}.stem().string(),
            .destinationFolder = m_defaultDestinationFolder,
            .sourceByteSize = size,
        };
        if (const auto *contribution = m_catalog->FindContributionByExtension(extension)) {
            item.importerContributionId = contribution->contributionId;
            item.importerVersion = contribution->version;
        }
        if (extension == "fbx") {
            item.settings["settings.importMaterials"] = "true";
            item.settings["settings.importAnimations"] = "true";
        }
        if (extension == "wav")
            item.diagnostics.push_back(Assets::ImportDiagnostic{
                .severity = Assets::ImportDiagnostic::Severity::Warning,
                .code = "ui-preview.sample-rate",
                .message = "Audio sample rate will be converted.",
            });
        m_snapshot.items.push_back(std::move(item));
        m_sourceFileSizes.push_back(size);
    }

    void AssetImportModal::FinalizeUiPreviewFixture() {
        m_itemCompleted.assign(m_snapshot.items.size(), false);
        m_includedItems.assign(m_snapshot.items.size(), true);
        m_activePresetNames.assign(m_snapshot.items.size(), "Default");
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
