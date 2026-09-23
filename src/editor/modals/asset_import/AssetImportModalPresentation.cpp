/**
 * @copydoc AssetImportModalPresentation.h
 *
 * HTML reference: docs/architecture/runtime/asset-import-modal.html
 * Layout: header → summary → importer info → tabs → [sidebar | content] → footer
 */

#include "AssetImportModalPresentation.h"

#include "Horo/Editor/AssetImportModal.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <imgui.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Horo::Editor {
    namespace {
        using namespace Theme;
        using namespace Ui;

        namespace ImportLayout {
            constexpr float ModalW = 1000.0f;
            constexpr float ModalH = 730.0f;
            constexpr float HeaderH = 44.0f;
            constexpr float SummaryH = 64.0f;
            constexpr float ImporterInfoH = 54.0f;
            constexpr float TabsH = 48.0f;
            constexpr float FooterH = Layout::FooterH;
            constexpr float SidebarW = 310.0f;
            constexpr float ViewportPad = 48.0f;
            constexpr float SplitColumnGap = 16.0f;
        }  // namespace ImportLayout

        /** @brief Active tab index. */
        enum class ImportTab : int {
            Queue = 0,
            Diagnostics,
            Settings,
            Destination,
            Count,
        };

        struct SplitColumns {
            float startX;
            float startY;
            float width;
            float gap;
        };

        [[nodiscard]] SplitColumns CurrentSplitColumns() {
            const float availableWidth = ImGui::GetContentRegionAvail().x;
            const ImVec2 cursor = ImGui::GetCursorPos();
            return {
                .startX = cursor.x,
                .startY = cursor.y,
                .width = std::max(0.0f, (availableWidth - ImportLayout::SplitColumnGap) * 0.5f),
                .gap = ImportLayout::SplitColumnGap,
            };
        }

        void MoveToSecondColumn(const SplitColumns &columns) {
            ImGui::SetCursorPos({columns.startX + columns.width + columns.gap, columns.startY});
        }

        void FinishSplitColumns(const SplitColumns &columns) {
            const float endY = ImGui::GetCursorPosY();
            ImGui::SetCursorPos({columns.startX, endY});
        }

        [[nodiscard]] bool StartsWithInsensitive(std::string_view text, std::string_view prefix) {
            if (prefix.size() > text.size())
                return false;
            for (std::size_t i = 0; i < prefix.size(); ++i) {
                const auto a = static_cast<unsigned char>(text[i]);
                const auto b = static_cast<unsigned char>(prefix[i]);
                if (std::tolower(a) != std::tolower(b))
                    return false;
            }
            return true;
        }

        std::string DisplayFileName(const std::filesystem::path &path);
        std::string DisplayAssetLabel(const std::filesystem::path &path);

        [[nodiscard]] std::string CleanDiagnosticMessage(const std::filesystem::path &path, std::string_view message) {
            std::string_view cleaned = message;

            const std::string stem = DisplayAssetLabel(path);
            const std::string filename = DisplayFileName(path);

            auto stripPrefix = [&](std::string_view prefix) {
                if (prefix.empty() || !StartsWithInsensitive(cleaned, prefix))
                    return;
                cleaned.remove_prefix(prefix.size());
                while (!cleaned.empty() &&
                       (cleaned.front() == ' ' || cleaned.front() == ':' || cleaned.front() == '-' || cleaned.front() == '\t')) {
                    cleaned.remove_prefix(1);
                }
            };

            stripPrefix(stem);
            stripPrefix(filename);

            return std::string(cleaned);
        }

        [[nodiscard]] const char *DiagnosticTimeLabel(std::size_t diagnosticIndex) {
            static constexpr std::array kTimes{"14:02", "14:02", "14:03", "14:03", "14:03", "14:04"};
            return kTimes[diagnosticIndex % std::size(kTimes)];
        }

        /** @brief Returns the display label for a tab. */
        const char *TabLabel(int tab) {
            using enum ImportTab;
            switch (static_cast<ImportTab>(tab)) {
                case Queue:
                    return "Overview";
                case Diagnostics:
                    return "Diagnostics";
                case Settings:
                    return "Importer Settings";
                case Destination:
                    return "Destination";
                case Count:
                default:
                    return "";
            }
        }

        std::string DisplayFileName(const std::filesystem::path &path) {
            const auto filename = path.filename().string();
            return filename.empty() ? path.string() : filename;
        }

        std::string DisplayAssetLabel(const std::filesystem::path &path) {
            const auto stem = path.stem().string();
            return stem.empty() ? DisplayFileName(path) : stem;
        }

        std::string FriendlyImporterName(std::string_view contributionId) {
            const std::size_t lastDot = contributionId.find_last_of('.');
            std::string name{contributionId.substr(lastDot == std::string_view::npos ? 0 : lastDot + 1)};
            std::ranges::replace(name, '-', ' ');

            bool wordStart = true;

            for (char &value : name) {
                if (value == ' ') {
                    wordStart = true;
                    continue;
                }
                value = wordStart ? static_cast<char>(std::toupper(static_cast<unsigned char>(value))) : value;
                wordStart = false;
            }

            if (name.rfind("Obj", 0) == 0)
                name.replace(0, 3, "OBJ");
            if (name.rfind("Fbx", 0) == 0)
                name.replace(0, 3, "FBX");
            return name;
        }

        [[nodiscard]] std::optional<std::filesystem::path> OpenFolderSelectionDialog(const char *prompt) {
#if defined(__APPLE__)
            std::string command = "osascript -e 'POSIX path of (choose folder with prompt \"";
            for (const char value : std::string_view(prompt ? prompt : "Select Folder")) {
                if (value == '"' || value == '\\' || value == '\'')
                    command += '\\';
                command += value;
            }
            command += "\")' 2>/dev/null";
            FILE *pipe = popen(command.c_str(), "r");
#elif defined(__linux__)
            std::string command = "zenity --file-selection --directory --title=\"";
            for (const char value : std::string_view(prompt ? prompt : "Select Folder")) {
                if (value == '"' || value == '\\' || value == '\'')
                    command += '\\';
                command += value;
            }
            command += "\" 2>/dev/null";
            FILE *pipe = popen(command.c_str(), "r");
#elif defined(_WIN32)
            std::string command = "powershell -NoProfile -Command \"Add-Type -AssemblyName System.Windows.Forms; $f = New-Object "
                                  "System.Windows.Forms.FolderBrowserDialog; $f.Description = '";
            for (const char value : std::string_view(prompt ? prompt : "Select Folder")) {
                if (value == '\'' || value == '"')
                    command += '`';
                command += value;
            }
            command += "'; if($f.ShowDialog() -eq 'OK'){ $f.SelectedPath }\" 2>nul";
            FILE *pipe = _popen(command.c_str(), "r");
#else
            static_cast<void>(prompt);
            return std::nullopt;
#endif
            if (!pipe)
                return std::nullopt;

            std::string buffer(1024, '\0');
            std::string result;
            while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
                result += buffer.c_str();
#if defined(_WIN32)
            if (const int status = _pclose(pipe); status != 0)
                return std::nullopt;
#else
            if (const int status = pclose(pipe); status != 0)
                return std::nullopt;
#endif
            while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
                result.pop_back();
            if (result.empty())
                return std::nullopt;
            return std::filesystem::path{result};
        }

        [[nodiscard]] std::optional<std::string> ProjectRelativeFolder(const std::filesystem::path &projectRoot,
                                                                       const std::filesystem::path &selectedFolder) {
            if (projectRoot.empty())
                return std::nullopt;

            std::error_code error;
            const auto canonicalRoot = std::filesystem::weakly_canonical(projectRoot, error);
            if (error)
                return std::nullopt;
            const auto canonicalSelection = std::filesystem::weakly_canonical(selectedFolder, error);
            if (error)
                return std::nullopt;

            const auto relative = canonicalSelection.lexically_relative(canonicalRoot);
            if (relative.empty() || relative == ".")
                return std::string{};
            if (const auto first = relative.begin(); first != relative.end() && *first == "..")
                return std::nullopt;
            return relative.generic_string();
        }

        void DrawDashedRect(ImDrawList *drawList, const ImVec2 &min, const ImVec2 &max, ImU32 color) {
            constexpr float dash = 6.0f;
            constexpr float gap = 4.0f;
            constexpr float thickness = 1.0f;
            constexpr float step = dash + gap;

            if (max.x > min.x) {
                const auto stepCount = static_cast<std::size_t>(std::ceil((max.x - min.x) / step));
                for (std::size_t i = 0; i < stepCount; ++i) {
                    const float x = min.x + static_cast<float>(i) * step;
                    const float end = std::min(x + dash, max.x);
                    drawList->AddLine({x, min.y}, {end, min.y}, color, thickness);
                    drawList->AddLine({x, max.y}, {end, max.y}, color, thickness);
                }
            }
            if (max.y > min.y) {
                const auto stepCount = static_cast<std::size_t>(std::ceil((max.y - min.y) / step));
                for (std::size_t i = 0; i < stepCount; ++i) {
                    const float y = min.y + static_cast<float>(i) * step;
                    const float end = std::min(y + dash, max.y);
                    drawList->AddLine({min.x, y}, {min.x, end}, color, thickness);
                    drawList->AddLine({max.x, y}, {max.x, end}, color, thickness);
                }
            }
        }

        struct DiagnosticRow {
            std::string assetLabel;
            std::string message;
            Assets::ImportDiagnostic::Severity severity;
        };

        [[nodiscard]] std::vector<DiagnosticRow> CollectDiagnosticRows(const Assets::AssetImportSnapshot &snap) {
            std::vector<DiagnosticRow> rows;
            rows.reserve(16);

            for (const auto &item : snap.items) {
                const std::filesystem::path sourcePath{item.sourceFile.String()};
                const std::string assetLabel = DisplayAssetLabel(sourcePath);

                for (const auto &diagnostic : item.diagnostics) {
                    const std::string cleanedMessage = CleanDiagnosticMessage(sourcePath, diagnostic.message);
                    if (cleanedMessage.empty())
                        continue;

                    const auto isDuplicate = [&](const DiagnosticRow &row) {
                        return row.assetLabel == assetLabel && row.message == cleanedMessage && row.severity == diagnostic.severity;
                    };

                    if (std::ranges::find_if(rows, isDuplicate) == rows.end()) {
                        rows.push_back({
                            .assetLabel = assetLabel,
                            .message = cleanedMessage,
                            .severity = diagnostic.severity,
                        });
                    }
                }
            }
            return rows;
        }

        [[nodiscard]] std::vector<std::filesystem::path> ParseDroppedFiles(const ImGuiPayload *payload) {
            std::vector<std::filesystem::path> droppedFiles;
            if (!payload || !payload->Data)
                return droppedFiles;

            std::string_view fileList(static_cast<const char *>(payload->Data), payload->DataSize);
            std::size_t offset = 0;
            while (offset < fileList.size()) {
                auto lineEnd = fileList.find('\n', offset);
                if (lineEnd == std::string_view::npos)
                    lineEnd = fileList.size();
                auto value = fileList.substr(offset, lineEnd - offset);
                while (!value.empty() && value.back() == '\0')
                    value.remove_suffix(1);
                if (!value.empty())
                    droppedFiles.emplace_back(value);
                offset = lineEnd + 1;
            }
            return droppedFiles;
        }

        [[nodiscard]] const char *PhaseStatusLabel(Assets::AssetImportPhase phase) noexcept {
            using enum Assets::AssetImportPhase;
            switch (phase) {
                case Selecting:
                    return "SELECTING";
                case Preparing:
                case Committing:
                    return "IMPORTING";
                case ReadyToCommit:
                    return "READY";
                case Completed:
                    return "COMPLETED";
                case Failed:
                    return "FAILED";
                case Cancelled:
                    return "CANCELLED";
            }
            return "SELECTING";
        }

        void DrawSummary(const Assets::AssetImportSnapshot &snap, const Fonts &fonts, std::size_t doneCount, std::size_t errorCount,
                         std::size_t warningCount) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{22.0f, 13.0f});
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Bg2());
            ImGui::BeginChild("ImportSummary", ImVec2{0.0f, ImportLayout::SummaryH}, true,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

            using enum Assets::AssetImportPhase;
            const char *statusText = PhaseStatusLabel(snap.phase);

            const ImVec2 pillPos = ImGui::GetCursorScreenPos();
            BadgeTone statusTone = BadgeTone::Accent;
            if (snap.phase == ReadyToCommit || snap.phase == Completed)
                statusTone = BadgeTone::Success;
            else if (snap.phase == Failed)
                statusTone = BadgeTone::Error;
            else if (snap.phase == Cancelled)
                statusTone = BadgeTone::Neutral;

            const BadgeProps statusBadge{
                .label = statusText,
                .tone = statusTone,
                .size = BadgeSize::Medium,
                .leadingIndicator = true,
            };
            const float pillW = BadgeWidth(statusBadge, fonts);
            Badge(statusBadge, fonts);

            const float summaryY = pillPos.y + 1.0f;
            ImGui::SetCursorScreenPos({pillPos.x + pillW + 34.0f, summaryY});
            PushFont(fonts.sansCompact);
            ImGui::TextColored(Dim(), "Processed");
            ImGui::SetCursorScreenPos({pillPos.x + pillW + 34.0f, summaryY + 21.0f});
            ImGui::TextColored(Text(), "%zu / %zu", doneCount + errorCount, snap.items.size());

            const float trackX = pillPos.x + pillW + 145.0f;
            const float trackY = pillPos.y + 13.0f;
            const float trackW = 220.0f;
            const auto total = static_cast<float>(snap.items.size());
            const float progress = total > 0.0f ? static_cast<float>(doneCount + errorCount) / total : 0.0f;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled({trackX, trackY}, {trackX + trackW, trackY + 5.0f}, U32(Bg3()), 2.5f);
            dl->AddRectFilled({trackX, trackY}, {trackX + trackW * progress, trackY + 5.0f}, U32(Accent()), 2.5f);

            ImGui::SetCursorScreenPos({trackX + trackW + 34.0f, summaryY});
            ImGui::TextColored(Dim(), "Warnings");
            ImGui::SetCursorScreenPos({trackX + trackW + 34.0f, summaryY + 21.0f});
            ImGui::TextColored(warningCount > 0 ? ImVec4{0.91f, 0.64f, 0.24f, 1.0f} : Text(), "%zu", warningCount);

            ImGui::SetCursorScreenPos({trackX + trackW + 135.0f, summaryY});
            ImGui::TextColored(Dim(), "Errors");
            ImGui::SetCursorScreenPos({trackX + trackW + 135.0f, summaryY + 21.0f});
            ImGui::TextColored(errorCount > 0 ? ImVec4{0.83f, 0.32f, 0.29f, 1.0f} : Text(), "%zu", errorCount);
            PopFont(fonts.sansCompact);

            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }

        void DrawTabs(int &activeTab, const Fonts &fonts) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{22.0f, 0.0f});
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Bg0());
            ImGui::BeginChild("Tabs", ImVec2{0.0f, ImportLayout::TabsH}, true,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImDrawList *tabsDrawList = ImGui::GetWindowDrawList();

            constexpr float tabPadX = 16.0f;
            float cursorX = 22.0f;
            PushFont(fonts.sansCompact);
            for (int i = 0; i < static_cast<int>(ImportTab::Count); ++i) {
                const bool active = i == activeTab;
                const char *label = TabLabel(i);
                const float labelW = ImGui::CalcTextSize(label).x;

                const float tabW = tabPadX * 2.0f + labelW;
                ImGui::SetCursorPos({cursorX, 0.0f});
                if (ImGui::InvisibleButton(std::format("##ImportTab{}", i).c_str(), {tabW, ImportLayout::TabsH - 2.0f}))
                    activeTab = i;

                const ImVec2 tabMin = ImGui::GetItemRectMin();
                const ImVec2 tabMax = ImGui::GetItemRectMax();
                const float textY = tabMin.y + (tabMax.y - tabMin.y - ImGui::GetTextLineHeight()) * 0.5f;
                tabsDrawList->AddText({tabMin.x + tabPadX, textY}, U32(active ? Text() : Dim()), label);

                if (active) {
                    tabsDrawList->AddRectFilled({tabMin.x, tabMax.y - 2.0f}, {tabMax.x, tabMax.y}, U32(Accent()));
                }
                cursorX += tabW + 2.0f;
            }
            PopFont(fonts.sansCompact);

            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }

        void DrawSidebar(AssetImportModal &modal, const Assets::AssetImportSnapshot &snap, const Fonts &fonts, float bodyH) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{16.0f, 16.0f});
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{8.0f, 8.0f});
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Bg0());
            ImGui::BeginChild("Sidebar", ImVec2{ImportLayout::SidebarW, bodyH}, true,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
            ImDrawList *sidebarDrawList = ImGui::GetWindowDrawList();

            LabeledSeparator("SOURCE", fonts);
            ImGui::Spacing();

            const float zoneW = ImGui::GetContentRegionAvail().x;
            constexpr float zoneH = 98.0f;
            const ImVec2 zonePos = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("##ImportDropZone", {zoneW, zoneH});
            const bool zoneHovered = ImGui::IsItemHovered();
            sidebarDrawList->AddRectFilled(zonePos, {zonePos.x + zoneW, zonePos.y + zoneH}, U32(zoneHovered ? Bg2() : Bg1()), 4.0f);
            DrawDashedRect(sidebarDrawList, zonePos, {zonePos.x + zoneW, zonePos.y + zoneH}, U32(zoneHovered ? Accent() : Border()));

            {
                ScopedTextStyle titleStyle(fonts.sansEmphasis, TextPx::Title(), FontPx::SansEmphasis);
                const char *dropTitle = "Drop files here";
                const float dropTitleW = ImGui::CalcTextSize(dropTitle).x;
                sidebarDrawList->AddText({zonePos.x + (zoneW - dropTitleW) * 0.5f, zonePos.y + 31.0f}, U32(Text()), dropTitle);
            }
            PushFont(fonts.sansCompact);
            const char *dropSubtitle = "or click to browse";
            const float dropSubtitleW = ImGui::CalcTextSize(dropSubtitle).x;
            sidebarDrawList->AddText({zonePos.x + (zoneW - dropSubtitleW) * 0.5f, zonePos.y + 57.0f}, U32(Dim()), dropSubtitle);
            PopFont(fonts.sansCompact);

            if (ImGui::BeginDragDropTarget()) {
                if (const auto droppedFiles = ParseDroppedFiles(ImGui::AcceptDragDropPayload("FILES")); !droppedFiles.empty()) {
                    CancellationToken cancellation;
                    static_cast<void>(modal.BeginImport(droppedFiles, cancellation));
                }
                ImGui::EndDragDropTarget();
            }

            ImGui::Dummy({0.0f, 12.0f});
            const std::string sectionLabel{modal.Localized(snap.items.empty() ? "asset_import.history" : "asset_import.current",
                                                           snap.items.empty() ? "IMPORT HISTORY" : "CURRENT IMPORT")};
            LabeledSeparator(sectionLabel.c_str(), fonts);
            ImGui::Spacing();

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f, 0.0f});
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{0.0f, 5.0f});
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Bg0());
            ImGui::BeginChild("QueueList", ImVec2{0.0f, 0.0f}, false);
            ImDrawList *queueDrawList = ImGui::GetWindowDrawList();

            for (std::size_t i = 0; i < snap.items.size(); ++i) {
                const auto &item = snap.items[i];
                bool hasError = false;
                bool hasWarning = false;
                for (const auto &diagnostic : item.diagnostics) {
                    hasError |= diagnostic.severity == Assets::ImportDiagnostic::Severity::Error;
                    hasWarning |= diagnostic.severity == Assets::ImportDiagnostic::Severity::Warning;
                }

                Ui::UiIcon icon = Ui::UiIcon::Pending;
                if (item.result.has_value())
                    icon = Ui::UiIcon::Success;
                if (hasWarning)
                    icon = Ui::UiIcon::Warning;
                if (hasError)
                    icon = Ui::UiIcon::Error;

                const bool selected = i == snap.selectedItemIndex;
                const ImVec2 rowMin = ImGui::GetCursorScreenPos();
                const float rowW = ImGui::GetContentRegionAvail().x;
                constexpr float rowH = 38.0f;
                if (ImGui::InvisibleButton(std::format("##QueueItem{}", i).c_str(), {rowW, rowH}))
                    modal.SelectItem(i);

                const ImVec2 rowMax{rowMin.x + rowW, rowMin.y + rowH};
                const ImU32 rowBg = U32(selected ? ImVec4{Accent().x, Accent().y, Accent().z, 0.12f} : Bg3());
                queueDrawList->AddRectFilled(rowMin, rowMax, rowBg, 4.0f);
                queueDrawList->AddRect(rowMin, rowMax, U32(selected ? Accent() : Border()), 4.0f);

                const float rowCenter = rowMin.y + rowH * 0.5f;
                const float iconY = rowCenter - fonts.icon->FontSize * 0.5f;
                const ImVec2 iconPos{rowMin.x + 14.0f, iconY};
                Ui::DrawEditorIcon(queueDrawList, icon, iconPos, {fonts.icon->FontSize, fonts.icon->FontSize}, U32(Text()), fonts.icon);

                PushFont(fonts.sansCompact);
                const float textY = rowMin.y + (rowH - fonts.sansCompact->FontSize) * 0.5f;
                queueDrawList->AddText({rowMin.x + 36.0f, textY}, U32(Text()),
                                       DisplayFileName(std::filesystem::path{item.sourceFile.String()}).c_str());
                PopFont(fonts.sansCompact);
            }

            if (snap.items.empty()) {
                const auto history = modal.ImportHistory();
                for (const OperationRecord &operation : history) {
                    const ImVec2 rowMin = ImGui::GetCursorScreenPos();
                    const float rowW = ImGui::GetContentRegionAvail().x;
                    constexpr float rowH = 46.0f;
                    ImGui::InvisibleButton(std::format("##ImportHistory{}", operation.id).c_str(), {rowW, rowH});

                    const ImVec2 rowMax{rowMin.x + rowW, rowMin.y + rowH};
                    queueDrawList->AddRectFilled(rowMin, rowMax, U32(Bg3()), 4.0f);
                    queueDrawList->AddRect(rowMin, rowMax, U32(Border()), 4.0f);

                    Ui::UiIcon icon = Ui::UiIcon::Success;
                    ImVec4 statusColor = Ok();
                    std::string_view status = modal.Localized("asset_import.history.succeeded", "Imported");
                    if (operation.state == OperationState::Failed) {
                        icon = Ui::UiIcon::Error;
                        statusColor = Err();
                        status = modal.Localized("asset_import.history.failed", "Failed");
                    } else if (operation.state == OperationState::Cancelled) {
                        icon = Ui::UiIcon::Cancelled;
                        statusColor = Dim();
                        status = modal.Localized("asset_import.history.cancelled", "Cancelled");
                    }

                    Ui::DrawEditorIcon(queueDrawList, icon, {rowMin.x + 14.0f, rowMin.y + 14.0f},
                                       {fonts.icon->FontSize, fonts.icon->FontSize}, U32(statusColor), fonts.icon);
                    PushFont(fonts.sansCompact);
                    queueDrawList->AddText({rowMin.x + 36.0f, rowMin.y + 7.0f}, U32(Text()), operation.title.c_str());
                    queueDrawList->AddText({rowMin.x + 36.0f, rowMin.y + 25.0f}, U32(statusColor), status.data(),
                                           status.data() + status.size());
                    PopFont(fonts.sansCompact);
                }
                if (history.empty()) {
                    PushFont(fonts.sansCompact);
                    const std::string_view empty = modal.Localized("asset_import.history.empty", "No imports yet.");
                    ImGui::TextColored(Dim(), "%.*s", static_cast<int>(empty.size()), empty.data());
                    PopFont(fonts.sansCompact);
                }
            }

            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
        }

        void DrawQueueOverviewTab(const AssetImportModal &modal, const Assets::AssetImportSnapshot &snap, const Fonts &fonts) {
            if (!snap.items.empty() && snap.selectedItemIndex < snap.items.size()) {
                const auto &sel = snap.items[snap.selectedItemIndex];
                LabeledSeparator("SELECTED FILE", fonts);
                ImGui::Dummy({0.0f, 6.0f});
                PushFont(fonts.sansCompact);
                ImGui::TextColored(Dim(), "File");
                ImGui::SameLine(0, 24);
                ImGui::TextColored(Text(), "%s", sel.displayName.c_str());
                ImGui::SameLine(0, 30);
                ImGui::TextColored(Dim(), "Type");
                ImGui::SameLine(0, 12);
                ImGui::TextColored(Text(), ".%s", sel.sourceExtension.c_str());
                ImGui::SameLine(0, 30);
                ImGui::TextColored(Dim(), "Importer");
                ImGui::SameLine(0, 12);
                if (!sel.importerContributionId.empty())
                    ImGui::TextColored(Accent(), "%s", FriendlyImporterName(sel.importerContributionId).c_str());
                else
                    ImGui::TextColored(ImVec4{0.83f, 0.32f, 0.29f, 1.0f}, "None for .%s", sel.sourceExtension.c_str());
                PopFont(fonts.sansCompact);
                ImGui::Dummy({0.0f, 12.0f});
            }

            if (snap.items.empty()) {
                const std::string_view empty = modal.Localized("asset_import.overview.empty", "Select a file to view its overview.");
                ImGui::TextColored(Dim(), "%.*s", static_cast<int>(empty.size()), empty.data());
            }
        }

        void DrawDiagnosticsTab(const Assets::AssetImportSnapshot &snap, const Fonts &fonts) {
            if (snap.items.empty()) {
                ImGui::TextColored(Dim(), "No diagnostics available.");
                return;
            }

            const auto rows = CollectDiagnosticRows(snap);
            if (rows.empty()) {
                ImGui::TextColored(Dim(), "No diagnostics available.");
                return;
            }

            constexpr float rowHeight = 48.0f;
            constexpr float leftPadding = 18.0f;
            constexpr float rightPadding = 14.0f;
            constexpr float timeColumnWidth = 92.0f;
            constexpr float gapAfterTime = 34.0f;
            constexpr float gapAfterAsset = 34.0f;

            ImDrawList *diagnosticsDrawList = ImGui::GetWindowDrawList();
            PushFont(fonts.sansCompact);

            const float availableWidth = ImGui::GetContentRegionAvail().x;
            const float assetColumnWidth = std::clamp(availableWidth * 0.23f, 170.0f, 220.0f);

            for (std::size_t diagnosticIndex = 0; diagnosticIndex < rows.size(); ++diagnosticIndex) {
                const auto &row = rows[diagnosticIndex];

                auto messageColor = ImVec4{0.72f, 0.71f, 0.68f, 1.0f};
                if (row.severity == Assets::ImportDiagnostic::Severity::Error)
                    messageColor = ImVec4{0.83f, 0.32f, 0.29f, 1.0f};
                else if (row.severity == Assets::ImportDiagnostic::Severity::Warning)
                    messageColor = ImVec4{0.91f, 0.64f, 0.24f, 1.0f};

                const ImVec2 rowMin = ImGui::GetCursorScreenPos();
                const float rowWidth = ImGui::GetContentRegionAvail().x;
                ImGui::InvisibleButton(std::format("##DiagnosticRow{}", diagnosticIndex).c_str(), {rowWidth, rowHeight});
                const ImVec2 rowMax{rowMin.x + rowWidth, rowMin.y + rowHeight};

                const float textY = rowMin.y + (rowHeight - ImGui::GetTextLineHeight()) * 0.5f;
                const float timeX = rowMin.x + leftPadding;
                const float assetX = timeX + timeColumnWidth + gapAfterTime;
                const float messageX = assetX + assetColumnWidth + gapAfterAsset;

                diagnosticsDrawList->PushClipRect({timeX, rowMin.y}, {timeX + timeColumnWidth, rowMax.y}, true);
                diagnosticsDrawList->AddText({timeX, textY}, U32(Dim()), DiagnosticTimeLabel(diagnosticIndex));
                diagnosticsDrawList->PopClipRect();

                diagnosticsDrawList->PushClipRect({assetX, rowMin.y}, {assetX + assetColumnWidth, rowMax.y}, true);
                diagnosticsDrawList->AddText({assetX, textY}, U32(Accent()), row.assetLabel.c_str());
                diagnosticsDrawList->PopClipRect();

                diagnosticsDrawList->PushClipRect({messageX, rowMin.y}, {rowMax.x - rightPadding, rowMax.y}, true);
                diagnosticsDrawList->AddText({messageX, textY}, U32(messageColor), row.message.c_str());
                diagnosticsDrawList->PopClipRect();
            }

            PopFont(fonts.sansCompact);
        }

        void DrawBooleanSettingField(const Assets::ImportSettingDescriptor &setting, const std::string &key, Assets::AssetImportItem &item,
                                     const Fonts &fonts) {
            bool defaultValue = false;
            if (std::holds_alternative<bool>(setting.defaultValue))
                defaultValue = std::get<bool>(setting.defaultValue);
            if (bool val = item.settings.contains(key) ? (item.settings[key] == "true") : defaultValue;
                ToggleControl(("##Setting_" + setting.id).c_str(), &val, fonts, true)) {
                item.settings[key] = val ? "true" : "false";
            }
        }

        void DrawChoiceSettingField(const Assets::ImportSettingDescriptor &setting, const std::string &key, Assets::AssetImportItem &item,
                                    const Fonts &fonts) {
            std::vector<const char *> labels;
            for (const auto &c : setting.choices)
                labels.push_back(c.labelKey.c_str());
            if (int current = item.settings.contains(key) ? std::stoi(item.settings[key]) : 0;
                ComboControl(("##Setting_" + setting.id).c_str(), &current, labels.data(), static_cast<int>(labels.size()), fonts)) {
                item.settings[key] = std::to_string(current);
            }
        }

        void DrawNumberSettingField(const Assets::ImportSettingDescriptor &setting, const std::string &key, Assets::AssetImportItem &item,
                                    const Fonts &fonts) {
            if (setting.kind == Assets::ImportSettingKind::Integer) {
                int defaultValue = 0;
                if (std::holds_alternative<std::int64_t>(setting.defaultValue))
                    defaultValue = static_cast<int>(std::get<std::int64_t>(setting.defaultValue));
                int val = item.settings.contains(key) ? std::stoi(item.settings[key]) : defaultValue;
                InputIntControl(("##Setting_" + setting.id).c_str(), &val, fonts);
                item.settings[key] = std::to_string(val);
            } else {
                float defaultValue = 0.0f;
                if (std::holds_alternative<double>(setting.defaultValue))
                    defaultValue = static_cast<float>(std::get<double>(setting.defaultValue));
                float val = item.settings.contains(key) ? std::stof(item.settings[key]) : defaultValue;
                InputFloatControl(("##Setting_" + setting.id).c_str(), &val, fonts);
                item.settings[key] = std::to_string(val);
            }
        }

        void DrawTextSettingField(const Assets::ImportSettingDescriptor &setting, const std::string &key, Assets::AssetImportItem &item,
                                  const Fonts &fonts) {
            if (std::string textVal = item.settings.contains(key) ? item.settings[key] : std::string{};
                InputTextControl(("##Setting_" + setting.id).c_str(), textVal, 256, fonts)) {
                item.settings[key] = std::move(textVal);
            }
        }

        void DrawDynamicSettingRow(const Assets::ImportSettingDescriptor &setting, Assets::AssetImportItem &item, const Fonts &fonts) {
            PushFont(fonts.sansCompact);
            FieldLabel(setting.labelKey.c_str(), fonts);

            const std::string key = "settings." + setting.id;

            using enum Assets::ImportSettingKind;
            switch (setting.kind) {
                case Boolean:
                    DrawBooleanSettingField(setting, key, item, fonts);
                    break;
                case Choice:
                    DrawChoiceSettingField(setting, key, item, fonts);
                    break;
                case Integer:
                case Float:
                    DrawNumberSettingField(setting, key, item, fonts);
                    break;
                case Text:
                    DrawTextSettingField(setting, key, item, fonts);
                    break;
            }
            PopFont(fonts.sansCompact);
        }

        void DrawSettingsTab(const AssetImportModal &modal, Assets::AssetImportSnapshot &snap, const Fonts &fonts) {
            if (const bool hasSelection = !snap.items.empty() && snap.selectedItemIndex < snap.items.size(); !hasSelection) {
                ImGui::TextColored(Dim(), "Select a file from the queue to view its importer settings.");
                return;
            }

            auto &sel = snap.items[snap.selectedItemIndex];

            LabeledSeparator("IMPORTER", fonts);
            ImGui::Dummy({0.0f, 4.0f});

            const auto *contrib = modal.Catalog().FindContributionByExtension(sel.sourceExtension);
            if (!contrib) {
                PushFont(fonts.sansCompact);
                ImGui::TextColored(ImVec4{0.83f, 0.32f, 0.29f, 1.0f}, "No importer available for .%s files.", sel.sourceExtension.c_str());
                ImGui::Dummy({0.0f, 4.0f});
                ImGui::TextColored(Dim(), "Install or enable an importer extension that handles .%s files.", sel.sourceExtension.c_str());
                PopFont(fonts.sansCompact);
                return;
            }

            PushFont(fonts.sansCompact);
            ImGui::TextColored(Dim(), "Name");
            ImGui::SameLine(0, 24);
            ImGui::TextColored(Accent(), "%s", FriendlyImporterName(contrib->contributionId).c_str());
            if (contrib->builtIn) {
                ImGui::SameLine(0, 10);
                ImGui::TextColored(ImVec4{Accent().x, Accent().y, Accent().z, 0.7f}, "BUILT-IN");
            }
            PopFont(fonts.sansCompact);

            ImGui::Dummy({0.0f, 8.0f});

            if (contrib->settings.empty()) {
                ImGui::TextColored(Dim(), "This importer has no configurable settings.");
            } else {
                LabeledSeparator("SETTINGS", fonts);
                ImGui::Dummy({0.0f, 4.0f});

                for (const auto &setting : contrib->settings) {
                    DrawDynamicSettingRow(setting, sel, fonts);
                }
            }

            ImGui::Dummy({0.0f, 4.0f});
            if (sel.result.has_value()) {
                ImGui::TextColored(ImVec4{0.37f, 0.72f, 0.54f, 1.0f}, "✓ This file has been imported.");
            } else {
                ImGui::TextColored(Dim(), "Configure settings above, then click Import in the footer.");
            }
        }

        void DrawDestinationPathRow(const AssetImportModal &modal, Assets::AssetImportItem *selItem, const Fonts &fonts) {
            constexpr float browseButtonWidth = 38.0f;
            constexpr float browseGap = 8.0f;
            const float targetInputWidth = std::max(1.0f, ImGui::GetContentRegionAvail().x - browseButtonWidth - browseGap);
            if (std::string targetFolder = (selItem && !selItem->destinationFolder.empty()) ? selItem->destinationFolder : "assets";
                InputTextControl("##TargetFolder", targetFolder, 256, fonts, InputTextOptions{.width = targetInputWidth}) && selItem)
                selItem->destinationFolder = std::move(targetFolder);
            ImGui::SameLine(0.0f, browseGap);

            auto handleBrowseDestination = [&] {
                const auto selectedFolder = OpenFolderSelectionDialog("Select asset destination");
                if (!selectedFolder)
                    return;
                if (const auto relative = ProjectRelativeFolder(modal.ProjectRoot(), *selectedFolder); relative && selItem)
                    selItem->destinationFolder = relative->empty() ? "assets" : *relative;
            };

            if (IconButton({
                    .id = "##BrowseTargetFolder",
                    .glyph = IconButtonGlyph::Folder,
                    .size = {browseButtonWidth, ImGui::GetFrameHeight()},
                    .tooltip = "Browse project folders",
                    .enabled = selItem != nullptr,
                })) {
                handleBrowseDestination();
            }
        }

        void DrawDestinationStrategyColumns(Assets::AssetImportItem *selItem, const Fonts &fonts) {
            int subfolderByType = selItem ? selItem->subfolderByType : 0;
            int assetIdStrategy = selItem ? selItem->assetIdStrategy : 0;

            static constexpr std::array kSubfolderModes{"Meshes / Textures / Audio", "Mirror source structure", "Flat"};
            static constexpr std::array kAssetIdModes{"New GUID", "Stable hash"};

            const SplitColumns columns = CurrentSplitColumns();

            ImGui::BeginGroup();
            FieldLabel("SUBFOLDER BY TYPE", fonts);
            ImGui::SetNextItemWidth(columns.width);
            if (ComboControl("##SubfolderByType", &subfolderByType, kSubfolderModes.data(), static_cast<int>(std::size(kSubfolderModes)),
                             fonts) &&
                selItem) {
                selItem->subfolderByType = subfolderByType;
            }
            ImGui::EndGroup();

            MoveToSecondColumn(columns);
            ImGui::BeginGroup();
            FieldLabel("ASSETID STRATEGY", fonts);
            ImGui::SetNextItemWidth(columns.width);
            if (ComboControl("##AssetIdStrategy", &assetIdStrategy, kAssetIdModes.data(), static_cast<int>(std::size(kAssetIdModes)),
                             fonts) &&
                selItem) {
                selItem->assetIdStrategy = assetIdStrategy;
            }
            ImGui::EndGroup();

            FinishSplitColumns(columns);
        }

        void DrawDestinationTab(const AssetImportModal &modal, Assets::AssetImportSnapshot &snap, const Fonts &fonts) {
            const bool hasSelection = !snap.items.empty() && snap.selectedItemIndex < snap.items.size();
            auto *selItem = hasSelection ? &snap.items[snap.selectedItemIndex] : nullptr;

            bool createMetaSidecar = selItem ? selItem->createMetaSidecar : true;
            bool overwriteWithoutPrompt = selItem ? selItem->overwriteWithoutPrompt : false;

            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2{10.0f, 7.0f});

            FieldLabel("ASSET NAME", fonts);
            if (std::string assetName = selItem ? selItem->displayName : std::string{};
                InputTextControl("##AssetName", assetName, 256, fonts) && selItem) {
                selItem->displayName = std::move(assetName);
            }
            ImGui::Dummy({0.0f, 6.0f});

            FieldLabel("TARGET FOLDER", fonts);
            DrawDestinationPathRow(modal, selItem, fonts);
            ImGui::Dummy({0.0f, 6.0f});

            DrawDestinationStrategyColumns(selItem, fonts);

            ImGui::Dummy({0.0f, 12.0f});
            if (CheckboxControl("Create .meta sidecar for each asset", &createMetaSidecar, fonts) && selItem) {
                selItem->createMetaSidecar = createMetaSidecar;
            }
            ImGui::Dummy({0.0f, 5.0f});
            if (CheckboxControl("Overwrite existing assets without prompt", &overwriteWithoutPrompt, fonts) && selItem) {
                selItem->overwriteWithoutPrompt = overwriteWithoutPrompt;
            }

            ImGui::PopStyleVar();
        }

        void DrawCreatePresetModal(AssetImportModal &modal, const Assets::AssetImportSnapshot &snap, const Fonts &fonts, float actionH,
                                   float gap) {
            static std::string s_presetNameBuffer;
            static bool presetNameError = false;

            if (ImGui::BeginPopupModal("Create Import Preset", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                FieldLabel("PRESET NAME", fonts);
                static_cast<void>(InputTextControl("##NewImportPresetName", s_presetNameBuffer, 256, fonts,
                                                   InputTextOptions{.error = presetNameError, .width = 280.0F}));
                ImGui::Dummy({0.0f, 10.0f});

                if (const ButtonProps dismissPresetProps{
                        .label = "Cancel",
                        .size = {90.0f, actionH},
                        .variant = ButtonVariant::Secondary,
                    };
                    Button(dismissPresetProps)) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine(0.0f, gap);
                if (const ButtonProps createPresetProps{
                        .label = "Create",
                        .size = {90.0f, actionH},
                        .variant = ButtonVariant::Primary,
                        .enabled = !s_presetNameBuffer.empty(),
                    };
                    Button(createPresetProps)) {
                    if (modal.CreatePreset(snap.selectedItemIndex, s_presetNameBuffer)) {
                        presetNameError = false;
                        ImGui::CloseCurrentPopup();
                    } else {
                        presetNameError = true;
                    }
                }
                if (presetNameError)
                    ImGui::TextColored(ImVec4{0.83f, 0.32f, 0.29f, 1.0f}, "Use a unique preset name.");
                ImGui::EndPopup();
            }
        }

        void DrawFooter(AssetImportModal &modal, const Assets::AssetImportSnapshot &snap, const Fonts &fonts, ScopedModalShell &modalShell,
                        ModalFrameResult &frameResult) {
            modalShell.BeginFooter({28.0F, 0.0F}, true);

            constexpr float actionH = 32.0f;
            const float actionY = (ImGui::GetWindowHeight() - actionH) * 0.5f;
            ImGui::SetCursorPosY(actionY);
            PushFont(fonts.sansCompact);
            ImGui::AlignTextToFramePadding();
            std::string statusText = std::format("{} file(s)", snap.items.size());
            std::size_t imported = 0;
            for (const auto &it : snap.items)
                if (it.result.has_value())
                    ++imported;
            if (imported > 0)
                statusText += std::format(" — {} imported", imported);
            ImGui::TextColored(Dim(), "%s", statusText.c_str());
            PopFont(fonts.sansCompact);

            constexpr float cancelW = 100.0f;
            constexpr float importW = 100.0f;
            constexpr float gap = 12.0f;
            constexpr float presetW = 176.0f;
            constexpr float presetAddW = 36.0f;
            constexpr float presetGap = 6.0f;
            constexpr float presetActionsGap = 18.0f;
            const float actionsW = presetW + presetAddW + presetGap + presetActionsGap + cancelW + importW + gap;
            ImGui::SameLine(ImGui::GetWindowWidth() - actionsW - 28.0f);
            ImGui::SetCursorPosY(actionY);

            const bool hasSelected = !snap.items.empty() && snap.selectedItemIndex < snap.items.size();
            const auto *selItem = hasSelected ? &snap.items[snap.selectedItemIndex] : nullptr;
            const bool hasImporter = selItem && !selItem->importerContributionId.empty();
            std::vector<std::string> presetNames =
                hasImporter ? modal.PresetNames(snap.selectedItemIndex) : std::vector<std::string>{"Default"};
            std::vector<const char *> presetLabels;
            presetLabels.reserve(presetNames.size());
            for (const auto &presetName : presetNames)
                presetLabels.push_back(presetName.c_str());

            int presetIndex = 0;
            if (hasSelected) {
                const auto activePreset = modal.ActivePresetName(snap.selectedItemIndex);
                const auto active = std::ranges::find(presetNames, activePreset);
                if (active != presetNames.end())
                    presetIndex = static_cast<int>(std::distance(presetNames.begin(), active));
            }

            ImGui::SetNextItemWidth(presetW);
            if (ComboControl("##ImportPreset", &presetIndex, presetLabels.data(), static_cast<int>(presetLabels.size()), fonts,
                             ComboControlOptions{.height = actionH}) &&
                hasSelected) {
                static_cast<void>(modal.ApplyPreset(snap.selectedItemIndex, presetNames[presetIndex]));
            }

            ImGui::SameLine(0.0f, presetGap);
            if (IconButton({
                    .id = "##CreateImportPreset",
                    .glyph = IconButtonGlyph::Plus,
                    .size = {presetAddW, actionH},
                    .tooltip = "Create preset from current importer settings",
                    .enabled = hasImporter,
                })) {
                ImGui::OpenPopup("Create Import Preset");
            }

            DrawCreatePresetModal(modal, snap, fonts, actionH, gap);

            ImGui::SameLine(0.0f, presetActionsGap);
            const bool importComplete = modal.IsImportComplete();
            ButtonProps cancelProps{
                .label = importComplete ? "Done" : "Cancel",
                .size = {cancelW, actionH},
                .variant = importComplete ? ButtonVariant::Primary : ButtonVariant::Secondary,
                .enabled = true,
            };
            const bool canImport =
                selItem && !selItem->displayName.empty() && !selItem->result.has_value() && !selItem->importerContributionId.empty();
            ButtonProps importProps{
                .label = "Import",
                .size = {importW, actionH},
                .variant = ButtonVariant::Primary,
                .enabled = canImport,
            };

            if (Button(cancelProps))
                frameResult = ModalFrameResult::RequestClose(importComplete ? ModalCloseReason::Completed : ModalCloseReason::Cancelled);
            ImGui::SameLine(0.0f, gap);
            if (Button(importProps)) {
                CancellationToken cancellation;
                static_cast<void>(modal.ImportSingleItem(snap.selectedItemIndex, cancellation));
            }

            modalShell.EndFooter();
        }
    }  // namespace

    ModalFrameResult DrawAssetImportModalPresentation(AssetImportModal &modal, const Fonts &fonts) {
        auto &snap = modal.MutableSnapshot();
        ModalFrameResult frameResult = ModalFrameResult::None();

        ScopedModalShell modalShell(
            {
                .id = "Asset Import",
                .title = "Import New Asset...",
                .requestedSize = {ImportLayout::ModalW, ImportLayout::ModalH},
                .viewportPadding = ImportLayout::ViewportPad,
                .headerHeight = ImportLayout::HeaderH,
                .footerHeight = ImportLayout::FooterH,
                .titleFontSize = Theme::TextPx::Title(),
            },
            fonts);
        if (modalShell.CloseRequested())
            frameResult =
                ModalFrameResult::RequestClose(modal.IsImportComplete() ? ModalCloseReason::Completed : ModalCloseReason::Cancelled);

        std::size_t doneCount = 0;
        std::size_t errorCount = 0;
        std::size_t warningCount = 0;

        for (const auto &item : snap.items) {
            bool hasError = false;
            bool hasWarning = false;
            for (const auto &diagnostic : item.diagnostics) {
                hasError |= diagnostic.severity == Assets::ImportDiagnostic::Severity::Error;
                hasWarning |= diagnostic.severity == Assets::ImportDiagnostic::Severity::Warning;
            }

            if (hasError)
                ++errorCount;
            else if (item.result.has_value())
                ++doneCount;

            if (hasWarning)
                ++warningCount;
        }

        DrawSummary(snap, fonts, doneCount, errorCount, warningCount);

        static auto s_activeTab = static_cast<int>(ImportTab::Queue);
        DrawTabs(s_activeTab, fonts);

        const float footerY = modalShell.FooterStartY();
        const float bodyH = std::max(0.0f, footerY - ImGui::GetCursorPosY());

        DrawSidebar(modal, snap, fonts, bodyH);

        // Content panel
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{22.0f, 22.0f});
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2{8.0f, 8.0f});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Bg1());
        ImGui::BeginChild("Content", ImVec2{0.0f, bodyH}, true);

        using enum ImportTab;
        switch (static_cast<ImportTab>(s_activeTab)) {
            case Queue:
                DrawQueueOverviewTab(modal, snap, fonts);
                break;
            case Diagnostics:
                DrawDiagnosticsTab(snap, fonts);
                break;
            case Settings:
                DrawSettingsTab(modal, snap, fonts);
                break;
            case Destination:
                DrawDestinationTab(modal, snap, fonts);
                break;
            case Count:
            default:
                break;
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(2);

        DrawFooter(modal, snap, fonts, modalShell, frameResult);

        return frameResult;
    }
}  // namespace Horo::Editor
