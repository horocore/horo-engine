#include "Horo/Editor/EditorUiComponents.h"

#include <algorithm>
#include <format>
#include <string>

namespace Horo::Editor::Ui {
    namespace {
        /** @brief Draws one bounded lane; source metadata does not contain key times, so none are invented. */
        bool DrawLane(const std::uint64_t duration, SequenceTimelineState &state, const float height) {
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const float width = std::max(1.0F, ImGui::GetContentRegionAvail().x);
            ImGui::InvisibleButton("##lane", {width, height});
            auto *draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(origin, {origin.x + width, origin.y + height}, Theme::U32(Theme::Bg2()),
                                Theme::GetActiveTokens().radii.control);
            constexpr int divisions = 4;
            for (int tick = 0; tick <= divisions; ++tick) {
                const float x = origin.x + width * static_cast<float>(tick) / divisions;
                draw->AddLine({x, origin.y}, {x, origin.y + height}, Theme::U32(Theme::Border()));
            }
            if (const auto span = state.VisibleFrames(duration);
                state.playhead >= state.firstFrame && state.playhead - state.firstFrame < span) {
                const double fraction =
                    span <= 1 ? 0.0 : static_cast<double>(state.playhead - state.firstFrame) / static_cast<double>(span - 1);
                const float x = origin.x + static_cast<float>(fraction) * width;
                draw->AddLine({x, origin.y}, {x, origin.y + height}, Theme::U32(Theme::Accent()), Theme::GetActiveTokens().sizes.uiScale);
            }
            if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                state.playhead = state.FrameAt(duration, (ImGui::GetIO().MousePos.x - origin.x) / width);
                return true;
            }
            return false;
        }

        /** @brief Draws only delivered track types with vertically scrollable, width-constrained labels. */
        bool DrawTracks(const Cinematic::SequenceAssetData &data, SequenceTimelineState &state, const SequenceTimelineLabels &labels) {
            bool changed = false;
            if (const float height = DesignSystem::MetricsFor(Theme::GetActiveTokens(), ComponentSize::Medium).minimumHeight;
                ImGui::BeginTable("##tracks", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV,
                                  {0.0F, std::max(height, ImGui::GetContentRegionAvail().y)})) {
                ImGui::TableSetupColumn("##track", ImGuiTableColumnFlags_WidthStretch, 1.0F);
                ImGui::TableSetupColumn("##timeline", ImGuiTableColumnFlags_WidthStretch, 2.0F);
                for (const auto &track : data.tracks) {
                    if (!IsTimelineTrackAvailable(track.type))
                        continue;
                    const std::string identity = std::format("{}:{}", track.id.stableValue, track.id.generation);
                    ImGui::PushID(identity.c_str());
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextWrapped("%s · %s: %u", labels.trackTypes[static_cast<std::size_t>(track.type)].c_str(), labels.keys.c_str(),
                                       track.keyframeCount);
                    ImGui::TableSetColumnIndex(1);
                    changed = DrawLane(data.durationFrames, state, height) || changed;
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            return changed;
        }
    }  // namespace

    /** @copydoc SequenceTimeline */
    bool SequenceTimeline(const Cinematic::SequenceAsset &asset, SequenceTimelineState &state, const SequenceTimelineLabels &labels,
                          const Theme::Fonts &fonts) {
        const auto &data = asset.Data();
        state.Clamp(data.durationFrames);
        Theme::ScopedTextStyle typography(fonts.sans, Theme::TextPx::Label(), Theme::FontPx::Sans);
        const std::string range =
            std::format("{} – {} / {}", state.firstFrame, state.FrameAt(data.durationFrames, 1.0), data.durationFrames);
        ImGui::TextWrapped("%s", range.c_str());
        ImGui::TextWrapped("%s", data.name.c_str());
        ImGui::TextWrapped("%s", labels.noContext.c_str());
        ImGui::TextWrapped("%s", labels.zoom.c_str());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        const double minimumZoom = 1.0;
        const double maximumZoom = 1024.0;
        bool changed = ImGui::SliderScalar("##zoom", ImGuiDataType_Double, &state.zoom, &minimumZoom, &maximumZoom, "%.1fx");
        ImGui::TextWrapped("%s", labels.scroll.c_str());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        changed = ImGui::InputScalar("##scroll", ImGuiDataType_U64, &state.firstFrame) || changed;
        ImGui::TextWrapped("%s", labels.playhead.c_str());
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
        changed = ImGui::InputScalar("##playhead", ImGuiDataType_U64, &state.playhead) || changed;
        state.Clamp(data.durationFrames);
        if (const bool unavailable = std::ranges::any_of(data.tracks,
                                                         [](const auto &track) {
            return !IsTimelineTrackAvailable(track.type);
        });
            unavailable)
            ImGui::TextWrapped("%s", labels.unavailable.c_str());
        if (data.tracks.empty())
            ImGui::TextWrapped("%s", labels.empty.c_str());
        return DrawTracks(data, state, labels) || changed;
    }
}  // namespace Horo::Editor::Ui
