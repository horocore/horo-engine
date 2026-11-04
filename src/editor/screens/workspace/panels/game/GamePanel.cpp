#include "editor/screens/workspace/panels/game/GamePanel.h"

#include "Horo/Editor/Localization/ILocalizationService.h"

#include <algorithm>
#include <cstdint>

namespace Horo::Editor {
    /** @copydoc GamePanel::OnAttach */
    void GamePanel::OnAttach(PanelContext &context) {
        viewportRenderer_ = context.viewportRenderer;
    }

    /** @copydoc GamePanel::OnDetach */
    void GamePanel::OnDetach() {
        viewportRenderer_ = nullptr;
    }

    /** @copydoc GamePanel::DrawIcon */
    void GamePanel::DrawIcon(ImDrawList *drawList, const ImVec2 &position, const ImVec2 &size, const ImU32 color) {
        const float originX = position.x + (size.x - 14.0F) * 0.5F;
        const float originY = position.y + (size.y - 14.0F) * 0.5F;
        drawList->AddRect(ImVec2(originX + 1.0F, originY + 2.0F), ImVec2(originX + 13.0F, originY + 12.0F), color, 2.0F, 0, 1.5F);
        drawList->AddTriangleFilled(ImVec2(originX + 5.0F, originY + 4.5F), ImVec2(originX + 10.0F, originY + 7.0F),
                                    ImVec2(originX + 5.0F, originY + 9.5F), color);
    }

    void GamePanel::RequestViewport(const float width, const float height) {
        if (!viewportRenderer_)
            return;
        const auto scale = ImGui::GetIO().DisplayFramebufferScale;
        viewportRenderer_->RequestExtent(width > 0 && height > 0
                                             ? EditorViewportExtent{static_cast<std::uint32_t>(std::max(1.0F, width * scale.x)),
                                                                    static_cast<std::uint32_t>(std::max(1.0F, height * scale.y))}
                                             : EditorViewportExtent{});
        viewportRenderer_->RequestGrid({.visible = false});
        viewportRenderer_->RequestLightVisualizer({});
    }

    bool GamePanel::CanDraw(const EditorWorkspaceViewModel &viewModel, const EditorViewportTextureView &texture) const {
        if (viewModel.playViewSource == EditorPlayViewSource::Simulator && !viewModel.simulatorPreviewAvailable)
            return false;
        const bool playing = viewModel.playState == EditorPlayState::Playing || viewModel.playState == EditorPlayState::Paused;
        return viewportRenderer_ && texture.IsValid() && playing &&
               (viewModel.playViewSource == EditorPlayViewSource::SelectedXr || viewportRenderer_->IsReady());
    }

    void GamePanel::DrawUnavailable(const ImVec2 &origin, const float width, const float height, const EditorWorkspaceViewModel &viewModel,
                                    const EditorGuiContext &context) {
        const auto fallback = viewModel.playViewSource == EditorPlayViewSource::Simulator ? "simulator_unavailable" : "idle";
        const std::string message = !viewModel.playError.empty() ? viewModel.playError : kTextScope.Text(context.localization, fallback);
        const ImVec2 extent = ImGui::CalcTextSize(message.c_str());
        ImGui::SetCursorScreenPos(
            ImVec2(origin.x + std::max(0.0F, (width - extent.x) * 0.5F), origin.y + std::max(0.0F, (height - extent.y) * 0.5F)));
        ImGui::TextUnformatted(message.c_str());
    }

    /** @copydoc GamePanel::DrawPanel */
    void GamePanel::DrawPanel(const ImVec2 &, const ImVec2 &size, const EditorWorkspaceViewModel &viewModel,
                              EditorWorkspaceViewCommandData &, const EditorGuiContext &context) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::BeginChild("##GameContent", size, false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
        if (viewModel.playViewSource != EditorPlayViewSource::AuthoredCamera) {
            const auto key = viewModel.playViewSource == EditorPlayViewSource::Simulator ? "simulator_preview" : "xr_mirror";
            ImGui::TextWrapped("%s", kTextScope.Text(context.localization, key).c_str());
        }
        const auto origin = ImGui::GetCursorScreenPos();
        const float height = std::max(0.0F, ImGui::GetContentRegionAvail().y);
        RequestViewport(size.x, height);
        const auto texture = viewportRenderer_ ? viewportRenderer_->TextureView() : EditorViewportTextureView{};
        if (CanDraw(viewModel, texture)) {
            ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(texture.textureId), origin,
                                                 ImVec2(origin.x + size.x, origin.y + height), ImVec2(texture.u0, texture.v0),
                                                 ImVec2(texture.u1, texture.v1));
        } else
            DrawUnavailable(origin, size.x, height, viewModel, context);
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }
}  // namespace Horo::Editor
