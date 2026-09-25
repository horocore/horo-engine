/** @copydoc AssetImportModalPresentation.h */

#include "AssetImportModalPresentation.h"

#include "AssetImportModalPresentationCommon.h"
#include "AssetImportModalPresentationDetails.h"
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
        using namespace AssetImportPresentationDetail;

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

        /** @brief Draws one destination breadcrumb segment at the vertical center of the bar. */
        void DrawDestinationText(const std::string &value, const ImVec4 color, const Fonts &fonts, float &x, const float centerY) {
            ScopedTextStyle breadcrumbStyle(fonts.sansCompact, TextPx::Body(), FontPx::SansCompact);
            const ImVec2 size = ImGui::CalcTextSize(value.c_str());
            ImGui::SetCursorScreenPos({x, centerY - size.y * 0.5f});
            ImGui::TextColored(color, "%s", value.c_str());
            x += size.x;
        }

        /** @brief Draws project and folder segments inside the destination bar's clip region. */
        void DrawDestinationPath(ImDrawList &drawList, const Fonts &fonts, const std::string &projectName, const std::string &path,
                                 const std::string &assetRootName, float &x, const float centerY) {
            if (!projectName.empty()) {
                DrawEditorIcon(&drawList, UiIcon::AccountTree, {x, centerY - 9.0f}, {18.0f, 18.0f}, ImGui::ColorConvertFloat4ToU32(Muted()),
                               fonts.icon);
                x += 25.0f;
                DrawDestinationText(projectName, Text(), fonts, x, centerY);
            }
            const std::filesystem::path breadcrumb{path};
            auto part = breadcrumb.begin();
            if (!projectName.empty() && part != breadcrumb.end() && *part == assetRootName && std::next(part) != breadcrumb.end())
                ++part;
            const auto firstVisiblePart = part;
            for (; part != breadcrumb.end(); ++part) {
                if (!projectName.empty() || part != firstVisiblePart) {
                    x += 9.0f;
                    DrawDestinationText(">", Dim(), fonts, x, centerY);
                    x += 9.0f;
                }
                DrawDestinationText(part->string(), Text(), fonts, x, centerY);
            }
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
            DrawEditorIcon(drawList, UiIcon::Folder, {x, centerY - 10.0f}, {20.0f, 20.0f}, ImGui::ColorConvertFloat4ToU32(Text()),
                           fonts.icon);
            x += 30.0f;
            DrawDestinationText(label, Dim(), fonts, x, centerY);
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
            DrawDestinationPath(*drawList, fonts, projectName, path, assetRootName, x, centerY);
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

        /** @brief Draws the include and remove controls for one queued source. */
        void DrawFileRowActions(AssetImportModal &modal, const Assets::AssetImportItem &item, const std::size_t index, const Fonts &fonts,
                                const ImVec2 rowMax, const float rowCenterY) {
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
        }

        /** @brief Draws one selectable source row and its diagnostics. */
        void DrawFileRow(AssetImportModal &modal, const Assets::AssetImportSnapshot &snapshot, const std::size_t index,
                         const Fonts &fonts) {
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
                DrawEditorIcon(drawList, HasDiagnostic(item, Assets::ImportDiagnostic::Severity::Error) ? UiIcon::Error : UiIcon::Warning,
                               {rowMax.x - 86.0f, rowCenterY - 10.5f}, {21.0f, 21.0f}, ImGui::ColorConvertFloat4ToU32(tone), fonts.icon);
            }
            DrawFileRowActions(modal, item, index, fonts, rowMax, rowCenterY);
            ImGui::SetCursorScreenPos({rowMin.x, rowMax.y + 5.0f});
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
                DrawFileRow(modal, snapshot, index, fonts);
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

        struct FooterDiagnostics {
            std::size_t warnings{};
            std::size_t errors{};
            const Assets::ImportDiagnostic *first{};
        };

        /** @brief Summarizes visible queue diagnostics without retaining mutable state. */
        [[nodiscard]] FooterDiagnostics SummarizeFooterDiagnostics(const AssetImportModal &modal,
                                                                   const Assets::AssetImportSnapshot &snapshot) {
            FooterDiagnostics summary;
            for (std::size_t index = 0; index < snapshot.items.size(); ++index) {
                if (!modal.IsItemVisible(index))
                    continue;
                const auto &item = snapshot.items[index];
                summary.warnings += HasDiagnostic(item, Assets::ImportDiagnostic::Severity::Warning) ? 1 : 0;
                summary.errors += HasDiagnostic(item, Assets::ImportDiagnostic::Severity::Error) ? 1 : 0;
            }
            for (std::size_t index = 0; index < snapshot.items.size(); ++index) {
                if (!modal.IsItemVisible(index))
                    continue;
                const auto &item = snapshot.items[index];
                for (const auto &diagnostic : item.diagnostics) {
                    if ((summary.errors > 0 && diagnostic.severity == Assets::ImportDiagnostic::Severity::Error) ||
                        (summary.errors == 0 && diagnostic.severity == Assets::ImportDiagnostic::Severity::Warning)) {
                        summary.first = &diagnostic;
                        break;
                    }
                }
                if (summary.first)
                    break;
            }
            return summary;
        }

        /** @brief Draws visible error, warning, or file counts and the first diagnostic. */
        void DrawFooterStatus(AssetImportModal &modal, const Assets::AssetImportSnapshot &snapshot, const Fonts &fonts,
                              const float footerCenterY) {
            ImGui::SetCursorScreenPos({ImGui::GetCursorScreenPos().x, footerCenterY - 9.0f});
            const FooterDiagnostics summary = SummarizeFooterDiagnostics(modal, snapshot);
            if (summary.errors > 0 || summary.warnings > 0) {
                const auto tone = summary.errors > 0 ? ErrorColor : WarningColor;
                const ImVec2 iconPosition = ImGui::GetCursorScreenPos();
                DrawEditorIcon(ImGui::GetWindowDrawList(), summary.errors > 0 ? UiIcon::Error : UiIcon::Warning, iconPosition,
                               {18.0f, 18.0f}, ImGui::ColorConvertFloat4ToU32(tone), fonts.icon);
                ImGui::Dummy({20.0f, 18.0f});
                ImGui::SameLine(0.0f, 8.0f);
            }
            if (summary.errors > 0)
                ImGui::TextColored(ErrorColor, "%zu %s", summary.errors,
                                   Copy(modal.Localized(summary.errors == 1 ? "asset_import.error" : "asset_import.errors",
                                                        summary.errors == 1 ? "error" : "errors"))
                                       .c_str());
            else if (summary.warnings > 0)
                ImGui::TextColored(WarningColor, "%zu %s", summary.warnings,
                                   Copy(modal.Localized(summary.warnings == 1 ? "asset_import.warning" : "asset_import.warnings",
                                                        summary.warnings == 1 ? "warning" : "warnings"))
                                       .c_str());
            else
                ImGui::TextColored(Dim(), "%zu %s", modal.VisibleItemCount(), Copy(modal.Localized("asset_import.files", "files")).c_str());
            if (summary.first) {
                ImGui::SameLine(0.0f, 14.0f);
                const ImVec2 divider = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddLine({divider.x, divider.y}, {divider.x, divider.y + 18.0f},
                                                    ImGui::ColorConvertFloat4ToU32(Border()));
                ImGui::Dummy({1.0f, 18.0f});
                ImGui::SameLine(0.0f, 14.0f);
                ImGui::TextColored(Dim(), "%.55s", summary.first->message.c_str());
            }
        }

        /** @brief Draws completion, cancellation, and import actions. */
        void DrawFooterActions(AssetImportModal &modal, const float footerCenterY, ModalFrameResult &result) {
            const float actionHeight = 34.0f;
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
        }

        void DrawFooter(AssetImportModal &modal, const Assets::AssetImportSnapshot &snapshot, const Fonts &fonts, ScopedModalShell &shell,
                        ModalFrameResult &result) {
            shell.BeginFooter({24.0f, 0.0f}, true);
            const float footerCenterY = ImGui::GetWindowPos().y + ImGui::GetWindowHeight() * 0.5f;
            DrawFooterStatus(modal, snapshot, fonts, footerCenterY);
            DrawFooterActions(modal, footerCenterY, result);
            shell.EndFooter();
        }
    }  // namespace

    ModalFrameResult DrawAssetImportModalPresentation(AssetImportModal &modal, const Fonts &fonts) {
        using namespace AssetImportPresentationDetail;
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
        DrawAssetImportDetails(modal, snapshot, fonts, height);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        DrawFooter(modal, snapshot, fonts, shell, result);
        return result;
    }
}  // namespace Horo::Editor
