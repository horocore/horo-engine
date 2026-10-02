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

    /** @copydoc GamePanel::DrawPanel */
    void GamePanel::DrawPanel([[maybe_unused]] const ImVec2 &position, const ImVec2 &size, const EditorWorkspaceViewModel &viewModel,
                              EditorWorkspaceViewCommandData &, const EditorGuiContext &context) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0F, 0.0F));
        ImGui::BeginChild("##GameContent", ImVec2(size.x, size.y), false, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = size.x;
        const float height = size.y;
        const ImVec2 framebufferScale = ImGui::GetIO().DisplayFramebufferScale;
        if (viewportRenderer_ != nullptr) {
            viewportRenderer_->RequestExtent(
                width > 0.0F && height > 0.0F
                    ? EditorViewportExtent{static_cast<std::uint32_t>(std::max(1.0F, width * framebufferScale.x)),
                                           static_cast<std::uint32_t>(std::max(1.0F, height * framebufferScale.y))}
                    : EditorViewportExtent{});
            viewportRenderer_->RequestGrid({.visible = false});
            viewportRenderer_->RequestLightVisualizer({});
        }
        if (const EditorViewportTextureView texture =
                viewportRenderer_ != nullptr ? viewportRenderer_->TextureView() : EditorViewportTextureView{};
            viewportRenderer_ != nullptr && viewportRenderer_->IsReady() && texture.IsValid() &&
            (viewModel.playState == EditorPlayState::Playing || viewModel.playState == EditorPlayState::Paused)) {
            ImGui::GetWindowDrawList()->AddImage(static_cast<ImTextureID>(texture.textureId), origin,
                                                 ImVec2(origin.x + width, origin.y + height), ImVec2(texture.u0, texture.v0),
                                                 ImVec2(texture.u1, texture.v1));
        } else {
            const std::string &message =
                !viewModel.playError.empty() ? viewModel.playError : context.localization.Get("editor", "workspace.game.idle");
            const ImVec2 textSize = ImGui::CalcTextSize(message.c_str());
            ImGui::SetCursorScreenPos(
                ImVec2(origin.x + std::max(0.0F, (width - textSize.x) * 0.5F), origin.y + std::max(0.0F, (height - textSize.y) * 0.5F)));
            ImGui::TextUnformatted(message.c_str());
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        static_cast<void>(position);
    }
}  // namespace Horo::Editor
