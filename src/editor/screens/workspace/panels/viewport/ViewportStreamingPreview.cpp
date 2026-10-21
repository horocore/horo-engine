#include "ViewportStreamingPreview.h"

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/renderer/EditorGuiRenderer.h"
#include "editor/renderer/EditorViewportScene.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <algorithm>
#include <cstdio>

namespace Horo::Editor {
    namespace {
        namespace WS = WorldStreaming;

        /** @brief Resolves a closed residency vocabulary without constructing strings in draw paths. */
        const char *DesiredKey(const WS::StreamingDesiredResidency state) {
            switch (state) {
                case WS::StreamingDesiredResidency::Unloaded:
                    return "workspace.streaming.unloaded";
                case WS::StreamingDesiredResidency::Loaded:
                    return "workspace.streaming.loaded";
                case WS::StreamingDesiredResidency::Activated:
                    return "workspace.streaming.activated";
            }
            return "workspace.streaming.unknown";
        }

        /** @brief Resolves canonical actual cell state independently of requested residency. */
        const char *RuntimeKey(const WS::StreamingCellState state) {
            switch (state) {
                case WS::StreamingCellState::Unloaded:
                    return "workspace.streaming.unloaded";
                case WS::StreamingCellState::Loading:
                    return "workspace.streaming.loading";
                case WS::StreamingCellState::Resident:
                    return "workspace.streaming.resident";
                case WS::StreamingCellState::Active:
                    return "workspace.streaming.active";
                case WS::StreamingCellState::Evicting:
                    return "workspace.streaming.evicting";
                case WS::StreamingCellState::Failed:
                    return "workspace.streaming.failed";
            }
            return "workspace.streaming.unknown";
        }

        /** @brief Resolves typed authority causes; absence is never reported as success. */
        const char *OutcomeKey(const WS::StreamingCellOperationOutcome reason) {
            switch (reason) {
                case WS::StreamingCellOperationOutcome::None:
                    return "workspace.streaming.unknown";
                case WS::StreamingCellOperationOutcome::Succeeded:
                    return "workspace.streaming.succeeded";
                case WS::StreamingCellOperationOutcome::Cancelled:
                    return "workspace.streaming.cancelled";
                case WS::StreamingCellOperationOutcome::Failed:
                    return "workspace.streaming.failed";
                case WS::StreamingCellOperationOutcome::Replaced:
                    return "workspace.streaming.replaced";
                case WS::StreamingCellOperationOutcome::Shutdown:
                    return "workspace.streaming.shutdown";
            }
            return "workspace.streaming.unknown";
        }

        /** @brief Preserves admission decisions and lifecycle transitions instead of treating None as a failure. */
        const char *DecisionKey(const WS::StreamingDiagnosticDecisionEvent &event) {
            if (event.admission) {
                switch (*event.admission) {
                    case WS::StreamingDiagnosticAdmissionDecision::Accepted:
                        return "workspace.streaming.accepted";
                    case WS::StreamingDiagnosticAdmissionDecision::Deferred:
                        return "workspace.streaming.deferred";
                    case WS::StreamingDiagnosticAdmissionDecision::Rejected:
                        return "workspace.streaming.rejected";
                }
            }
            if (event.outcome != WS::StreamingCellOperationOutcome::None)
                return OutcomeKey(event.outcome);
            return event.transition ? RuntimeKey(event.transition->current) : "workspace.streaming.unknown";
        }

        /** @brief Resolves actual planner retention reasons, including source-free linger and cooldown. */
        const char *StabilityKey(const WS::StreamingCellStabilityPhase phase) {
            switch (phase) {
                case WS::StreamingCellStabilityPhase::Unloaded:
                    return "workspace.streaming.unloaded";
                case WS::StreamingCellStabilityPhase::Resident:
                    return "workspace.streaming.retained";
                case WS::StreamingCellStabilityPhase::Lingering:
                    return "workspace.streaming.lingering";
                case WS::StreamingCellStabilityPhase::Cooldown:
                    return "workspace.streaming.cooldown";
                case WS::StreamingCellStabilityPhase::Watching:
                    return "workspace.streaming.watching";
                default:
                    return "workspace.streaming.unknown";
            }
        }

        /** @brief Shows source provenance and stability evidence separately from actual runtime decisions. */
        void SourceDetails(const WorldStreamingPreviewCell &cell, const EditorGuiContext &context) {
            const auto label = [&](const char *key) {
                return context.localization.Get("editor", key).c_str();
            };
            if (cell.desired)
                Ui::DrawPropRow(label("workspace.streaming.stability"), label(StabilityKey(cell.desired->stability.phase)),
                                context.theme.fonts);
            if (cell.desired && cell.desired->pinnedFloor)
                Ui::DrawPropRow(label("workspace.streaming.pinned"), label(DesiredKey(*cell.desired->pinnedFloor)), context.theme.fonts);
            for (std::size_t index = 0; index < std::min<std::size_t>(cell.contributions.size(), 4); ++index) {
                const auto &source = cell.contributions[index];
                char value[160];
                std::snprintf(value, sizeof(value), "%llu / %s", static_cast<unsigned long long>(source.source.id.Value()),
                              label(DesiredKey(source.residency)));
                Ui::DrawPropRow(label(source.predicted ? "workspace.streaming.predicted" : "workspace.streaming.source"), value,
                                context.theme.fonts);
            }
        }

        /** @brief Maps actual residency to theme tokens without changing either ImGui's theme or native renderer state. */
        ImU32 RuntimeColor(const WorldStreamingPreviewCell &cell) {
            if (cell.attempts.empty())
                return Theme::U32(Theme::Muted());
            switch (cell.attempts.back().state) {
                case WS::StreamingCellState::Active:
                    return Theme::U32(Theme::Ok());
                case WS::StreamingCellState::Resident:
                    return Theme::U32(Theme::Accent());
                case WS::StreamingCellState::Failed:
                    return Theme::U32(Theme::Err());
                case WS::StreamingCellState::Loading:
                case WS::StreamingCellState::Evicting:
                    return Theme::U32(Theme::Warn());
                default:
                    return Theme::U32(Theme::Muted());
            }
        }

        /** @brief Shows bounded source/attempt/reason evidence; counts disclose channels larger than the visible prefix. */
        void Details(const WorldStreamingPreviewCell &cell, const EditorGuiContext &context) {
            Ui::ScopedTooltip tooltip{&context.theme.fonts};
            const auto &localization = context.localization;
            const auto label = [&](const char *key) {
                return localization.Get("editor", key).c_str();
            };
            char identity[128];
            std::snprintf(identity, sizeof(identity), "%d, %d, %d / %u / %u", cell.cell.cell.x, cell.cell.cell.y, cell.cell.cell.z,
                          static_cast<unsigned>(cell.cell.cell.lod), static_cast<unsigned>(cell.cell.cell.layer.Value()));
            Ui::DrawPropRow(label("workspace.streaming.cell"), identity, context.theme.fonts);
            Ui::DrawPropRow(label("workspace.streaming.desired"),
                            label(cell.desired ? DesiredKey(cell.desired->residency) : "workspace.streaming.no_demand"),
                            context.theme.fonts);
            Ui::DrawPropRow(label("workspace.streaming.runtime"),
                            label(cell.attempts.empty() ? "workspace.streaming.unknown" : RuntimeKey(cell.attempts.back().state)),
                            context.theme.fonts);
            for (std::size_t index = 0; index < std::min<std::size_t>(cell.failures.size(), 4); ++index)
                Ui::DrawPropRow(label("workspace.streaming.reason"), label(OutcomeKey(cell.failures[index].reason)), context.theme.fonts);
            for (std::size_t index = 0; index < std::min<std::size_t>(cell.decisions.size(), 4); ++index)
                Ui::DrawPropRow(label("workspace.streaming.reason"), label(DecisionKey(cell.decisions[index])), context.theme.fonts);
            SourceDetails(cell, context);
            std::snprintf(identity, sizeof(identity), "%zu / %zu / %zu / %zu", cell.contributions.size(), cell.attempts.size(),
                          cell.failures.size(), cell.decisions.size());
            Ui::DrawPropRow(label("workspace.streaming.evidence_counts"), identity, context.theme.fonts);
        }

        /** @brief Projects fixed box corners through the existing backend-neutral viewport camera. */
        std::array<std::optional<Math::Vec2>, 8> Pixels(const WorldStreamingPreviewBox &box, const EditorWorkspaceViewModel &view,
                                                        const ImVec2 origin, const ImVec2 extent, const Math::ClipDepthRange depthRange) {
            std::array<std::optional<Math::Vec2>, 8> pixels;
            for (std::size_t index = 0; index < pixels.size(); ++index) {
                const auto point = ProjectEditorViewportPoint(view.viewportCamera, box.corners[index], extent.x / extent.y, depthRange);
                if (point.HasError() || !point.Value())
                    continue;
                const auto pixel = MapEditorViewportPointToPixels(*point.Value(), {origin.x, origin.y}, {extent.x, extent.y});
                if (pixel.HasValue())
                    pixels[index] = pixel.Value();
            }
            return pixels;
        }

        /** @brief Draws each projected cube edge once, respecting near-plane rejection. */
        void DrawEdges(ImDrawList &drawList, const std::array<std::optional<Math::Vec2>, 8> &pixels, const ImU32 desiredColor) {
            for (std::size_t start = 0; start < 8; ++start) {
                for (const std::size_t axis : {1U, 2U, 4U}) {
                    const auto end = start ^ axis;
                    if (start >= end || !pixels[start] || !pixels[end])
                        continue;
                    drawList.AddLine({pixels[start]->x, pixels[start]->y}, {pixels[end]->x, pixels[end]->y}, desiredColor);
                }
            }
        }

        /** @brief Validates immutable geometry against the currently active canonical viewport binding. */
        bool Current(const EditorWorkspaceViewModel &view) {
            const auto &geometry = view.worldStreamingPreview;
            return geometry && geometry->preview && geometry->preview->Source() == view.worldViewport.snapshot &&
                   geometry->viewRevision == view.worldViewport.viewRevision && geometry->globalOrigin == view.worldViewport.globalOrigin;
        }

        /** @brief Resolves typed refresh failures into localized viewport status. */
        const char *StatusKey(const EditorWorkspaceViewModel &view, const bool current) {
            if (current)
                return "workspace.streaming.legend";
            if (!view.worldStreamingPreviewError)
                return "workspace.streaming.unavailable";
            const auto &code = view.worldStreamingPreviewError->code.Value();
            if (code == WorldDocumentErrors::Capacity.code.Value())
                return "workspace.streaming.capacity";
            if (code == WorldDocumentErrors::Stale.code.Value())
                return "workspace.streaming.stale";
            if (code == WorldDocumentErrors::Cancelled.code.Value())
                return "workspace.streaming.cancelled";
            return "workspace.streaming.unavailable";
        }

        /** @brief Wraps the status and discloses the exact visible/out-of-window cell counts using existing typography. */
        void Status(ImDrawList &drawList, const ImVec2 origin, const ImVec2 extent, const EditorWorkspaceViewModel &view,
                    const EditorGuiContext &context, const bool current) {
            const auto &message = context.localization.Get("editor", StatusKey(view, current));
            const auto padding = ImGui::GetStyle().WindowPadding;
            const float wrapWidth = std::max(1.0F, extent.x - padding.x * 2);
            const ImVec2 position{origin.x + padding.x, origin.y + padding.y + ImGui::GetFrameHeightWithSpacing() * 2};
            drawList.AddText(ImGui::GetFont(), ImGui::GetFontSize(), position, Theme::U32(current ? Theme::Muted() : Theme::Warn()),
                             message.c_str(), nullptr, wrapWidth);
            if (!current)
                return;
            char counts[192];
            const auto &geometry = *view.worldStreamingPreview;
            std::snprintf(counts, sizeof(counts), "%s: %zu / %zu",
                          context.localization.Get("editor", "workspace.streaming.visible_counts").c_str(), geometry.boxes.size(),
                          geometry.outsideLocalWindow);
            const float height = ImGui::CalcTextSize(message.c_str(), nullptr, false, wrapWidth).y;
            drawList.AddText(ImGui::GetFont(), ImGui::GetFontSize(), {position.x, position.y + height + ImGui::GetStyle().ItemSpacing.y},
                             Theme::U32(Theme::Muted()), counts, nullptr, wrapWidth);
        }

        /** @brief Draws desired-state box edges and a distinct runtime-state marker; no runtime reads or geometry allocations occur. */
        void DrawBox(ImDrawList &drawList, const WorldStreamingPreviewBox &box, const EditorWorkspaceViewModel &view,
                     const EditorGuiContext &context, const ImVec2 origin, const ImVec2 extent, const Math::ClipDepthRange depthRange) {
            const auto &cell = view.worldStreamingPreview->preview->Cells()[box.cellIndex];
            const auto pixels = Pixels(box, view, origin, extent, depthRange);
            const auto desiredColor = Theme::U32(
                cell.desired && cell.desired->residency != WS::StreamingDesiredResidency::Unloaded ? Theme::Accent() : Theme::Dim());
            DrawEdges(drawList, pixels, desiredColor);
            if (!pixels[0] || !pixels[7])
                return;
            const ImVec2 center{(pixels[0]->x + pixels[7]->x) * 0.5F, (pixels[0]->y + pixels[7]->y) * 0.5F};
            const float radius = ImGui::GetTextLineHeight() * 0.3F;
            drawList.AddCircleFilled(center, radius, RuntimeColor(cell));
            if (ImGui::IsMouseHoveringRect({center.x - radius, center.y - radius}, {center.x + radius, center.y + radius}))
                Details(cell, context);
        }
    }  // namespace

    /** @copydoc DrawViewportStreamingPreview */
    void DrawViewportStreamingPreview(ImDrawList &drawList, const ImVec2 origin, const ImVec2 extent, const EditorWorkspaceViewModel &view,
                                      const EditorGuiContext &context, const Math::ClipDepthRange depthRange) {
        if (!view.worldViewport.snapshot || extent.x <= 0.0F || extent.y <= 0.0F)
            return;
        const auto &geometry = view.worldStreamingPreview;
        const bool current = Current(view);
        drawList.PushClipRect(origin, {origin.x + extent.x, origin.y + extent.y}, true);
        if (current)
            for (const auto &box : geometry->boxes)
                DrawBox(drawList, box, view, context, origin, extent, depthRange);
        Status(drawList, origin, extent, view, context, current);
        drawList.PopClipRect();
    }
}  // namespace Horo::Editor
