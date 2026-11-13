#include "RuntimeConsoleRecordList.h"

#include "Horo/Editor/EditorUiComponents.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>

namespace Horo::Editor::Ui {
    namespace {
        /** @brief Keeps fixed row geometry independent of multiline user/handler text. */
        std::string_view FirstLine(std::string_view value) {
            return value.substr(0, value.find_first_of("\r\n"));
        }

        /** @brief Current-frame typography/geometry, resolved once from the existing design tokens. */
        struct RowStyle final {
            ImFont *font{};
            float textSize{}, height{}, paddingX{}, paddingY{};
        };

        /** @brief Ephemeral display fields borrowing a real source row; created only for visible rows. */
        struct RecordPaint final {
            std::string_view primary, secondary, label;
            ImVec4 color;
            std::uint32_t repetitions{1};
            std::optional<float> progress;
        };

        /** @brief One current virtual-list frame; it owns no row data and performs no source query. */
        struct ListFrame final {
            const char *id{};
            std::size_t count{};
            std::uint64_t revision{};
            std::string_view empty;
            std::optional<std::size_t> selected;
            float height{};
            bool enabled{}, followTail{};
        };

        /** @brief Clips complete borrowed text to measured available bounds, preserving body typography. */
        void PaintText(const RowStyle &style, const ImVec2 position, const ImVec4 clip, const ImVec4 color, std::string_view text) {
            text = FirstLine(text);
            if (!text.empty() && clip.z > clip.x)
                ImGui::GetWindowDrawList()->AddText(style.font, style.textSize, position, ImGui::GetColorU32(color), text.data(),
                                                    text.data() + text.size(), 0, &clip);
        }

        /** @brief Paints one native keyboard-selectable row with clipped borrowed text and the current disabled alpha. */
        bool DrawRecordRow(const ListFrame &frame, const std::size_t ordinal, const RecordPaint &record, const RowStyle &style) {
            ImGui::PushID(static_cast<int>(ordinal));
            const bool activated = ImGui::Selectable("##record", frame.selected == ordinal, ImGuiSelectableFlags_None, {0, style.height});
            const ImVec2 minimum = ImGui::GetItemRectMin();
            const ImVec2 maximum = ImGui::GetItemRectMax();
            const ImVec4 clip{minimum.x + style.paddingX, minimum.y, maximum.x - style.paddingX, maximum.y};
            ImVec4 primaryClip = clip;
            if (record.repetitions > 1) {
                std::array<char, 16> buffer{'x'};
                const auto number = std::to_chars(buffer.data() + 1, buffer.data() + buffer.size(), record.repetitions);
                const std::string_view badge(buffer.data(), static_cast<std::size_t>(number.ptr - buffer.data()));
                const float width =
                    style.font->CalcTextSizeA(style.textSize, std::numeric_limits<float>::max(), 0, badge.data(), number.ptr).x;
                PaintText(style, {clip.z - width, minimum.y + style.paddingY}, clip, Theme::Muted(), badge);
                primaryClip.z = clip.z - width - style.paddingX;
            }
            float x = clip.x;
            if (!record.label.empty()) {
                PaintText(style, {x, minimum.y + style.paddingY}, primaryClip, record.color, record.label);
                x += style.font
                         ->CalcTextSizeA(style.textSize, std::numeric_limits<float>::max(), 0, record.label.data(),
                                         record.label.data() + record.label.size())
                         .x +
                     style.paddingX;
            }
            primaryClip.x = x;
            PaintText(style, {x, minimum.y + style.paddingY}, primaryClip, Theme::Text(), record.primary);
            PaintText(style, {clip.x, minimum.y + style.paddingY + style.textSize + style.paddingX}, clip, record.color, record.secondary);
            if (record.progress && clip.z > clip.x) {
                const float height = std::max(1.0F, style.paddingY * 0.3F);
                const ImVec2 start{clip.x, maximum.y - height};
                auto *draw = ImGui::GetWindowDrawList();
                draw->AddRectFilled(start, {clip.z, maximum.y}, ImGui::GetColorU32(Theme::Bg3()));
                draw->AddRectFilled(start, {clip.x + (clip.z - clip.x) * *record.progress, maximum.y}, ImGui::GetColorU32(Theme::Accent()));
            }
            ImGui::PopID();
            return activated;
        }

        /** @brief Shares identical bounded geometry, selection identity and scrolling across the three typed source adapters. */
        template <typename RowAt>
        RuntimeConsoleRecordListResult DrawVirtualList(const ListFrame &props, const Theme::Fonts &fonts, const RowAt &rowAt) {
            RuntimeConsoleRecordListResult result;
            if (!std::isfinite(props.height) || props.height <= 0 || props.count > 10000)
                return result;
            const auto &metrics = DesignSystem::MetricsFor(Theme::GetActiveTokens(), ComponentSize::Medium);
            const float textSize = Theme::TextPx::Body();
            const float rowHeight = 2 * textSize + 2 * metrics.paddingY + metrics.paddingX;
            ImFont *font = fonts.monospace ? fonts.monospace : fonts.sans;
            if (!font)
                font = ImGui::GetFont();
            const RowStyle row{font, textSize, rowHeight, metrics.paddingX, metrics.paddingY};
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Bg1());
            ImGui::PushStyleColor(ImGuiCol_Header, Theme::AccentSoft());
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Theme::Hover());
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, Theme::AccentSoft());
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, {0, 0});
            ImGui::BeginDisabled(!props.enabled);
            if (ImGui::BeginChild(props.id, {0, std::min(props.height, rowHeight * 128)}, ImGuiChildFlags_Borders)) {
                const bool pinned = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1;
                std::array<char, 16> revision{};
                const auto text = std::to_chars(revision.data(), revision.data() + revision.size(), props.revision, 16);
                ImGui::PushID(revision.data(), text.ptr);
                if (props.count == 0)
                    WrappedText(ImGui::GetContentRegionAvail().x, props.empty, Theme::Muted(), fonts);
                ImGuiListClipper clipper;
                clipper.Begin(static_cast<int>(props.count), rowHeight);
                while (clipper.Step()) {
                    for (int index = clipper.DisplayStart; index < clipper.DisplayEnd && result.painted < 128; ++index) {
                        const auto ordinal = static_cast<std::size_t>(index);
                        if (DrawRecordRow(props, ordinal, rowAt(ordinal), row))
                            result.activated = ordinal;
                        ++result.painted;
                    }
                }
                if (props.followTail && pinned)
                    ImGui::SetScrollY(ImGui::GetCursorPosY());
                ImGui::PopID();
            }
            ImGui::EndChild();
            ImGui::EndDisabled();
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(4);
            return result;
        }
    }  // namespace

    /** @copydoc RuntimeConsoleRecordList */
    RuntimeConsoleRecordListResult RuntimeConsoleRecordList(const RuntimeConsoleRecordListProps &props, const Theme::Fonts &fonts) {
        return DrawVirtualList({props.id, props.rows.size(), props.revision, props.empty, props.selected, props.height, props.enabled,
                                props.followTail},
                               fonts, [&](std::size_t ordinal) {
            const auto &entry = props.rows[ordinal];
            std::string_view output = entry.output;
            if (entry.operationUnavailable)
                output = props.unavailable;
            else if (entry.HasPendingResult())
                output = props.pending;
            return RecordPaint{entry.command, output, {}, entry.state == OperationState::Failed ? Theme::Err() : Theme::Muted(), 1, {}};
        });
    }

    /** @copydoc RuntimeInspectionRecordList */
    RuntimeConsoleRecordListResult RuntimeInspectionRecordList(const RuntimeInspectionRecordListProps &props, const Theme::Fonts &fonts) {
        return DrawVirtualList({"##inspection.records", std::min(props.rows.size(), std::size_t{256}), props.revision, props.empty,
                                props.selected, props.height, true, false},
                               fonts, [&](std::size_t ordinal) {
            const auto &row = props.rows[ordinal];
            const std::string_view title{row.title.data(), std::min<std::size_t>(row.titleBytes, row.title.size())};
            const std::string_view detail{row.detail.data(), std::min<std::size_t>(row.detailBytes, row.detail.size())};
            return RecordPaint{title, detail, detail.empty() ? std::string_view{} : props.detailLabel, Theme::Muted(), 1, {}};
        });
    }

    /** @copydoc RuntimeStoreRecordList */
    RuntimeConsoleRecordListResult RuntimeStoreRecordList(const RuntimeStoreRecordListProps &props, const Theme::Fonts &fonts) {
        if (props.labels.size() != 7 || (props.kind != RuntimeStoreRecordKind::Logs && props.kind != RuntimeStoreRecordKind::Jobs))
            return {};
        const bool logs = props.kind == RuntimeStoreRecordKind::Logs;
        const auto count = logs ? props.source.LogRowCount() : props.source.JobRowCount();
        const auto revision = logs ? props.source.LogProjectionRevision() : props.source.JobProjectionRevision();
        return DrawVirtualList({props.id, count, revision, props.empty, props.selected, props.height, props.enabled, props.followTail},
                               fonts, [&](std::size_t ordinal) {
            if (logs) {
                const auto rows = props.source.Logs(ordinal, 1);
                const auto &row = rows.front();
                const auto severity = row.source->level;
                ImVec4 color = Theme::Muted();
                if (severity >= Log::Level::Error)
                    color = Theme::Err();
                else if (severity == Log::Level::Warn)
                    color = Theme::Warn();
                return RecordPaint{row.category, row.message, props.labels[static_cast<std::size_t>(severity)], color, row.repetitions, {}};
            }
            const auto rows = props.source.Jobs(ordinal, 1);
            const auto &row = rows.front();
            const auto state = row.source->state;
            const auto label = props.labels[static_cast<std::size_t>(state)];
            std::string_view text = row.message;
            if (!row.phase.empty())
                text = row.phase;
            if (state == OperationState::Failed && !row.errorCode.empty())
                text = row.errorCode;
            if (text.empty())
                text = label;
            const ImVec4 color = state == OperationState::Failed ? Theme::Err() : Theme::Muted();
            return RecordPaint{row.title, text, label, color, 1, row.source->progress};
        });
    }
}  // namespace Horo::Editor::Ui
