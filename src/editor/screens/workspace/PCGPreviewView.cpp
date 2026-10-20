#include "editor/screens/workspace/PCGPreviewView.h"

#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"

#include <algorithm>
#include <cmath>

namespace Horo::Editor {
    namespace {
        /** @brief Maps validated provider-space X/Z coordinates into a clipped top-down view. */
        ImVec2 Project(const Math::Vec3 &point, const Math::Aabb &region, ImVec2 origin, ImVec2 size) {
            const float extentX = std::max(0.001F, region.maximum.x - region.minimum.x);
            const float extentZ = std::max(0.001F, region.maximum.z - region.minimum.z);
            return {origin.x + (point.x - region.minimum.x) / extentX * size.x,
                    origin.y + size.y - (point.z - region.minimum.z) / extentZ * size.y};
        }

        /** @brief Draws at most the finite global budget, preserving canonical output/point order. */
        void DrawOverlay(const PCGPreviewOverlay &overlay, const Math::Aabb &region, ImVec2 origin, ImVec2 size,
                         std::size_t maximumPoints) {
            auto *draw = ImGui::GetWindowDrawList();
            const auto pointColor = ImGui::ColorConvertFloat4ToU32(Theme::Text());
            const auto boundsColor = ImGui::ColorConvertFloat4ToU32(Theme::Muted());
            std::size_t remaining = std::min<std::size_t>(maximumPoints, 65'536);
            draw->PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
            for (const auto &output : overlay.candidate.Outputs()) {
                if (remaining == 0)
                    break;
                const auto count = std::min(remaining, output.points->PointCount());
                for (std::size_t index = 0; index < count; ++index) {
                    const auto point = Project(output.points->Transforms()[index].translation, region, origin, size);
                    const auto &bounds = output.points->Bounds()[index];
                    const auto minimum = Project(bounds.minimum, region, origin, size);
                    const auto maximum = Project(bounds.maximum, region, origin, size);
                    // Candidate coordinates are validated; reject projection overflow before handing geometry to ImGui.
                    if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(minimum.x) || !std::isfinite(minimum.y) ||
                        !std::isfinite(maximum.x) || !std::isfinite(maximum.y))
                        continue;
                    draw->AddRect({minimum.x, maximum.y}, {maximum.x, minimum.y}, boundsColor);
                    draw->AddCircleFilled(point, 2.0F * Theme::GetActiveTokens().sizes.uiScale, pointColor, 4);
                }
                remaining -= count;
            }
            draw->PopClipRect();
        }
    }  // namespace

    /** @copydoc DrawPCGPreview */
    PCGPreviewAction DrawPCGPreview(const PCGPreviewSnapshot &preview, const Math::Aabb &region, std::size_t maximumPoints,
                                    const ILocalizationService &localization, const Theme::Fonts &fonts) {
        auto action = PCGPreviewAction::None;
        Ui::ScopedCard card("##pcg_preview", {ImGui::GetContentRegionAvail().x, 220.0F * Theme::GetActiveTokens().sizes.uiScale});
        if (Ui::Button({.label = localization.Get("editor", "workspace.pcg.preview.refresh").c_str(),
                        .enabled = preview.phase != PCGPreviewPhase::Closed,
                        .font = fonts.sans}))
            action = PCGPreviewAction::Refresh;
        if (ImGui::GetContentRegionAvail().x > 240.0F * Theme::GetActiveTokens().sizes.uiScale)
            ImGui::SameLine();
        if (Ui::Button({.label = localization.Get("editor", "workspace.pcg.preview.cancel").c_str(),
                        .variant = Ui::ButtonVariant::Secondary,
                        .enabled = preview.phase == PCGPreviewPhase::Evaluating || preview.overlay != nullptr,
                        .font = fonts.sans}))
            action = PCGPreviewAction::Cancel;
        const auto key = preview.phase == PCGPreviewPhase::Evaluating ? "workspace.pcg.preview.running"
                         : preview.phase == PCGPreviewPhase::Failed   ? "workspace.pcg.preview.failed"
                         : preview.overlay                            ? "workspace.pcg.preview.current"
                                                                      : "workspace.pcg.preview.empty";
        Ui::WrappedText(ImGui::GetContentRegionAvail().x, localization.Get("editor", key), Theme::Muted(), fonts);
        if (preview.error)
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, preview.error->code.Value(), Theme::Muted(), fonts);
        const auto origin = ImGui::GetCursorScreenPos();
        const auto available = ImGui::GetContentRegionAvail();
        const ImVec2 size{std::max(1.0F, available.x), std::max(1.0F, available.y)};
        ImGui::InvisibleButton("##pcg_preview_overlay", size);
        if (preview.overlay)
            DrawOverlay(*preview.overlay, region, origin, size, maximumPoints);
        return action;
    }
}  // namespace Horo::Editor
