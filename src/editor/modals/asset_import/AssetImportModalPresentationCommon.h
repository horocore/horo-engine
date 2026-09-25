#pragma once

/**
 * @file AssetImportModalPresentationCommon.h
 * @brief Private drawing constants and formatting helpers shared by the Asset Import panels.
 */

#include "Horo/Editor/AssetImportModal.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"

#include <algorithm>
#include <format>
#include <imgui.h>
#include <string>
#include <string_view>

namespace Horo::Editor::AssetImportPresentationDetail {
    using namespace Theme;
    using namespace Ui;

    constexpr ImVec4 WarningColor{0.91f, 0.64f, 0.24f, 1.0f};
    constexpr ImVec4 ErrorColor{0.83f, 0.32f, 0.29f, 1.0f};
    constexpr float PanelGap = 14.0f;
    constexpr float PanelPadding = 16.0f;
    constexpr float ImportFieldGap = 8.0f;
    constexpr float ImportSectionGap = 12.0f;

    inline void BeginImportField(const char *label, const Fonts &fonts) {
        FieldLabel(label, fonts);
        ImGui::SetNextItemWidth(-1.0f);
    }

    inline void EndImportField() {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImportFieldGap);
    }

    [[nodiscard]] inline std::string Copy(std::string_view text) {
        return std::string{text};
    }

    [[nodiscard]] inline std::string FileName(const Assets::AssetImportItem &item) {
        const auto name = item.absoluteSourcePath.filename().string();
        return name.empty() ? item.displayName : name;
    }

    [[nodiscard]] inline std::string FitFileName(const std::string &name, const float width) {
        if (width <= 0.0f)
            return {};
        if (ImGui::CalcTextSize(name.c_str()).x <= width)
            return name;
        constexpr std::string_view ellipsis = "…";
        const float available = width - ImGui::CalcTextSize(ellipsis.data()).x;
        if (available <= 0.0f)
            return std::string{ellipsis};
        std::size_t length = name.size();
        while (length > 0) {
            --length;
            while (length > 0 && (static_cast<unsigned char>(name[length]) & 0xC0U) == 0x80U)
                --length;
            if (ImGui::CalcTextSize(name.c_str(), name.c_str() + length).x <= available)
                return name.substr(0, length) + std::string{ellipsis};
        }
        return std::string{ellipsis};
    }

    [[nodiscard]] inline std::string FileSize(const AssetImportModal &modal, std::size_t index) {
        const auto sourceSize = modal.SourceFileSize(index);
        if (!sourceSize)
            return "—";
        const float bytes = static_cast<float>(*sourceSize);
        if (bytes >= 1024.0f * 1024.0f)
            return std::format("{:.1f} MB", bytes / (1024.0f * 1024.0f));
        if (bytes >= 1024.0f)
            return std::format("{:.1f} KB", bytes / 1024.0f);
        return std::format("{} B", *sourceSize);
    }

    [[nodiscard]] inline Assets::AssetPreviewFallback PreviewKind(const AssetImportModal &modal, const std::size_t index) {
        const auto *importer = modal.ImporterFor(index);
        return importer ? importer->previewFallback : Assets::AssetPreviewFallback::Generic;
    }

    [[nodiscard]] inline std::string AssetKind(const AssetImportModal &modal, const std::size_t index) {
        switch (PreviewKind(modal, index)) {
            case Assets::AssetPreviewFallback::Mesh:
                return Copy(modal.Localized("asset_import.type.model", "3D Model"));
            case Assets::AssetPreviewFallback::Image:
                return Copy(modal.Localized("asset_import.type.texture", "Texture"));
            case Assets::AssetPreviewFallback::Audio:
                return Copy(modal.Localized("asset_import.type.audio", "Audio"));
            default:
                return Copy(modal.Localized("asset_import.type.asset", "Asset"));
        }
    }

    [[nodiscard]] inline UiIcon AssetIcon(const AssetImportModal &modal, const std::size_t index) {
        switch (PreviewKind(modal, index)) {
            case Assets::AssetPreviewFallback::Mesh:
                return UiIcon::HierarchyMesh;
            case Assets::AssetPreviewFallback::Image:
                return UiIcon::Image;
            case Assets::AssetPreviewFallback::Audio:
                return UiIcon::AudioFile;
            default:
                return UiIcon::Package;
        }
    }

    [[nodiscard]] inline bool HasDiagnostic(const Assets::AssetImportItem &item, Assets::ImportDiagnostic::Severity severity) {
        return std::ranges::any_of(item.diagnostics, [severity](const auto &diagnostic) {
            return diagnostic.severity == severity;
        });
    }

}  // namespace Horo::Editor::AssetImportPresentationDetail
