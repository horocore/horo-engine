/** @copydoc AssetImportModalPresentation.h */

#include "AssetImportModalPresentation.h"

#include "Horo/Editor/AssetImportModal.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <imgui.h>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Editor {
    namespace {
        using namespace Theme;
        using namespace Ui;

        constexpr ImVec4 WarningColor{0.91f, 0.64f, 0.24f, 1.0f};
        constexpr ImVec4 ErrorColor{0.83f, 0.32f, 0.29f, 1.0f};
        constexpr float PanelGap = 14.0f;
        constexpr float PanelPadding = 16.0f;

        [[nodiscard]] std::string Copy(std::string_view text) {
            return std::string{text};
        }

        [[nodiscard]] std::string FileName(const Assets::AssetImportItem &item) {
            const auto name = item.absoluteSourcePath.filename().string();
            return name.empty() ? item.displayName : name;
        }

        [[nodiscard]] std::string FitFileName(const std::string &name, const float width) {
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

        [[nodiscard]] std::string FileSize(const AssetImportModal &modal, std::size_t index) {
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

        [[nodiscard]] Assets::AssetPreviewFallback PreviewKind(const AssetImportModal &modal, const std::size_t index) {
            const auto *importer = modal.ImporterFor(index);
            return importer ? importer->previewFallback : Assets::AssetPreviewFallback::Generic;
        }

        [[nodiscard]] std::string AssetKind(const AssetImportModal &modal, const std::size_t index) {
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

        [[nodiscard]] UiIcon AssetIcon(const AssetImportModal &modal, const std::size_t index) {
            switch (PreviewKind(modal, index)) {
                case Assets::AssetPreviewFallback::Mesh: return UiIcon::HierarchyMesh;
                case Assets::AssetPreviewFallback::Image: return UiIcon::Image;
                case Assets::AssetPreviewFallback::Audio: return UiIcon::AudioFile;
                default: return UiIcon::Package;
            }
        }

        [[nodiscard]] bool HasDiagnostic(const Assets::AssetImportItem &item, Assets::ImportDiagnostic::Severity severity) {
            return std::ranges::any_of(item.diagnostics, [severity](const auto &diagnostic) {
                return diagnostic.severity == severity;
            });
        }

        [[nodiscard]] std::vector<std::filesystem::path> DroppedFiles(const ImGuiPayload *payload) {
            std::vector<std::filesystem::path> paths;
            if (!payload || !payload->Data)
                return paths;
            const std::string_view data{static_cast<const char *>(payload->Data), static_cast<std::size_t>(payload->DataSize)};
            for (std::size_t start = 0; start < data.size();) {
                const auto end = data.find('\n', start);
                auto path = data.substr(start, end == std::string_view::npos ? data.size() - start : end - start);
                while (!path.empty() && (path.back() == '\0' || path.back() == '\r'))
                    path.remove_suffix(1);
                if (!path.empty())
                    paths.emplace_back(path);
                if (end == std::string_view::npos)
                    break;
                start = end + 1;
            }
            return paths;
        }

        void DrawDestination(AssetImportModal &modal, const Assets::AssetImportSnapshot &snapshot, const Fonts &fonts) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Bg2());
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {PanelPadding, 0.0f});
            ImGui::BeginChild("ImportDestination", {0.0f, 54.0f}, true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            const auto label = Copy(modal.Localized("asset_import.destination", "Destination"));
            const ImVec2 barPosition = ImGui::GetWindowPos();
            const ImVec2 barSize = ImGui::GetWindowSize();
            const float centerY = barPosition.y + barSize.y * 0.5f;
            constexpr float buttonWidth = 104.0f;
            constexpr float buttonHeight = 32.0f;
            const float buttonX = barPosition.x + barSize.x - PanelPadding - buttonWidth;
            ImDrawList *const drawList = ImGui::GetWindowDrawList();
            float x = barPosition.x + PanelPadding;
            DrawEditorIcon(drawList, UiIcon::Folder, {x, centerY - 10.0f}, {20.0f, 20.0f},
                           ImGui::ColorConvertFloat4ToU32(Text()), fonts.icon);
            x += 30.0f;
            const auto drawText = [&](const std::string &value, const ImVec4 color) {
                ScopedTextStyle breadcrumbStyle(fonts.sansCompact, TextPx::Body(), FontPx::SansCompact);
                const ImVec2 size = ImGui::CalcTextSize(value.c_str());
                ImGui::SetCursorScreenPos({x, centerY - size.y * 0.5f});
                ImGui::TextColored(color, "%s", value.c_str());
                x += size.x;
            };
            drawText(label, Dim());
            x += 17.0f;
            drawList->AddLine({x, centerY - 12.0f}, {x, centerY + 12.0f}, ImGui::ColorConvertFloat4ToU32(Border()));
            x += 16.0f;
            const auto folder = snapshot.items.empty() || snapshot.selectedItemIndex >= snapshot.items.size()
                                    ? std::string{modal.DefaultDestinationFolder()}
                                    : snapshot.items[snapshot.selectedItemIndex].destinationFolder;
            const std::filesystem::path defaultDestination{modal.DefaultDestinationFolder()};
            const std::string assetRootName = defaultDestination.empty() ? "Assets" : defaultDestination.begin()->string();
            const std::string path = folder.empty() ? assetRootName : folder;
            const std::string projectName = modal.ProjectRoot().filename().string();
            ImGui::PushClipRect({x, barPosition.y}, {buttonX - 16.0f, barPosition.y + barSize.y}, true);
            if (!projectName.empty()) {
                DrawEditorIcon(drawList, UiIcon::AccountTree, {x, centerY - 9.0f}, {18.0f, 18.0f},
                               ImGui::ColorConvertFloat4ToU32(Muted()), fonts.icon);
                x += 25.0f;
                drawText(projectName, Text());
            }
            const std::filesystem::path breadcrumb{path};
            auto part = breadcrumb.begin();
            if (!projectName.empty() && part != breadcrumb.end() && *part == assetRootName && std::next(part) != breadcrumb.end())
                ++part;
            const auto firstVisiblePart = part;
            for (; part != breadcrumb.end(); ++part) {
                if (!projectName.empty() || part != firstVisiblePart) {
                    x += 9.0f;
                    drawText(">", Dim());
                    x += 9.0f;
                }
                drawText(part->string(), Text());
            }
            ImGui::PopClipRect();
            ImGui::SetCursorScreenPos({buttonX, centerY - buttonHeight * 0.5f});
            if (Button({.label = Copy(modal.Localized("asset_import.change", "Change...")).c_str(),
                        .size = {buttonWidth, buttonHeight},
                        .variant = ButtonVariant::Secondary,
                        .enabled = !modal.ProjectRoot().empty() && !modal.IsReadOnlyPresentation()})) {
                modal.BrowseDestination();
            }
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }

        void DrawDashedBorder(ImDrawList *drawList, const ImVec2 &min, const ImVec2 &max, ImU32 color) {
            constexpr float dash = 6.0f;
            constexpr float step = 11.0f;
            for (float x = min.x; x < max.x; x += step) {
                drawList->AddLine({x, min.y}, {std::min(x + dash, max.x), min.y}, color);
                drawList->AddLine({x, max.y}, {std::min(x + dash, max.x), max.y}, color);
            }
            for (float y = min.y; y < max.y; y += step) {
                drawList->AddLine({min.x, y}, {min.x, std::min(y + dash, max.y)}, color);
                drawList->AddLine({max.x, y}, {max.x, std::min(y + dash, max.y)}, color);
            }
        }

        void DrawDropZone(AssetImportModal &modal, const Fonts &fonts, float height) {
            const float width = ImGui::GetContentRegionAvail().x;
            const ImVec2 topLeft = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##ImportDropZone", {width, height}) && !modal.IsReadOnlyPresentation())
                modal.BrowseSourceFiles();
            ImDrawList *drawList = ImGui::GetWindowDrawList();
            const ImVec2 bottomRight{topLeft.x + width, topLeft.y + height};
            drawList->AddRectFilled(topLeft, bottomRight, ImGui::ColorConvertFloat4ToU32(Bg2()), 5.0f);
            DrawDashedBorder(drawList, topLeft, bottomRight, ImGui::ColorConvertFloat4ToU32(ImGui::IsItemHovered() ? Accent() : Border()));
            const auto title = Copy(modal.Localized("asset_import.add_files", "Add more files"));
            const auto hint = Copy(modal.Localized("asset_import.drop_hint", "Drag and drop files here or click to browse"));
            PushFont(fonts.sansCompact);
            const float titleWidth = ImGui::CalcTextSize(title.c_str()).x;
            const float hintWidth = ImGui::CalcTextSize(hint.c_str()).x;
            DrawEditorIcon(drawList, UiIcon::Create, {topLeft.x + (width - titleWidth) * 0.5f - 24.0f, topLeft.y + height * 0.5f - 20.0f},
                           {18.0f, 18.0f}, ImGui::ColorConvertFloat4ToU32(Accent()), fonts.icon);
            drawList->AddText({topLeft.x + (width - titleWidth) * 0.5f, topLeft.y + height * 0.5f - 20.0f},
                              ImGui::ColorConvertFloat4ToU32(Accent()), title.c_str());
            drawList->AddText({topLeft.x + (width - hintWidth) * 0.5f, topLeft.y + height * 0.5f + 4.0f},
                              ImGui::ColorConvertFloat4ToU32(Dim()), hint.c_str());
            PopFont(fonts.sansCompact);
            if (ImGui::BeginDragDropTarget()) {
                modal.AddSourceFiles(DroppedFiles(ImGui::AcceptDragDropPayload("FILES")));
                ImGui::EndDragDropTarget();
            }
        }

        void DrawFiles(AssetImportModal &modal, const Assets::AssetImportSnapshot &snapshot, const Fonts &fonts, float width,
                       float height) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Bg1());
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {PanelPadding, PanelPadding});
            ImGui::BeginChild("ImportFilePanel", {width, height}, true);
            const auto heading = Copy(modal.Localized("asset_import.list_title", "Files"));
            PushFont(fonts.sansEmphasis);
            ImGui::TextColored(Text(), "%s", heading.c_str());
            PopFont(fonts.sansEmphasis);
            PushFont(fonts.sansCompact);
            const auto count = std::format("{} {}", modal.VisibleItemCount(), Copy(modal.Localized("asset_import.files", "files")));
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(count.c_str()).x);
            ImGui::TextColored(Dim(), "%s", count.c_str());
            PopFont(fonts.sansCompact);
            ImGui::Dummy({0.0f, 7.0f});

            const float zoneHeight = 76.0f;
            const float listHeight =
                std::max(0.0f, ImGui::GetContentRegionAvail().y - zoneHeight - 7.0f - 2.0f * ImGui::GetStyle().ItemSpacing.y - 2.0f);
            const ImGuiWindowFlags listFlags =
                modal.VisibleItemCount() == 0 ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : ImGuiWindowFlags_None;
            ImGui::BeginChild("ImportFileList", {0.0f, listHeight}, false, listFlags);
            for (std::size_t index = 0; index < snapshot.items.size(); ++index) {
                if (!modal.IsItemVisible(index))
                    continue;
                const auto &item = snapshot.items[index];
                const bool selected = snapshot.selectedItemIndex == index;
                const ImVec2 rowMin = ImGui::GetCursorScreenPos();
                const float rowWidth = ImGui::GetContentRegionAvail().x;
                constexpr float rowHeight = 56.0f;
                constexpr float actionWidth = 94.0f;
                if (ImGui::InvisibleButton(std::format("##ImportFile{}", index).c_str(), {std::max(1.0f, rowWidth - actionWidth), rowHeight}))
                    modal.SelectItem(index);
                const ImVec2 rowMax{rowMin.x + rowWidth, rowMin.y + rowHeight};
                auto *drawList = ImGui::GetWindowDrawList();
                const ImVec4 background = selected ? ImVec4{Accent().x, Accent().y, Accent().z, 0.13f} : Bg2();
                drawList->AddRectFilled(rowMin, rowMax, ImGui::ColorConvertFloat4ToU32(background), 5.0f);
                drawList->AddRect(rowMin, rowMax, ImGui::ColorConvertFloat4ToU32(selected ? Accent() : Border()), 5.0f);
                drawList->AddRectFilled({rowMin.x + 9.0f, rowMin.y + 8.0f}, {rowMin.x + 49.0f, rowMin.y + 48.0f},
                                        ImGui::ColorConvertFloat4ToU32(Bg3()), 5.0f);
                DrawEditorIcon(drawList, AssetIcon(modal, index), {rowMin.x + 17.0f, rowMin.y + 16.0f}, {24.0f, 24.0f},
                               ImGui::ColorConvertFloat4ToU32(Text()), fonts.icon);
                const auto name = FileName(item);
                const auto details = std::format("{}  |  {}", AssetKind(modal, index), FileSize(modal, index));
                drawList->PushClipRect({rowMin.x + 64.0f, rowMin.y}, {rowMax.x - actionWidth, rowMax.y}, true);
                drawList->AddText({rowMin.x + 64.0f, rowMin.y + 8.0f}, ImGui::ColorConvertFloat4ToU32(Text()), name.c_str());
                drawList->AddText({rowMin.x + 64.0f, rowMin.y + 29.0f}, ImGui::ColorConvertFloat4ToU32(Dim()), details.c_str());
                drawList->PopClipRect();
                const float rowCenterY = rowMin.y + rowHeight * 0.5f;
                if (HasDiagnostic(item, Assets::ImportDiagnostic::Severity::Warning) ||
                    HasDiagnostic(item, Assets::ImportDiagnostic::Severity::Error)) {
                    const auto tone = HasDiagnostic(item, Assets::ImportDiagnostic::Severity::Error) ? ErrorColor : WarningColor;
                    DrawEditorIcon(drawList,
                                   HasDiagnostic(item, Assets::ImportDiagnostic::Severity::Error) ? UiIcon::Error : UiIcon::Warning,
                                   {rowMax.x - 86.0f, rowCenterY - 10.5f}, {21.0f, 21.0f}, ImGui::ColorConvertFloat4ToU32(tone), fonts.icon);
                }
                ImGui::SetCursorScreenPos({rowMax.x - 58.0f, rowCenterY - 10.0f});
                bool included = modal.IsItemIncluded(index);
                ImGui::BeginDisabled(item.result.has_value() || modal.IsReadOnlyPresentation());
                if (CheckboxControl(std::format("##IncludeImport{}", index).c_str(), &included, fonts, 20.0f))
                    modal.SetItemIncluded(index, included);
                ImGui::EndDisabled();
                ImGui::SetCursorScreenPos({rowMax.x - 29.0f, rowCenterY - 10.0f});
                ImGui::BeginDisabled(modal.IsReadOnlyPresentation() || modal.HasPendingConflicts());
                const bool remove = IconCloseButton(std::format("##RemoveImport{}", index).c_str(), {20.0f, 20.0f});
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered())
                    ShowTooltip(Copy(modal.Localized("asset_import.remove_file", "Remove from list")).c_str(), &fonts);
                if (remove)
                    modal.RemoveItem(index);
                ImGui::SetCursorScreenPos({rowMin.x, rowMax.y + 5.0f});
            }
            if (modal.VisibleItemCount() == 0) {
                const auto empty = Copy(modal.Localized("asset_import.no_files", "No files in queue."));
                const float textWidth = ImGui::CalcTextSize(empty.c_str()).x;
                const ImVec2 available = ImGui::GetContentRegionAvail();
                const ImVec2 origin = ImGui::GetCursorScreenPos();
                ImGui::SetCursorScreenPos({origin.x + (available.x - textWidth) * 0.5f, origin.y + std::max(20.0f, available.y * 0.48f)});
                ImGui::TextColored(Dim(), "%s", empty.c_str());
            }
            ImGui::EndChild();
            ImGui::Dummy({0.0f, 7.0f});
            DrawDropZone(modal, fonts, zoneHeight);
            ImGui::EndChild();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        }

        [[nodiscard]] bool DrawBooleanSettingRow(const AssetImportModal &modal, const std::string &label, const char *id,
                                                 bool *value, const Fonts &fonts) {
            const ImVec2 row = ImGui::GetCursorScreenPos();
            const float scale = GetActiveTokens().sizes.uiScale;
            const float rowHeight = 30.0f * scale;
            const float toggleHeight = 20.0f * scale;
            const float toggleWidth = 36.0f * scale;
            ImGui::Dummy({0.0f, rowHeight});
            const ImVec2 nextRow = ImGui::GetCursorScreenPos();
            {
                ScopedTextStyle labelStyle(fonts.sansCompact, TextPx::Body(), FontPx::SansCompact);
                const float labelHeight = ImGui::CalcTextSize(label.c_str()).y;
                ImGui::SetCursorScreenPos({row.x, row.y + (rowHeight - labelHeight) * 0.5f});
                ImGui::TextColored(Muted(), "%s", label.c_str());
            }
            const float controlX = ImGui::GetWindowPos().x + 190.0f;
            ImGui::SetCursorScreenPos({controlX, row.y + (rowHeight - toggleHeight) * 0.5f});
            const bool changed = ToggleControl(id, value, fonts, false);
            const auto status = Copy(modal.Localized(*value ? "asset_import.enabled" : "asset_import.disabled",
                                                     *value ? "Enabled" : "Disabled"));
            {
                ScopedTextStyle textStyle(fonts.sansCompact, TextPx::Body(), FontPx::SansCompact);
                const float statusHeight = ImGui::CalcTextSize(status.c_str()).y;
                ImGui::SetCursorScreenPos({controlX + toggleWidth + 10.0f * scale, row.y + (rowHeight - statusHeight) * 0.5f});
                ImGui::TextColored(Muted(), "%s", status.c_str());
            }
            ImGui::SetCursorScreenPos(nextRow);
            return changed;
        }

        void DrawSetting(AssetImportModal &modal, const Assets::ImportSettingDescriptor &setting,
                         const std::size_t itemIndex, const Fonts &fonts) {
            const std::string label = Copy(modal.Localized(setting.labelKey, setting.labelKey));
            const std::string id = "##Setting_" + setting.id;
            const auto current = modal.SettingValue(itemIndex, setting);
            if (setting.kind == Assets::ImportSettingKind::Boolean) {
                bool value = std::get_if<bool>(&current) ? std::get<bool>(current) : false;
                if (DrawBooleanSettingRow(modal, label, id.c_str(), &value, fonts))
                    modal.SetSettingValue(itemIndex, setting, value);
                return;
            }
            PushFont(fonts.sansCompact);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(Muted(), "%s", label.c_str());
            PopFont(fonts.sansCompact);
            ImGui::SameLine(190.0f);
            ImGui::SetNextItemWidth(std::max(100.0f, ImGui::GetContentRegionAvail().x));
            switch (setting.kind) {
                case Assets::ImportSettingKind::Choice: {
                    std::vector<std::string> localized;
                    std::vector<const char *> choices;
                    localized.reserve(setting.choices.size());
                    choices.reserve(setting.choices.size());
                    for (const auto &choice : setting.choices)
                        localized.push_back(Copy(modal.Localized(choice.labelKey, choice.labelKey)));
                    for (const auto &choice : localized)
                        choices.push_back(choice.c_str());
                    const auto *selected = std::get_if<std::size_t>(&current);
                    int index = static_cast<int>(std::min(selected ? *selected : 0U, setting.choices.size() ? setting.choices.size() - 1 : 0U));
                    if (!choices.empty() && ComboControl(id.c_str(), &index, choices.data(), static_cast<int>(choices.size()), fonts))
                        modal.SetSettingValue(itemIndex, setting, static_cast<std::size_t>(index));
                    break;
                }
                case Assets::ImportSettingKind::Float: {
                    const auto *typed = std::get_if<double>(&current);
                    float value = typed ? static_cast<float>(*typed) : 0.0f;
                    if (InputFloatStepperControl(id.c_str(), &value, fonts))
                        modal.SetSettingValue(itemIndex, setting, static_cast<double>(value));
                    break;
                }
                case Assets::ImportSettingKind::Integer: {
                    const auto *typed = std::get_if<std::int64_t>(&current);
                    int value = typed ? static_cast<int>(*typed) : 0;
                    const int previous = value;
                    InputIntControl(id.c_str(), &value, fonts);
                    if (value != previous)
                        modal.SetSettingValue(itemIndex, setting, static_cast<std::int64_t>(value));
                    break;
                }
                case Assets::ImportSettingKind::Text: {
                    const auto *typed = std::get_if<std::string>(&current);
                    std::string value = typed ? *typed : std::string{};
                    if (InputTextControl(id.c_str(), value, 256, fonts))
                        modal.SetSettingValue(itemIndex, setting, value);
                    break;
                }
                case Assets::ImportSettingKind::Boolean:
                    break;
            }
        }

        void DrawPreset(AssetImportModal &modal, const Assets::AssetImportSnapshot &snapshot, const Fonts &fonts) {
            const auto index = snapshot.selectedItemIndex;
            auto names = modal.PresetNames(index);
            std::vector<const char *> labels;
            labels.reserve(names.size());
            for (const auto &name : names)
                labels.push_back(name.c_str());
            int selected = 0;
            const auto active = modal.ActivePresetName(index);
            if (const auto found = std::ranges::find(names, active); found != names.end())
                selected = static_cast<int>(std::distance(names.begin(), found));
            ImGui::TextColored(Muted(), "%s", Copy(modal.Localized("asset_import.preset", "Import Preset")).c_str());
            ImGui::SameLine(190.0f);
            ImGui::SetNextItemWidth(std::max(100.0f, ImGui::GetContentRegionAvail().x));
            if (ComboControl("##ImportPreset", &selected, labels.data(), static_cast<int>(labels.size()), fonts))
                static_cast<void>(modal.ApplyPreset(index, names[selected]));
        }

        void DrawCreatePresetAction(AssetImportModal &modal, const Assets::AssetImportSnapshot &snapshot, const Fonts &fonts) {
            const auto index = snapshot.selectedItemIndex;
            const auto createPreset = Copy(modal.Localized("asset_import.create_preset", "Create preset from current settings"));
            constexpr const char *popupId = "##CreateImportPresetPopup";
            static std::string name;
            if (Button({.label = createPreset.c_str(), .variant = ButtonVariant::Secondary})) {
                name.clear();
                ImGui::OpenPopup(popupId);
            }
            if (!ImGui::IsPopupOpen(popupId))
                return;

            const ImGuiViewport *viewport = ImGui::GetMainViewport();
            const float popupWidth = std::max(280.0f, std::min(420.0f, viewport->WorkSize.x - 32.0f));
            constexpr float popupHeight = 230.0f;
            ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, {0.5f, 0.5f});
            ImGui::SetNextWindowSize({popupWidth, popupHeight}, ImGuiCond_Always);
            ImGui::PushStyleColor(ImGuiCol_PopupBg, Bg1());
            ImGui::PushStyleColor(ImGuiCol_Border, BorderStrong());
            ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, Shadow());
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
            ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, GetActiveTokens().radii.modal);
            ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 1.0f);
            if (ImGui::BeginPopupModal(popupId, nullptr,
                                       ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                           ImGuiWindowFlags_NoScrollbar)) {
                const ImVec2 origin = ImGui::GetWindowPos();
                ImDrawList *const drawList = ImGui::GetWindowDrawList();
                drawList->AddRectFilled(origin, {origin.x + popupWidth, origin.y + 56.0f}, U32(Bg2()), GetActiveTokens().radii.modal,
                                        ImDrawFlags_RoundCornersTop);
                drawList->AddLine({origin.x, origin.y + 56.0f}, {origin.x + popupWidth, origin.y + 56.0f}, U32(Border()));
                drawList->AddLine({origin.x, origin.y + 168.0f}, {origin.x + popupWidth, origin.y + 168.0f}, U32(Border()));
                drawList->AddRect(origin, {origin.x + popupWidth, origin.y + popupHeight}, U32(BorderStrong()),
                                  GetActiveTokens().radii.modal);

                const auto title = Copy(modal.Localized("asset_import.create", "Create")) + " " +
                                   Copy(modal.Localized("asset_import.preset", "Import Preset"));
                ImGui::SetCursorPos({22.0f, 16.0f});
                ImGui::PushClipRect({origin.x + 22.0f, origin.y}, {origin.x + popupWidth - 56.0f, origin.y + 56.0f}, true);
                {
                    ScopedTextStyle titleStyle(fonts.sansEmphasis, TextPx::Title(), FontPx::SansEmphasis);
                    ImGui::TextColored(Text(), "%s", title.c_str());
                }
                ImGui::PopClipRect();
                ImGui::SetCursorPos({popupWidth - 46.0f, 13.0f});
                if (IconCloseButton("##CloseImportPresetPopup", {28.0f, 28.0f})) {
                    name.clear();
                    ImGui::CloseCurrentPopup();
                }

                ImGui::SetCursorPos({22.0f, 75.0f});
                FieldLabel(Copy(modal.Localized("asset_import.preset_name", "Preset Name")).c_str(), fonts);
                ImGui::SetCursorPosX(22.0f);
                if (ImGui::IsWindowAppearing())
                    ImGui::SetKeyboardFocusHere();
                static_cast<void>(
                    InputTextControl("##PresetName", name, 256, fonts, {.width = (popupWidth - 44.0f) / GetActiveTokens().sizes.uiScale}));

                const bool importerAvailable = !snapshot.items[index].importerContributionId.empty();
                const auto names = modal.PresetNames(index);
                const bool duplicate = std::ranges::find(names, name) != names.end();
                const bool blank = std::ranges::all_of(name, [](const unsigned char character) {
                    return std::isspace(character) != 0;
                });
                const auto hint =
                    !importerAvailable ? Copy(modal.Localized("asset_import.preset_unavailable", "No importer is available for this file."))
                    : duplicate        ? Copy(modal.Localized("asset_import.preset_exists", "A preset with this name already exists."))
                                       : Copy(modal.Localized("asset_import.preset_hint", "Save these settings for future imports."));
                ImGui::SetCursorPos({22.0f, 139.0f});
                {
                    ScopedTextStyle hintStyle(fonts.sansCompact, TextPx::Caption(), FontPx::SansCompact);
                    ImGui::PushTextWrapPos(popupWidth - 22.0f);
                    ImGui::TextColored(!importerAvailable || duplicate ? Warn() : Muted(), "%s", hint.c_str());
                    ImGui::PopTextWrapPos();
                }

                ImGui::SetCursorPos({popupWidth - 22.0f - 112.0f - 12.0f - 104.0f, 184.0f});
                if (Button({.label = Copy(modal.Localized("asset_import.cancel", "Cancel")).c_str(),
                            .size = {104.0f, 34.0f},
                            .variant = ButtonVariant::Secondary})) {
                    name.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine(0.0f, 12.0f);
                if (Button({.label = Copy(modal.Localized("asset_import.create", "Create")).c_str(),
                            .size = {112.0f, 34.0f},
                            .enabled = importerAvailable && !blank && !duplicate}) &&
                    modal.CreatePreset(index, name)) {
                    name.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(3);
        }

        void DrawSelectedFileHeader(AssetImportModal &modal, const Assets::AssetImportItem &item,
                                    const std::size_t index, const Fonts &fonts) {
            const ImVec2 preview = ImGui::GetCursorScreenPos();
            auto *drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(preview, {preview.x + 136.0f, preview.y + 116.0f}, ImGui::ColorConvertFloat4ToU32(Bg2()), 5.0f);
            if (PreviewKind(modal, index) == Assets::AssetPreviewFallback::Mesh) {
                const ImU32 grid = ImGui::ColorConvertFloat4ToU32(Border());
                drawList->PushClipRect({preview.x + 1.0f, preview.y + 1.0f}, {preview.x + 135.0f, preview.y + 115.0f}, true);
                for (float y = 78.0f; y <= 116.0f; y += 10.0f)
                    drawList->AddLine({preview.x, preview.y + y}, {preview.x + 136.0f, preview.y + y}, grid);
                for (float x = -24.0f; x <= 160.0f; x += 20.0f)
                    drawList->AddLine({preview.x + 68.0f, preview.y + 72.0f}, {preview.x + x, preview.y + 116.0f}, grid);
                drawList->PopClipRect();
            }
            if (const std::uintptr_t texture = modal.SelectedPreviewTexture(); texture != 0) {
                drawList->AddImage(static_cast<ImTextureID>(texture), {preview.x + 1.0f, preview.y + 1.0f},
                                   {preview.x + 135.0f, preview.y + 115.0f});
            } else {
                DrawEditorIcon(drawList, AssetIcon(modal, index), {preview.x + 48.0f, preview.y + 35.0f}, {40.0f, 40.0f},
                               ImGui::ColorConvertFloat4ToU32(Dim()), fonts.icon);
            }
            drawList->AddRect(preview, {preview.x + 136.0f, preview.y + 116.0f}, ImGui::ColorConvertFloat4ToU32(Border()), 5.0f);
            ImGui::Dummy({136.0f, 116.0f});
            ImGui::SameLine(0.0f, 16.0f);
            const float titleWidth = ImGui::GetContentRegionAvail().x;
            ImGui::BeginGroup();
            {
                ScopedTextStyle titleStyle(fonts.sansEmphasis, TextPx::Heading(), FontPx::SansEmphasis);
                const std::string name = FileName(item);
                const std::string fitted = FitFileName(name, titleWidth);
                ImGui::TextColored(Text(), "%s", fitted.c_str());
                if (fitted != name && ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", name.c_str());
            }
            ImGui::TextColored(Muted(), "%s  |  %s", AssetKind(modal, index).c_str(),
                               FileSize(modal, index).c_str());
#if defined(__APPLE__)
            ImGui::TextColored(Accent(), "%s", Copy(modal.Localized("asset_import.show_in_finder", "Show in Finder")).c_str());
            if (ImGui::IsItemClicked() && item.absoluteSourcePath.is_absolute() && !modal.IsReadOnlyPresentation())
                modal.RevealSelectedSource();
#else
            ImGui::TextColored(Dim(), "%s", item.absoluteSourcePath.parent_path().filename().string().c_str());
#endif
            ImGui::EndGroup();
        }

        void DrawImporterSettings(AssetImportModal &modal, const Assets::AssetImportSnapshot &snapshot, const Fonts &fonts) {
            const auto *contribution = modal.ImporterFor(snapshot.selectedItemIndex);
            if (!contribution) {
                ImGui::TextColored(ErrorColor, "%s",
                                   Copy(modal.Localized("asset_import.no_importer", "No importer is available for this file type."))
                                       .c_str());
            } else {
                ImGui::TextColored(Muted(), "%s", Copy(modal.Localized("asset_import.asset_type", "Asset Type")).c_str());
                ImGui::SameLine(190.0f);
                const auto assetType = AssetKind(modal, snapshot.selectedItemIndex);
                const char *assetTypes[]{assetType.c_str()};
                int selectedType = 0;
                ImGui::SetNextItemWidth(std::max(100.0f, ImGui::GetContentRegionAvail().x));
                static_cast<void>(ComboControl("##ImportAssetType", &selectedType, assetTypes, 1, fonts,
                                               {.leadingIcon = AssetIcon(modal, snapshot.selectedItemIndex)}));
                DrawPreset(modal, snapshot, fonts);
                for (const auto &setting : contribution->settings)
                    DrawSetting(modal, setting, snapshot.selectedItemIndex, fonts);
            }
        }

        void DrawAdvancedOptions(AssetImportModal &modal, const Assets::AssetImportSnapshot &snapshot, const Fonts &fonts) {
            ImGui::Dummy({0.0f, 8.0f});
            ImGui::SetCursorPosX(PanelPadding);
            if (modal.ConsumeInitialAdvancedState())
                ImGui::SetNextItemOpen(modal.InitialAdvancedOpen(), ImGuiCond_Always);
            if (ImGui::TreeNodeEx(Copy(modal.Localized("asset_import.advanced", "Advanced")).c_str())) {
                ImGui::Unindent();
                ImGui::Dummy({0.0f, 8.0f});
                DrawCreatePresetAction(modal, snapshot, fonts);
                auto options = modal.OptionsFor(snapshot.selectedItemIndex);
                bool optionsChanged = false;
                std::string name = options.assetName;
                FieldLabel(Copy(modal.Localized("asset_import.asset_name", "Asset Name")).c_str(), fonts);
                if (InputTextControl("##ImportAssetName", name, 256, fonts)) {
                    options.assetName = std::move(name);
                    optionsChanged = true;
                }
                ImGui::Dummy({0.0f, 6.0f});
                int subfolder = options.folderStrategy;
                const auto byType = Copy(modal.Localized("asset_import.folder.by_type", "By asset type"));
                const auto mirror = Copy(modal.Localized("asset_import.folder.mirror", "Mirror source"));
                const auto flat = Copy(modal.Localized("asset_import.folder.flat", "Flat"));
                const char *folders[]{byType.c_str(), mirror.c_str(), flat.c_str()};
                FieldLabel(Copy(modal.Localized("asset_import.folder_strategy", "Folder Strategy")).c_str(), fonts);
                ImGui::SetNextItemWidth(-1.0f);
                if (ComboControl("##ImportFolderStrategy", &subfolder, folders, 3, fonts)) {
                    options.folderStrategy = subfolder;
                    optionsChanged = true;
                }
                ImGui::Dummy({0.0f, 6.0f});
                int idStrategy = options.assetIdStrategy;
                const auto newGuid = Copy(modal.Localized("asset_import.id.new_guid", "New GUID"));
                const auto stableHash = Copy(modal.Localized("asset_import.id.stable_hash", "Stable hash"));
                const char *idStrategies[]{newGuid.c_str(), stableHash.c_str()};
                FieldLabel(Copy(modal.Localized("asset_import.id_strategy", "Asset ID Strategy")).c_str(), fonts);
                ImGui::SetNextItemWidth(-1.0f);
                if (ComboControl("##ImportAssetIdStrategy", &idStrategy, idStrategies, 2, fonts)) {
                    options.assetIdStrategy = idStrategy;
                    optionsChanged = true;
                }
                ImGui::Dummy({0.0f, 7.0f});
                bool sidecar = options.createMetaSidecar;
                if (CheckboxControl(Copy(modal.Localized("asset_import.meta_sidecar", "Create .meta sidecar")).c_str(), &sidecar, fonts)) {
                    options.createMetaSidecar = sidecar;
                    optionsChanged = true;
                }
                ImGui::Dummy({0.0f, 3.0f});
                bool overwrite = options.overwriteWithoutPrompt;
                if (CheckboxControl(Copy(modal.Localized("asset_import.overwrite", "Overwrite without prompt")).c_str(), &overwrite, fonts)) {
                    options.overwriteWithoutPrompt = overwrite;
                    optionsChanged = true;
                }
                if (optionsChanged)
                    modal.SetOptionsFor(snapshot.selectedItemIndex, std::move(options));
                ImGui::Indent();
                ImGui::TreePop();
            }
        }

        void DrawDetails(AssetImportModal &modal, const Assets::AssetImportSnapshot &snapshot, const Fonts &fonts, float height) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Bg1());
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {PanelPadding, PanelPadding});
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {8.0f, 5.0f});
            ImGui::BeginChild("ImportDetails", {0.0f, height}, true);
            if (snapshot.items.empty() || snapshot.selectedItemIndex >= snapshot.items.size()) {
                const ImVec2 origin = ImGui::GetCursorScreenPos();
                const ImVec2 available = ImGui::GetContentRegionAvail();
                const auto message = Copy(modal.Localized("asset_import.select_file", "Select a file to see its import settings."));
                const float messageWidth = ImGui::CalcTextSize(message.c_str()).x;
                const ImVec2 iconPosition{origin.x + available.x * 0.5f - 18.0f, origin.y + available.y * 0.5f - 35.0f};
                DrawEditorIcon(ImGui::GetWindowDrawList(), UiIcon::Package, iconPosition, {36.0f, 36.0f},
                               ImGui::ColorConvertFloat4ToU32(Dim()), fonts.icon);
                ImGui::SetCursorScreenPos({origin.x + std::max(0.0f, (available.x - messageWidth) * 0.5f), iconPosition.y + 48.0f});
                ImGui::TextColored(Dim(), "%s", message.c_str());
                ImGui::EndChild();
                ImGui::PopStyleVar(2);
                ImGui::PopStyleColor();
                return;
            }
            const auto &item = snapshot.items[snapshot.selectedItemIndex];
            DrawSelectedFileHeader(modal, item, snapshot.selectedItemIndex, fonts);
            ImGui::Dummy({0.0f, 4.0f});
            ImGui::Separator();
            ImGui::Dummy({0.0f, 4.0f});
            {
                ScopedTextStyle headingStyle(fonts.sansEmphasis, TextPx::CardTitle(), FontPx::SansEmphasis);
                ImGui::TextColored(Text(), "%s", Copy(modal.Localized("asset_import.settings", "Import Settings")).c_str());
            }
            ImGui::Dummy({0.0f, 3.0f});
            DrawImporterSettings(modal, snapshot, fonts);
            DrawAdvancedOptions(modal, snapshot, fonts);
            if (modal.ConsumeInitialScrollReset())
                ImGui::SetScrollY(0.0f);
            else if (ImGui::IsWindowHovered() && ImGui::GetScrollMaxY() > 0.0f) {
                // The editor distributes wheel deltas over several frames. Give this long
                // settings form a larger step without changing scrolling elsewhere.
                ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseWheel * 72.0f);
            }
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
        }

        void DrawFooter(AssetImportModal &modal, const Assets::AssetImportSnapshot &snapshot, const Fonts &fonts, ScopedModalShell &shell,
                        ModalFrameResult &result) {
            shell.BeginFooter({24.0f, 0.0f}, true);
            const float actionHeight = 34.0f;
            const float footerCenterY = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() * 0.5f;
            ImGui::SetCursorScreenPos({ImGui::GetCursorScreenPos().x, footerCenterY - 9.0f});
            std::size_t warnings = 0;
            std::size_t errors = 0;
            for (std::size_t index = 0; index < snapshot.items.size(); ++index) {
                if (!modal.IsItemVisible(index))
                    continue;
                const auto &item = snapshot.items[index];
                warnings += HasDiagnostic(item, Assets::ImportDiagnostic::Severity::Warning) ? 1 : 0;
                errors += HasDiagnostic(item, Assets::ImportDiagnostic::Severity::Error) ? 1 : 0;
            }
            const Assets::ImportDiagnostic *firstDiagnostic = nullptr;
            for (std::size_t index = 0; index < snapshot.items.size(); ++index) {
                if (!modal.IsItemVisible(index))
                    continue;
                const auto &item = snapshot.items[index];
                for (const auto &diagnostic : item.diagnostics) {
                    if ((errors > 0 && diagnostic.severity == Assets::ImportDiagnostic::Severity::Error) ||
                        (errors == 0 && diagnostic.severity == Assets::ImportDiagnostic::Severity::Warning)) {
                        firstDiagnostic = &diagnostic;
                        break;
                    }
                }
                if (firstDiagnostic)
                    break;
            }
            if (errors > 0 || warnings > 0) {
                const auto tone = errors > 0 ? ErrorColor : WarningColor;
                const ImVec2 iconPosition = ImGui::GetCursorScreenPos();
                DrawEditorIcon(ImGui::GetWindowDrawList(), errors > 0 ? UiIcon::Error : UiIcon::Warning, iconPosition, {18.0f, 18.0f},
                               ImGui::ColorConvertFloat4ToU32(tone), fonts.icon);
                ImGui::Dummy({20.0f, 18.0f});
                ImGui::SameLine(0.0f, 8.0f);
            }
            if (errors > 0)
                ImGui::TextColored(ErrorColor, "%zu %s", errors,
                                   Copy(modal.Localized(errors == 1 ? "asset_import.error" : "asset_import.errors",
                                                        errors == 1 ? "error" : "errors"))
                                       .c_str());
            else if (warnings > 0)
                ImGui::TextColored(WarningColor, "%zu %s", warnings,
                                   Copy(modal.Localized(warnings == 1 ? "asset_import.warning" : "asset_import.warnings",
                                                        warnings == 1 ? "warning" : "warnings"))
                                       .c_str());
            else
                ImGui::TextColored(Dim(), "%zu %s", modal.VisibleItemCount(), Copy(modal.Localized("asset_import.files", "files")).c_str());
            if (firstDiagnostic) {
                ImGui::SameLine(0.0f, 14.0f);
                const ImVec2 divider = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddLine({divider.x, divider.y}, {divider.x, divider.y + 18.0f},
                                                    ImGui::ColorConvertFloat4ToU32(Border()));
                ImGui::Dummy({1.0f, 18.0f});
                ImGui::SameLine(0.0f, 14.0f);
                ImGui::TextColored(Dim(), "%.55s", firstDiagnostic->message.c_str());
            }

            constexpr float cancelWidth = 112.0f;
            constexpr float importWidth = 174.0f;
            ImGui::SetCursorScreenPos({ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - cancelWidth - importWidth - 38.0f,
                                      footerCenterY - actionHeight * 0.5f});
            const bool complete = modal.IsImportComplete();
            if (Button({.label = Copy(modal.Localized(complete ? "asset_import.done" : "asset_import.cancel", complete ? "Done" : "Cancel"))
                                     .c_str(),
                        .size = {cancelWidth, actionHeight},
                        .variant = ButtonVariant::Secondary}))
                result = ModalFrameResult::RequestClose(complete ? ModalCloseReason::Completed : ModalCloseReason::Cancelled);
            ImGui::SameLine(0.0f, 12.0f);
            const bool valid = modal.CanImportIncludedItems();
            const auto label = std::format("{} {} {}", Copy(modal.Localized("asset_import.import", "Import")), modal.IncludedItemCount(),
                                           Copy(modal.Localized("asset_import.assets", "Assets")));
            if (Button(
                    {.label = label.c_str(), .size = {importWidth, actionHeight}, .variant = ButtonVariant::Primary, .enabled = valid})) {
                if (!modal.IsReadOnlyPresentation()) {
                    modal.StartIncludedImport();
                }
            }
            shell.EndFooter();
        }
    }  // namespace

    ModalFrameResult DrawAssetImportModalPresentation(AssetImportModal &modal, const Fonts &fonts) {
        const auto &snapshot = modal.Snapshot();
        ModalFrameResult result = ModalFrameResult::None();
        const auto title = Copy(modal.Localized("asset_import.title", "Import Assets"));
        std::optional<ModalPlacementRegion> previewRegion;
        const auto [canvasLeftInset, canvasTopInset] = modal.PresentationCanvasInsets();
        if (canvasLeftInset > 0.0f || canvasTopInset > 0.0f) {
            const ImGuiViewport *viewport = ImGui::GetMainViewport();
            previewRegion = ModalPlacementRegion{
                .position = {viewport->WorkPos.x + canvasLeftInset, viewport->WorkPos.y + canvasTopInset},
                .size = {viewport->WorkSize.x - canvasLeftInset, viewport->WorkSize.y - canvasTopInset},
            };
        }
        ScopedModalShell shell({.id = "Asset Import",
                                .title = title.c_str(),
                                .requestedSize = {1000.0f, 690.0f},
                                .viewportPadding = 64.0f,
                                .headerHeight = 38.0f,
                                .footerHeight = 68.0f,
                                .placementRegion = previewRegion,
                                .titleFontSize = TextPx::Title()},
                               fonts);
        if (shell.CloseRequested())
            result = ModalFrameResult::RequestClose(modal.IsImportComplete() ? ModalCloseReason::Completed : ModalCloseReason::Cancelled);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{22.0f, 12.0f});
        ImGui::BeginChild("##ImportBody", {0.0f, shell.BodyHeight()}, false,
                          ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::TextColored(Dim(), "%s",
                           Copy(modal.Localized("asset_import.subtitle", "Add files to your project and configure how they are imported."))
                               .c_str());
        ImGui::Dummy({0.0f, 4.0f});
        DrawDestination(modal, snapshot, fonts);
        ImGui::Dummy({0.0f, 6.0f});
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = std::max(160.0f, ImGui::GetContentRegionAvail().y - 12.0f);
        const float columnWidth = std::max(180.0f, (width - PanelGap) * 0.5f);
        DrawFiles(modal, snapshot, fonts, columnWidth, height);
        ImGui::SameLine(0.0f, PanelGap);
        DrawDetails(modal, snapshot, fonts, height);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        DrawFooter(modal, snapshot, fonts, shell, result);
        return result;
    }
}  // namespace Horo::Editor
