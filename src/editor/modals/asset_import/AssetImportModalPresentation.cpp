/** @copydoc AssetImportModalPresentation.h */

#include "AssetImportModalPresentation.h"

#include "Horo/Editor/AssetImportModal.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "editor/menu/EditorMenuPlatform.h"
#include "editor/ui_preview/EditorUiPreviewCatalog.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <imgui.h>
#include <iterator>
#include <optional>
#include <portable-file-dialogs.h>
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

        [[nodiscard]] std::string AssetKind(const AssetImportModal &modal, const Assets::AssetImportItem &item) {
            if (item.sourceExtension == "fbx" || item.sourceExtension == "obj" || item.sourceExtension == "gltf" ||
                item.sourceExtension == "glb")
                return Copy(modal.Localized("asset_import.type.model", "3D Model"));
            if (item.sourceExtension == "png" || item.sourceExtension == "jpg" || item.sourceExtension == "jpeg")
                return Copy(modal.Localized("asset_import.type.texture", "Texture"));
            if (item.sourceExtension == "wav" || item.sourceExtension == "mp3" || item.sourceExtension == "ogg")
                return Copy(modal.Localized("asset_import.type.audio", "Audio"));
            return Copy(modal.Localized("asset_import.type.asset", "Asset"));
        }

        [[nodiscard]] UiIcon AssetIcon(const Assets::AssetImportItem &item) {
            if (item.sourceExtension == "fbx" || item.sourceExtension == "obj" || item.sourceExtension == "gltf" ||
                item.sourceExtension == "glb")
                return UiIcon::HierarchyMesh;
            if (item.sourceExtension == "png" || item.sourceExtension == "jpg" || item.sourceExtension == "jpeg")
                return UiIcon::Image;
            if (item.sourceExtension == "wav" || item.sourceExtension == "mp3" || item.sourceExtension == "ogg")
                return UiIcon::AudioFile;
            return UiIcon::Package;
        }

        [[nodiscard]] bool HasDiagnostic(const Assets::AssetImportItem &item, Assets::ImportDiagnostic::Severity severity) {
            return std::ranges::any_of(item.diagnostics, [severity](const auto &diagnostic) {
                return diagnostic.severity == severity;
            });
        }

        [[nodiscard]] std::vector<std::filesystem::path> ChooseFiles(const std::filesystem::path &defaultPath) {
            pfd::open_file dialog("Select Asset Files", defaultPath.string(), {"All Files", "*"}, pfd::opt::multiselect);
            const auto selected = dialog.result();
            std::vector<std::filesystem::path> paths;
            paths.reserve(selected.size());
            for (const auto &path : selected)
                paths.emplace_back(path);
            return paths;
        }

        [[nodiscard]] std::optional<std::filesystem::path> ChooseFolder(const std::filesystem::path &defaultPath) {
            pfd::select_folder dialog("Select Asset Destination", defaultPath.string());
            const std::string selected = dialog.result();
            if (selected.empty())
                return std::nullopt;
            return std::filesystem::path{selected};
        }

        [[nodiscard]] std::optional<std::string> ProjectFolder(const std::filesystem::path &projectRoot,
                                                               const std::filesystem::path &selectedFolder) {
            if (projectRoot.empty())
                return std::nullopt;
            std::error_code error;
            const auto root = std::filesystem::weakly_canonical(projectRoot, error);
            if (error)
                return std::nullopt;
            const auto selected = std::filesystem::weakly_canonical(selectedFolder, error);
            if (error)
                return std::nullopt;
            const auto relative = selected.lexically_relative(root);
            if (relative.empty() || relative.is_absolute())
                return std::nullopt;
            if (const auto first = relative.begin(); first != relative.end() && *first == "..")
                return std::nullopt;
            const auto folder = relative.generic_string();
            if (folder != "assets" && !folder.starts_with("assets/"))
                return std::nullopt;
            return folder;
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

        void AddFiles(AssetImportModal &modal, const std::vector<std::filesystem::path> &paths) {
            if (paths.empty() || modal.IsUiPreview())
                return;
            CancellationToken cancellation;
            if (modal.ProjectRoot().empty())
                static_cast<void>(modal.BeginImport(paths, cancellation));
            else
                static_cast<void>(modal.BeginImport(paths, modal.ProjectRoot(), cancellation));
        }

        void DrawDestination(AssetImportModal &modal, Assets::AssetImportSnapshot &snapshot, const Fonts &fonts) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Bg2());
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {PanelPadding, 9.0f});
            ImGui::BeginChild("ImportDestination", {0.0f, 54.0f}, true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            const auto label = Copy(modal.Localized("asset_import.destination", "Destination"));
            PushFont(fonts.sansCompact);
            const ImVec2 iconPosition = ImGui::GetCursorScreenPos();
            DrawEditorIcon(ImGui::GetWindowDrawList(), UiIcon::Folder, {iconPosition.x, iconPosition.y + 5.0f}, {20.0f, 20.0f},
                           ImGui::ColorConvertFloat4ToU32(Text()), fonts.icon);
            ImGui::Dummy({22.0f, 27.0f});
            ImGui::SameLine(0.0f, 8.0f);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(Dim(), "%s", label.c_str());
            ImGui::SameLine(0.0f, 17.0f);
            const ImVec2 divider = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine({divider.x, divider.y + 2.0f}, {divider.x, divider.y + 25.0f},
                                                ImGui::ColorConvertFloat4ToU32(Border()));
            ImGui::Dummy({1.0f, 27.0f});
            ImGui::SameLine(0.0f, 16.0f);
            const auto folder = snapshot.items.empty() || snapshot.selectedItemIndex >= snapshot.items.size()
                                    ? std::string{modal.DefaultDestinationFolder()}
                                    : snapshot.items[snapshot.selectedItemIndex].destinationFolder;
            const std::string path = folder.empty() ? "assets" : folder;
            const std::string projectName = modal.ProjectRoot().filename().string();
            if (!projectName.empty()) {
                ImGui::TextColored(Text(), "%s", projectName.c_str());
                ImGui::SameLine(0.0f, 8.0f);
                ImGui::TextColored(Dim(), ">");
                ImGui::SameLine(0.0f, 8.0f);
            }
            const std::filesystem::path breadcrumb{path};
            for (auto part = breadcrumb.begin(); part != breadcrumb.end(); ++part) {
                ImGui::TextColored(Text(), "%s", part->string().c_str());
                if (std::next(part) != breadcrumb.end()) {
                    ImGui::SameLine(0.0f, 8.0f);
                    ImGui::TextColored(Dim(), ">");
                    ImGui::SameLine(0.0f, 8.0f);
                }
            }
            PopFont(fonts.sansCompact);
            ImGui::SameLine(ImGui::GetWindowWidth() - 128.0f);
            if (Button({.label = Copy(modal.Localized("asset_import.change", "Change...")).c_str(),
                        .size = {104.0f, 32.0f},
                        .variant = ButtonVariant::Secondary,
                        .enabled = !modal.ProjectRoot().empty() && !modal.IsUiPreview()})) {
                if (const auto selected = ChooseFolder(modal.ProjectRoot())) {
                    if (const auto relative = ProjectFolder(modal.ProjectRoot(), *selected)) {
                        modal.SetDefaultDestination(*selected);
                        for (auto &item : snapshot.items)
                            if (!item.result.has_value())
                                item.destinationFolder = *relative;
                    }
                }
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
            if (ImGui::InvisibleButton("##ImportDropZone", {width, height}) && !modal.IsUiPreview())
                AddFiles(modal, ChooseFiles(modal.ProjectRoot()));
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
                AddFiles(modal, DroppedFiles(ImGui::AcceptDragDropPayload("FILES")));
                ImGui::EndDragDropTarget();
            }
        }

        void DrawFiles(AssetImportModal &modal, const Assets::AssetImportSnapshot &snapshot, const Fonts &fonts, float width,
                       float height) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Bg1());
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {PanelPadding, PanelPadding});
            ImGui::BeginChild("ImportFilePanel", {width, height}, true);
            const auto heading = Copy(modal.Localized("asset_import.selected_files", "Selected Files"));
            PushFont(fonts.sansEmphasis);
            ImGui::TextColored(Text(), "%s", heading.c_str());
            PopFont(fonts.sansEmphasis);
            PushFont(fonts.sansCompact);
            const auto count = std::format("{} {}", snapshot.items.size(), Copy(modal.Localized("asset_import.files", "files")));
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(count.c_str()).x);
            ImGui::TextColored(Dim(), "%s", count.c_str());
            PopFont(fonts.sansCompact);
            ImGui::Dummy({0.0f, 7.0f});

            const float zoneHeight = 76.0f;
            const float listHeight =
                std::max(0.0f, ImGui::GetContentRegionAvail().y - zoneHeight - 7.0f - 2.0f * ImGui::GetStyle().ItemSpacing.y - 2.0f);
            const ImGuiWindowFlags listFlags =
                snapshot.items.empty() ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : ImGuiWindowFlags_None;
            ImGui::BeginChild("ImportFileList", {0.0f, listHeight}, false, listFlags);
            for (std::size_t index = 0; index < snapshot.items.size(); ++index) {
                const auto &item = snapshot.items[index];
                const bool selected = snapshot.selectedItemIndex == index;
                const ImVec2 rowMin = ImGui::GetCursorScreenPos();
                const float rowWidth = ImGui::GetContentRegionAvail().x;
                constexpr float rowHeight = 56.0f;
                if (ImGui::InvisibleButton(std::format("##ImportFile{}", index).c_str(), {rowWidth, rowHeight}))
                    modal.SelectItem(index);
                const ImVec2 rowMax{rowMin.x + rowWidth, rowMin.y + rowHeight};
                auto *drawList = ImGui::GetWindowDrawList();
                const ImVec4 background = selected ? ImVec4{Accent().x, Accent().y, Accent().z, 0.13f} : Bg2();
                drawList->AddRectFilled(rowMin, rowMax, ImGui::ColorConvertFloat4ToU32(background), 5.0f);
                drawList->AddRect(rowMin, rowMax, ImGui::ColorConvertFloat4ToU32(selected ? Accent() : Border()), 5.0f);
                drawList->AddRectFilled({rowMin.x + 9.0f, rowMin.y + 8.0f}, {rowMin.x + 49.0f, rowMin.y + 48.0f},
                                        ImGui::ColorConvertFloat4ToU32(Bg3()), 5.0f);
                DrawEditorIcon(drawList, AssetIcon(item), {rowMin.x + 17.0f, rowMin.y + 16.0f}, {24.0f, 24.0f},
                               ImGui::ColorConvertFloat4ToU32(Text()), fonts.icon);
                const auto name = FileName(item);
                const auto details = std::format("{}  |  {}", AssetKind(modal, item), FileSize(modal, index));
                drawList->PushClipRect({rowMin.x + 64.0f, rowMin.y}, {rowMax.x - 60.0f, rowMax.y}, true);
                drawList->AddText({rowMin.x + 64.0f, rowMin.y + 8.0f}, ImGui::ColorConvertFloat4ToU32(Text()), name.c_str());
                drawList->AddText({rowMin.x + 64.0f, rowMin.y + 29.0f}, ImGui::ColorConvertFloat4ToU32(Dim()), details.c_str());
                drawList->PopClipRect();
                if (HasDiagnostic(item, Assets::ImportDiagnostic::Severity::Warning) ||
                    HasDiagnostic(item, Assets::ImportDiagnostic::Severity::Error)) {
                    const auto tone = HasDiagnostic(item, Assets::ImportDiagnostic::Severity::Error) ? ErrorColor : WarningColor;
                    DrawEditorIcon(drawList,
                                   HasDiagnostic(item, Assets::ImportDiagnostic::Severity::Error) ? UiIcon::Error : UiIcon::Warning,
                                   {rowMax.x - 57.0f, rowMin.y + 19.0f}, {17.0f, 17.0f}, ImGui::ColorConvertFloat4ToU32(tone), fonts.icon);
                }
                ImGui::SetCursorScreenPos({rowMax.x - 32.0f, rowMin.y + 16.0f});
                bool included = modal.IsItemIncluded(index);
                ImGui::BeginDisabled(item.result.has_value());
                if (CheckboxControl(std::format("##IncludeImport{}", index).c_str(), &included, fonts))
                    modal.SetItemIncluded(index, included);
                ImGui::EndDisabled();
                ImGui::SetCursorScreenPos({rowMin.x, rowMax.y + 5.0f});
            }
            if (snapshot.items.empty()) {
                const auto empty = Copy(modal.Localized("asset_import.no_files", "No files selected yet."));
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

        [[nodiscard]] bool IsPrimarySetting(std::string_view id) {
            return id == "unitScale" || id == "importMaterials" || id == "generateCollision" || id == "importAnimations";
        }

        [[nodiscard]] const Assets::ImportSettingDescriptor *FindSetting(const Assets::AssetImporterContribution &contribution,
                                                                         std::string_view id) {
            const auto found = std::ranges::find_if(contribution.settings, [id](const auto &setting) {
                return setting.id == id;
            });
            return found == contribution.settings.end() ? nullptr : &*found;
        }

        void DrawSetting(const AssetImportModal &modal, const Assets::ImportSettingDescriptor &setting, Assets::AssetImportItem &item,
                         const Fonts &fonts) {
            const std::string key = "settings." + setting.id;
            PushFont(fonts.sansCompact);
            ImGui::AlignTextToFramePadding();
            const auto label =
                setting.id == "unitScale"          ? Copy(modal.Localized("asset_import.scale", "Scale"))
                : setting.id == "importMaterials"  ? Copy(modal.Localized("asset_import.generate_materials", "Generate Materials"))
                : setting.id == "importAnimations" ? Copy(modal.Localized("asset_import.animation_import", "Animation Import"))
                                                   : setting.labelKey;
            ImGui::TextColored(Muted(), "%s", label.c_str());
            PopFont(fonts.sansCompact);
            ImGui::SameLine(190.0f);
            ImGui::SetNextItemWidth(std::max(100.0f, ImGui::GetContentRegionAvail().x));
            if (setting.kind == Assets::ImportSettingKind::Boolean) {
                bool value = item.settings.contains(key)
                                 ? item.settings[key] == "true"
                                 : std::holds_alternative<bool>(setting.defaultValue) && std::get<bool>(setting.defaultValue);
                if (ToggleControl(("##Setting_" + setting.id).c_str(), &value, fonts, true))
                    item.settings[key] = value ? "true" : "false";
            } else if (setting.kind == Assets::ImportSettingKind::Choice) {
                std::vector<const char *> choices;
                choices.reserve(setting.choices.size());
                for (const auto &choice : setting.choices)
                    choices.push_back(choice.labelKey.c_str());
                int index = 0;
                if (const auto found = item.settings.find(key); found != item.settings.end()) {
                    try {
                        index = std::stoi(found->second);
                    } catch (...) {
                        index = 0;
                    }
                }
                index = std::clamp(index, 0, std::max(0, static_cast<int>(choices.size()) - 1));
                if (ComboControl(("##Setting_" + setting.id).c_str(), &index, choices.data(), static_cast<int>(choices.size()), fonts))
                    item.settings[key] = std::to_string(index);
            } else if (setting.kind == Assets::ImportSettingKind::Float) {
                float value = std::holds_alternative<double>(setting.defaultValue)
                                  ? static_cast<float>(std::get<double>(setting.defaultValue))
                                  : 0.0f;
                if (const auto found = item.settings.find(key); found != item.settings.end()) {
                    try {
                        value = std::stof(found->second);
                    } catch (...) {
                    }
                }
                if (setting.id == "unitScale")
                    static_cast<void>(InputFloatStepperControl(("##Setting_" + setting.id).c_str(), &value, fonts));
                else
                    InputFloatControl(("##Setting_" + setting.id).c_str(), &value, fonts);
                item.settings[key] = std::to_string(value);
            } else if (setting.kind == Assets::ImportSettingKind::Integer) {
                int value = std::holds_alternative<std::int64_t>(setting.defaultValue)
                                ? static_cast<int>(std::get<std::int64_t>(setting.defaultValue))
                                : 0;
                if (const auto found = item.settings.find(key); found != item.settings.end()) {
                    try {
                        value = std::stoi(found->second);
                    } catch (...) {
                    }
                }
                InputIntControl(("##Setting_" + setting.id).c_str(), &value, fonts);
                item.settings[key] = std::to_string(value);
            } else {
                std::string value = item.settings.contains(key) ? item.settings[key] : std::string{};
                if (InputTextControl(("##Setting_" + setting.id).c_str(), value, 256, fonts))
                    item.settings[key] = std::move(value);
            }
        }

        void DrawUnavailableSetting(const AssetImportModal &modal, std::string_view labelKey, std::string_view fallback, const char *id,
                                    bool number, const Fonts &fonts) {
            const auto label = Copy(modal.Localized(labelKey, fallback));
            ImGui::TextColored(Muted(), "%s", label.c_str());
            ImGui::SameLine(190.0f);
            ImGui::BeginDisabled();
            if (number) {
                float value = 1.0f;
                ImGui::SetNextItemWidth(std::max(100.0f, ImGui::GetContentRegionAvail().x));
                static_cast<void>(InputFloatStepperControl(id, &value, fonts));
            } else {
                bool value = false;
                static_cast<void>(ToggleControl(id, &value, fonts, true));
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ShowTooltip(Copy(modal.Localized("asset_import.setting_unavailable", "This importer does not support this setting."))
                                .c_str(),
                            &fonts);
        }

        void DrawPreset(AssetImportModal &modal, Assets::AssetImportSnapshot &snapshot, const Fonts &fonts) {
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

        void DrawCreatePresetAction(AssetImportModal &modal, Assets::AssetImportSnapshot &snapshot, const Fonts &fonts) {
            const auto index = snapshot.selectedItemIndex;
            const auto createPreset = Copy(modal.Localized("asset_import.create_preset", "Create preset from current settings"));
            const auto presetPopupTitle = Copy(modal.Localized("asset_import.create", "Create")) + " " +
                                          Copy(modal.Localized("asset_import.preset", "Import Preset")) + "##CreateImportPresetPopup";
            if (Button({.label = createPreset.c_str(), .variant = ButtonVariant::Secondary}))
                ImGui::OpenPopup(presetPopupTitle.c_str());
            if (ImGui::BeginPopupModal(presetPopupTitle.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                static std::string name;
                FieldLabel(Copy(modal.Localized("asset_import.preset_name", "Preset Name")).c_str(), fonts);
                static_cast<void>(InputTextControl("##PresetName", name, 256, fonts));
                if (Button({.label = Copy(modal.Localized("asset_import.cancel", "Cancel")).c_str(), .variant = ButtonVariant::Secondary}))
                    ImGui::CloseCurrentPopup();
                ImGui::SameLine();
                if (Button({.label = Copy(modal.Localized("asset_import.create", "Create")).c_str(), .enabled = !name.empty()}) &&
                    modal.CreatePreset(index, name)) {
                    name.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        void DrawDetails(AssetImportModal &modal, Assets::AssetImportSnapshot &snapshot, const Fonts &fonts, float height) {
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
            auto &item = snapshot.items[snapshot.selectedItemIndex];
            const ImVec2 preview = ImGui::GetCursorScreenPos();
            auto *drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(preview, {preview.x + 136.0f, preview.y + 116.0f}, ImGui::ColorConvertFloat4ToU32(Bg2()), 5.0f);
            if (AssetIcon(item) == UiIcon::HierarchyMesh) {
                const ImU32 grid = ImGui::ColorConvertFloat4ToU32(Border());
                drawList->PushClipRect({preview.x + 1.0f, preview.y + 1.0f}, {preview.x + 135.0f, preview.y + 115.0f}, true);
                for (float y = 78.0f; y <= 116.0f; y += 10.0f)
                    drawList->AddLine({preview.x, preview.y + y}, {preview.x + 136.0f, preview.y + y}, grid);
                for (float x = -24.0f; x <= 160.0f; x += 20.0f)
                    drawList->AddLine({preview.x + 68.0f, preview.y + 72.0f}, {preview.x + x, preview.y + 116.0f}, grid);
                drawList->PopClipRect();
            }
            drawList->AddRect(preview, {preview.x + 136.0f, preview.y + 116.0f}, ImGui::ColorConvertFloat4ToU32(Border()), 5.0f);
            DrawEditorIcon(drawList, AssetIcon(item), {preview.x + 48.0f, preview.y + 35.0f}, {40.0f, 40.0f},
                           ImGui::ColorConvertFloat4ToU32(Dim()), fonts.icon);
            ImGui::Dummy({136.0f, 116.0f});
            ImGui::SameLine(0.0f, 16.0f);
            ImGui::BeginGroup();
            {
                ScopedTextStyle titleStyle(fonts.sansEmphasis, TextPx::Heading(), FontPx::SansEmphasis);
                ImGui::TextColored(Text(), "%s", FileName(item).c_str());
            }
            ImGui::TextColored(Muted(), "%s  |  %s", AssetKind(modal, item).c_str(), FileSize(modal, snapshot.selectedItemIndex).c_str());
#if defined(__APPLE__)
            ImGui::TextColored(Accent(), "%s", Copy(modal.Localized("asset_import.show_in_finder", "Show in Finder")).c_str());
            if (ImGui::IsItemClicked() && item.absoluteSourcePath.is_absolute() && !modal.IsUiPreview())
                static_cast<void>(RevealInNativeFileManager(item.absoluteSourcePath));
#else
            ImGui::TextColored(Dim(), "%s", item.absoluteSourcePath.parent_path().filename().string().c_str());
#endif
            ImGui::EndGroup();
            ImGui::Dummy({0.0f, 4.0f});
            ImGui::Separator();
            ImGui::Dummy({0.0f, 4.0f});
            {
                ScopedTextStyle headingStyle(fonts.sansEmphasis, TextPx::CardTitle(), FontPx::SansEmphasis);
                ImGui::TextColored(Text(), "%s", Copy(modal.Localized("asset_import.settings", "Import Settings")).c_str());
            }
            ImGui::Dummy({0.0f, 3.0f});
            const auto *contribution = modal.Catalog().FindContributionByExtension(item.sourceExtension);
            if (!contribution) {
                ImGui::TextColored(ErrorColor, "%s",
                                   Copy(modal.Localized("asset_import.no_importer", "No importer is available for this file type."))
                                       .c_str());
            } else {
                ImGui::TextColored(Muted(), "%s", Copy(modal.Localized("asset_import.asset_type", "Asset Type")).c_str());
                ImGui::SameLine(190.0f);
                const auto assetType = AssetKind(modal, item);
                const char *assetTypes[]{assetType.c_str()};
                int selectedType = 0;
                ImGui::SetNextItemWidth(std::max(100.0f, ImGui::GetContentRegionAvail().x));
                static_cast<void>(ComboControl("##ImportAssetType", &selectedType, assetTypes, 1, fonts));
                DrawPreset(modal, snapshot, fonts);
                if (AssetIcon(item) == UiIcon::HierarchyMesh) {
                    if (const auto *scale = FindSetting(*contribution, "unitScale"))
                        DrawSetting(modal, *scale, item, fonts);
                    else
                        DrawUnavailableSetting(modal, "asset_import.scale", "Scale", "##UnavailableScale", true, fonts);
                    if (const auto *materials = FindSetting(*contribution, "importMaterials"))
                        DrawSetting(modal, *materials, item, fonts);
                    else
                        DrawUnavailableSetting(modal, "asset_import.generate_materials", "Generate Materials", "##UnavailableMaterials",
                                               false, fonts);
                    if (const auto *collision = FindSetting(*contribution, "generateCollision"))
                        DrawSetting(modal, *collision, item, fonts);
                    else
                        DrawUnavailableSetting(modal, "asset_import.generate_collision", "Generate Collision", "##UnavailableCollision",
                                               false, fonts);
                    if (const auto *animation = FindSetting(*contribution, "importAnimations"))
                        DrawSetting(modal, *animation, item, fonts);
                    else
                        DrawUnavailableSetting(modal, "asset_import.animation_import", "Animation Import", "##UnavailableAnimation", false,
                                               fonts);
                } else {
                    for (const auto &setting : contribution->settings)
                        DrawSetting(modal, setting, item, fonts);
                }
            }
            ImGui::Dummy({0.0f, 4.0f});
            ImGui::Separator();
            if (ImGui::TreeNodeEx(Copy(modal.Localized("asset_import.advanced", "Advanced")).c_str())) {
                DrawCreatePresetAction(modal, snapshot, fonts);
                if (contribution && AssetIcon(item) == UiIcon::HierarchyMesh) {
                    for (const auto &setting : contribution->settings)
                        if (!IsPrimarySetting(setting.id))
                            DrawSetting(modal, setting, item, fonts);
                }
                std::string name = item.displayName;
                FieldLabel(Copy(modal.Localized("asset_import.asset_name", "Asset Name")).c_str(), fonts);
                if (InputTextControl("##ImportAssetName", name, 256, fonts))
                    item.displayName = std::move(name);
                int subfolder = item.subfolderByType;
                const auto byType = Copy(modal.Localized("asset_import.folder.by_type", "By asset type"));
                const auto mirror = Copy(modal.Localized("asset_import.folder.mirror", "Mirror source"));
                const auto flat = Copy(modal.Localized("asset_import.folder.flat", "Flat"));
                const char *folders[]{byType.c_str(), mirror.c_str(), flat.c_str()};
                FieldLabel(Copy(modal.Localized("asset_import.folder_strategy", "Folder Strategy")).c_str(), fonts);
                if (ComboControl("##ImportFolderStrategy", &subfolder, folders, 3, fonts))
                    item.subfolderByType = subfolder;
                int idStrategy = item.assetIdStrategy;
                const auto newGuid = Copy(modal.Localized("asset_import.id.new_guid", "New GUID"));
                const auto stableHash = Copy(modal.Localized("asset_import.id.stable_hash", "Stable hash"));
                const char *idStrategies[]{newGuid.c_str(), stableHash.c_str()};
                FieldLabel(Copy(modal.Localized("asset_import.id_strategy", "Asset ID Strategy")).c_str(), fonts);
                if (ComboControl("##ImportAssetIdStrategy", &idStrategy, idStrategies, 2, fonts))
                    item.assetIdStrategy = idStrategy;
                bool sidecar = item.createMetaSidecar;
                if (CheckboxControl(Copy(modal.Localized("asset_import.meta_sidecar", "Create .meta sidecar")).c_str(), &sidecar, fonts))
                    item.createMetaSidecar = sidecar;
                bool overwrite = item.overwriteWithoutPrompt;
                if (CheckboxControl(Copy(modal.Localized("asset_import.overwrite", "Overwrite without prompt")).c_str(), &overwrite, fonts))
                    item.overwriteWithoutPrompt = overwrite;
                for (const auto &diagnostic : item.diagnostics) {
                    const auto color = diagnostic.severity == Assets::ImportDiagnostic::Severity::Error ? ErrorColor : WarningColor;
                    ImGui::PushStyleColor(ImGuiCol_Text, color);
                    ImGui::TextWrapped("%s", diagnostic.message.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::TreePop();
            }
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
        }

        void DrawFooter(AssetImportModal &modal, const Assets::AssetImportSnapshot &snapshot, const Fonts &fonts, ScopedModalShell &shell,
                        ModalFrameResult &result) {
            shell.BeginFooter({24.0f, 0.0f}, true);
            const float actionHeight = 34.0f;
            ImGui::SetCursorPosY((ImGui::GetWindowHeight() - actionHeight) * 0.5f);
            std::size_t warnings = 0;
            std::size_t errors = 0;
            for (const auto &item : snapshot.items) {
                warnings += HasDiagnostic(item, Assets::ImportDiagnostic::Severity::Warning) ? 1 : 0;
                errors += HasDiagnostic(item, Assets::ImportDiagnostic::Severity::Error) ? 1 : 0;
            }
            const Assets::ImportDiagnostic *firstDiagnostic = nullptr;
            for (const auto &item : snapshot.items) {
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
                ImGui::TextColored(Dim(), "%zu %s", snapshot.items.size(), Copy(modal.Localized("asset_import.files", "files")).c_str());
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
            ImGui::SameLine(ImGui::GetWindowWidth() - cancelWidth - importWidth - 38.0f);
            ImGui::SetCursorPosY((ImGui::GetWindowHeight() - actionHeight) * 0.5f);
            const bool complete = modal.IsImportComplete();
            if (Button({.label = Copy(modal.Localized(complete ? "asset_import.done" : "asset_import.cancel", complete ? "Done" : "Cancel"))
                                     .c_str(),
                        .size = {cancelWidth, actionHeight},
                        .variant = ButtonVariant::Secondary}))
                result = ModalFrameResult::RequestClose(complete ? ModalCloseReason::Completed : ModalCloseReason::Cancelled);
            ImGui::SameLine(0.0f, 12.0f);
            bool valid = modal.IncludedItemCount() > 0 && !modal.HasPendingConflicts();
            for (std::size_t index = 0; index < snapshot.items.size(); ++index)
                if (modal.IsItemIncluded(index) &&
                    (snapshot.items[index].displayName.empty() || snapshot.items[index].importerContributionId.empty()))
                    valid = false;
            const auto label = std::format("{} {} {}", Copy(modal.Localized("asset_import.import", "Import")), modal.IncludedItemCount(),
                                           Copy(modal.Localized("asset_import.assets", "Assets")));
            if (Button(
                    {.label = label.c_str(), .size = {importWidth, actionHeight}, .variant = ButtonVariant::Primary, .enabled = valid})) {
                if (!modal.IsUiPreview()) {
                    CancellationToken cancellation;
                    static_cast<void>(modal.ImportIncludedItems(cancellation));
                }
            }
            shell.EndFooter();
        }
    }  // namespace

    ModalFrameResult DrawAssetImportModalPresentation(AssetImportModal &modal, const Fonts &fonts) {
        auto &snapshot = modal.MutableSnapshot();
        ModalFrameResult result = ModalFrameResult::None();
        const auto title = Copy(modal.Localized("asset_import.title", "Import Assets"));
        std::optional<ModalPlacementRegion> previewRegion;
        if (modal.IsUiPreview()) {
            const ImGuiViewport *viewport = ImGui::GetMainViewport();
            previewRegion = ModalPlacementRegion{
                .position = {viewport->WorkPos.x + EditorUiPreviewSidebarWidth, viewport->WorkPos.y + EditorUiPreviewHeaderHeight},
                .size = {viewport->WorkSize.x - EditorUiPreviewSidebarWidth, viewport->WorkSize.y - EditorUiPreviewHeaderHeight},
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
