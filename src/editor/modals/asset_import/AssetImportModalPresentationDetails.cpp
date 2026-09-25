#include "AssetImportModalPresentationDetails.h"

#include "AssetImportModalPresentationCommon.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <imgui.h>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Editor {
    namespace {
        using namespace Theme;
        using namespace Ui;
        using namespace AssetImportPresentationDetail;

        [[nodiscard]] bool DrawBooleanSettingRow(const AssetImportModal &modal, const std::string &label, const char *id, bool *value,
                                                 const Fonts &fonts) {
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
            const auto status =
                Copy(modal.Localized(*value ? "asset_import.enabled" : "asset_import.disabled", *value ? "Enabled" : "Disabled"));
            {
                ScopedTextStyle textStyle(fonts.sansCompact, TextPx::Body(), FontPx::SansCompact);
                const float statusHeight = ImGui::CalcTextSize(status.c_str()).y;
                ImGui::SetCursorScreenPos({controlX + toggleWidth + 10.0f * scale, row.y + (rowHeight - statusHeight) * 0.5f});
                ImGui::TextColored(Muted(), "%s", status.c_str());
            }
            ImGui::SetCursorScreenPos(nextRow);
            return changed;
        }

        /** @brief Presents one catalog-defined choice and stores its selected index. */
        void DrawChoiceSetting(AssetImportModal &modal, const Assets::ImportSettingDescriptor &setting, const std::size_t itemIndex,
                               const Assets::ImportSettingValue &current, const std::string &id, const Fonts &fonts) {
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
        }

        void DrawSetting(AssetImportModal &modal, const Assets::ImportSettingDescriptor &setting, const std::size_t itemIndex,
                         const Fonts &fonts) {
            const std::string label = Copy(modal.Localized(setting.labelKey, setting.labelKey));
            const std::string id = "##Setting_" + setting.id;
            const auto current = modal.SettingValue(itemIndex, setting);
            if (setting.kind == Assets::ImportSettingKind::Boolean) {
                bool value = std::get_if<bool>(&current) ? std::get<bool>(current) : false;
                if (DrawBooleanSettingRow(modal, label, id.c_str(), &value, fonts))
                    modal.SetSettingValue(itemIndex, setting, value);
                EndImportField();
                return;
            }
            BeginImportField(label.c_str(), fonts);
            switch (setting.kind) {
                case Assets::ImportSettingKind::Choice: {
                    DrawChoiceSetting(modal, setting, itemIndex, current, id, fonts);
                    break;
                }
                case Assets::ImportSettingKind::Float: {
                    const auto *typed = std::get_if<double>(&current);
                    float value = typed ? static_cast<float>(*typed) : 0.0f;
                    if (InputFloatStepperControl(id.c_str(), &value, fonts, 0.1F, false))
                        modal.SetSettingValue(itemIndex, setting, static_cast<double>(value));
                    break;
                }
                case Assets::ImportSettingKind::Integer: {
                    const auto *typed = std::get_if<std::int64_t>(&current);
                    int value = typed ? static_cast<int>(*typed) : 0;
                    const int previous = value;
                    InputIntControl(id.c_str(), &value, fonts, false);
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
            EndImportField();
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
            BeginImportField(Copy(modal.Localized("asset_import.preset", "Import Preset")).c_str(), fonts);
            if (ComboControl("##ImportPreset", &selected, labels.data(), static_cast<int>(labels.size()), fonts))
                static_cast<void>(modal.ApplyPreset(index, names[selected]));
            EndImportField();
        }

        void DrawPresetPopupHeader(AssetImportModal &modal, const Fonts &fonts, std::string &name, const float popupWidth,
                                   const float popupHeight) {
            const ImVec2 origin = ImGui::GetWindowPos();
            ImDrawList *const drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(origin, {origin.x + popupWidth, origin.y + 56.0f}, U32(Bg2()), GetActiveTokens().radii.modal,
                                    ImDrawFlags_RoundCornersTop);
            drawList->AddLine({origin.x, origin.y + 56.0f}, {origin.x + popupWidth, origin.y + 56.0f}, U32(Border()));
            drawList->AddLine({origin.x, origin.y + 168.0f}, {origin.x + popupWidth, origin.y + 168.0f}, U32(Border()));
            drawList->AddRect(origin, {origin.x + popupWidth, origin.y + popupHeight}, U32(BorderStrong()), GetActiveTokens().radii.modal);

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
        }

        struct PresetPopupState {
            bool importerAvailable;
            bool duplicate;
            bool blank;
        };

        PresetPopupState DrawPresetPopupHint(AssetImportModal &modal, const Assets::AssetImportSnapshot &snapshot, const Fonts &fonts,
                                             const std::string &name, const float popupWidth) {
            const auto index = snapshot.selectedItemIndex;
            const bool importerAvailable = !snapshot.items[index].importerContributionId.empty();
            const auto names = modal.PresetNames(index);
            const bool duplicate = std::ranges::find(names, name) != names.end();
            const bool blank = std::ranges::all_of(name, [](const unsigned char character) {
                return std::isspace(character) != 0;
            });
            const auto hint = !importerAvailable
                                  ? Copy(modal.Localized("asset_import.preset_unavailable", "No importer is available for this file."))
                              : duplicate ? Copy(modal.Localized("asset_import.preset_exists", "A preset with this name already exists."))
                                          : Copy(modal.Localized("asset_import.preset_hint", "Save these settings for future imports."));
            ImGui::SetCursorPos({22.0f, 139.0f});
            {
                ScopedTextStyle hintStyle(fonts.sansCompact, TextPx::Caption(), FontPx::SansCompact);
                ImGui::PushTextWrapPos(popupWidth - 22.0f);
                ImGui::TextColored(!importerAvailable || duplicate ? Warn() : Muted(), "%s", hint.c_str());
                ImGui::PopTextWrapPos();
            }
            return {.importerAvailable = importerAvailable, .duplicate = duplicate, .blank = blank};
        }

        void DrawPresetPopupActions(AssetImportModal &modal, const Assets::AssetImportSnapshot &snapshot, const PresetPopupState &state,
                                    std::string &name, const float popupWidth) {
            const auto index = snapshot.selectedItemIndex;
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
                        .enabled = state.importerAvailable && !state.blank && !state.duplicate}) &&
                modal.CreatePreset(index, name)) {
                name.clear();
                ImGui::CloseCurrentPopup();
            }
        }

        void DrawCreatePresetAction(AssetImportModal &modal, const Assets::AssetImportSnapshot &snapshot, const Fonts &fonts) {
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
                DrawPresetPopupHeader(modal, fonts, name, popupWidth, popupHeight);
                ImGui::SetCursorPos({22.0f, 75.0f});
                FieldLabel(Copy(modal.Localized("asset_import.preset_name", "Preset Name")).c_str(), fonts);
                ImGui::SetCursorPosX(22.0f);
                if (ImGui::IsWindowAppearing())
                    ImGui::SetKeyboardFocusHere();
                static_cast<void>(
                    InputTextControl("##PresetName", name, 256, fonts, {.width = (popupWidth - 44.0f) / GetActiveTokens().sizes.uiScale}));
                const PresetPopupState state = DrawPresetPopupHint(modal, snapshot, fonts, name, popupWidth);
                DrawPresetPopupActions(modal, snapshot, state, name, popupWidth);
                ImGui::EndPopup();
            }
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(3);
        }

        void DrawSelectedFileHeader(AssetImportModal &modal, const Assets::AssetImportItem &item, const std::size_t index,
                                    const Fonts &fonts) {
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
            ImGui::TextColored(Muted(), "%s  |  %s", AssetKind(modal, index).c_str(), FileSize(modal, index).c_str());
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
                BeginImportField(Copy(modal.Localized("asset_import.asset_type", "Asset Type")).c_str(), fonts);
                const auto assetType = AssetKind(modal, snapshot.selectedItemIndex);
                const char *assetTypes[]{assetType.c_str()};
                int selectedType = 0;
                static_cast<void>(ComboControl("##ImportAssetType", &selectedType, assetTypes, 1, fonts,
                                               {.leadingIcon = AssetIcon(modal, snapshot.selectedItemIndex)}));
                EndImportField();
                DrawPreset(modal, snapshot, fonts);
                for (const auto &setting : contribution->settings)
                    DrawSetting(modal, setting, snapshot.selectedItemIndex, fonts);
            }
        }

        /** @brief Edits the selected item's advanced import options as one value update. */
        void DrawAdvancedFields(AssetImportModal &modal, const std::size_t itemIndex, const Fonts &fonts) {
            auto options = modal.OptionsFor(itemIndex);
            bool optionsChanged = false;
            std::string name = options.assetName;
            BeginImportField(Copy(modal.Localized("asset_import.asset_name", "Asset Name")).c_str(), fonts);
            if (InputTextControl("##ImportAssetName", name, 256, fonts)) {
                options.assetName = std::move(name);
                optionsChanged = true;
            }
            EndImportField();
            int subfolder = options.folderStrategy;
            const auto byType = Copy(modal.Localized("asset_import.folder.by_type", "By asset type"));
            const auto mirror = Copy(modal.Localized("asset_import.folder.mirror", "Mirror source"));
            const auto flat = Copy(modal.Localized("asset_import.folder.flat", "Flat"));
            const char *folders[]{byType.c_str(), mirror.c_str(), flat.c_str()};
            BeginImportField(Copy(modal.Localized("asset_import.folder_strategy", "Folder Strategy")).c_str(), fonts);
            if (ComboControl("##ImportFolderStrategy", &subfolder, folders, 3, fonts)) {
                options.folderStrategy = subfolder;
                optionsChanged = true;
            }
            EndImportField();
            int idStrategy = options.assetIdStrategy;
            const auto newGuid = Copy(modal.Localized("asset_import.id.new_guid", "New GUID"));
            const auto stableHash = Copy(modal.Localized("asset_import.id.stable_hash", "Stable hash"));
            const char *idStrategies[]{newGuid.c_str(), stableHash.c_str()};
            BeginImportField(Copy(modal.Localized("asset_import.id_strategy", "Asset ID Strategy")).c_str(), fonts);
            if (ComboControl("##ImportAssetIdStrategy", &idStrategy, idStrategies, 2, fonts)) {
                options.assetIdStrategy = idStrategy;
                optionsChanged = true;
            }
            EndImportField();
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
                modal.SetOptionsFor(itemIndex, std::move(options));
        }

        void DrawAdvancedOptions(AssetImportModal &modal, const Assets::AssetImportSnapshot &snapshot, const Fonts &fonts) {
            ImGui::Dummy({0.0f, ImportSectionGap});
            ImGui::SetCursorPosX(PanelPadding);
            if (modal.ConsumeInitialAdvancedState())
                ImGui::SetNextItemOpen(modal.InitialAdvancedOpen(), ImGuiCond_Always);
            if (ImGui::TreeNodeEx(Copy(modal.Localized("asset_import.advanced", "Advanced")).c_str())) {
                ImGui::Unindent();
                ImGui::Dummy({0.0f, ImportSectionGap});
                DrawCreatePresetAction(modal, snapshot, fonts);
                ImGui::Dummy({0.0f, ImportSectionGap});
                DrawAdvancedFields(modal, snapshot.selectedItemIndex, fonts);
                ImGui::Indent();
                ImGui::TreePop();
            }
        }

    }  // namespace

    /** @copydoc DrawAssetImportDetails */
    void DrawAssetImportDetails(AssetImportModal &modal, const Assets::AssetImportSnapshot &snapshot, const Fonts &fonts, float height) {
        using namespace Theme;
        using namespace Ui;
        using namespace AssetImportPresentationDetail;
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
            DrawEditorIcon(ImGui::GetWindowDrawList(), UiIcon::Package, iconPosition, {36.0f, 36.0f}, ImGui::ColorConvertFloat4ToU32(Dim()),
                           fonts.icon);
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

}  // namespace Horo::Editor
