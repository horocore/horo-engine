#include "ProjectLoadingView.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/GuiScreenHost.h"
#include "Horo/Editor/Localization/ILocalizationService.h"

#include <algorithm>
#include <format>
#include <imgui.h>

namespace Horo::Editor {
    /** @copydoc DrawProjectLoadingView */
    ProjectLoadingViewCommand DrawProjectLoadingView(ProjectLoadingViewState &state, const EditorGuiContext &ctx,
                                                     const GuiContentRegion &contentRegion) {
        using enum ProjectLoadingViewCommand;
        const auto &tokens = Theme::GetActiveTokens();
        const auto &metrics = DesignSystem::MetricsFor(tokens, Ui::ComponentSize::Medium);
        const float scale = tokens.sizes.uiScale;
        const float inset = DesignSystem::SpacingFor(tokens, DesignSystem::SpacingSize::Small);
        const float gap = DesignSystem::SpacingFor(tokens, DesignSystem::SpacingSize::Medium);
        const float footerInset = DesignSystem::SpacingFor(tokens, DesignSystem::SpacingSize::Large);
        const bool twoActions = state.hasFailed && state.canRetry;
        // Font scaling during measurement must belong to the loading canvas, not ImGui's implicit debug window.
        ImGui::SetNextWindowPos({contentRegion.x, contentRegion.y});
        ImGui::SetNextWindowSize({contentRegion.width, contentRegion.height});
        constexpr auto windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Bg0());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{});
        ImGui::Begin("ProjectLoadingCanvas", nullptr, windowFlags);

        const auto primaryLabel =
            ProjectLoadingTextScope.Text(ctx.localization,
                                         state.hasFailed ? (state.canRetry ? "action.retry" : "action.back") : "action.cancel");
        const auto backLabel = ProjectLoadingTextScope.Text(ctx.localization, "action.back");
        const auto measureButton = [&](const std::string &label) {
            Theme::ScopedTextStyle text(ctx.theme.fonts.sansCompact, metrics.fontSize, Theme::FontPx::SansCompact);
            return std::max(Ui::ScaledLayoutValue(128.0F), ImGui::CalcTextSize(label.c_str()).x + metrics.paddingX * 2.0F);
        };
        const float requestedPrimary = measureButton(primaryLabel);
        const float requestedBack = twoActions ? measureButton(backLabel) : 0.0F;
        const float width = std::max(1.0F, std::min(Ui::ScaledLayoutValue(480.0F), contentRegion.width - inset * 2.0F));
        const float actionSpace = std::max(1.0F, width - footerInset * 2.0F);
        const float primaryWidth = std::min(requestedPrimary, actionSpace);
        const float backWidth = std::min(requestedBack, actionSpace);
        const bool stacked = twoActions && requestedPrimary + gap + requestedBack > actionSpace;
        const float actionsHeight = metrics.minimumHeight * (stacked ? 2.0F : 1.0F) + (stacked ? gap : 0.0F);
        const ImVec2 bodyPadding{tokens.spacing.bodyPaddingX, tokens.spacing.bodyPaddingY};
        const float bodyWidth = std::max(1.0F, width - bodyPadding.x * 2.0F);
        const std::string title = ProjectLoadingTextScope.Text(ctx.localization, "title.opening") + " '" + state.projectName + "'";
        const std::string percentage = std::format("{}%", static_cast<int>(state.progress));
        float percentageWidth{};
        float percentageHeight{};
        float statusHeight{};
        {
            Theme::ScopedTextStyle text(ctx.theme.fonts.sans, Theme::TextPx::Body(), Theme::FontPx::Sans);
            const auto percentageSize = ImGui::CalcTextSize(percentage.c_str());
            percentageWidth = percentageSize.x;
            percentageHeight = percentageSize.y;
            statusHeight =
                ImGui::CalcTextSize(state.statusText.c_str(), nullptr, false, std::max(1.0F, bodyWidth - percentageWidth - gap)).y;
        }
        float titleHeight{};
        {
            Theme::ScopedTextStyle text(ctx.theme.fonts.sansEmphasis, Theme::TextPx::Title(), Theme::FontPx::SansEmphasis);
            titleHeight = ImGui::CalcTextSize(title.c_str(), nullptr, false, bodyWidth).y;
        }
        const float barHeight = Ui::ScaledLayoutValue(6.0F);
        const float height = bodyPadding.y + titleHeight + inset + std::max(statusHeight, percentageHeight) + gap + barHeight + gap +
                             actionsHeight + footerInset + ImGui::GetStyle().ItemSpacing.y * 3.0F;
        const ImVec4 progressColor = state.hasFailed || state.isCancelled ? Theme::Err() : Theme::Accent();
        ProjectLoadingViewCommand command = None;

        ImGui::SetCursorPos({std::max(0.0F, (contentRegion.width - width) * 0.5F), std::max(0.0F, (contentRegion.height - height) * 0.5F)});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Bg1());
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, tokens.radii.modal);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{inset, inset});
        constexpr auto childFlags =
            ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        ImGui::BeginChild("LoadingModal", {width, height}, true, childFlags);

        ImGui::SetCursorPos(bodyPadding);
        {
            Theme::ScopedTextStyle text(ctx.theme.fonts.sansEmphasis, Theme::TextPx::Title(), Theme::FontPx::SansEmphasis);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + bodyWidth);
            ImGui::TextUnformatted(title.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::Dummy({0.0F, inset});
        ImGui::SetCursorPosX(bodyPadding.x);
        const ImVec2 statusPosition = ImGui::GetCursorScreenPos();
        Ui::WrappedText(std::max(1.0F, bodyWidth - percentageWidth - gap), state.statusText,
                        state.hasFailed ? Theme::Err() : Theme::Muted(), ctx.theme.fonts);
        const float statusBottom = ImGui::GetCursorScreenPos().y;
        ImGui::SetCursorScreenPos({statusPosition.x + bodyWidth - percentageWidth, statusPosition.y});
        {
            Theme::ScopedTextStyle text(ctx.theme.fonts.sans, Theme::TextPx::Body(), Theme::FontPx::Sans);
            ImGui::PushStyleColor(ImGuiCol_Text, progressColor);
            ImGui::TextUnformatted(percentage.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::SetCursorScreenPos({statusPosition.x, std::max(statusBottom, ImGui::GetCursorScreenPos().y) + gap});
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Theme::Bg2());
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, progressColor);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, tokens.radii.control);
        ImGui::ProgressBar(state.progress / 100.0F, {bodyWidth, barHeight}, "");
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);

        // Button props use logical sizes; layout uses the resolved, scaled rectangles.
        const float rowWidth = primaryWidth + (twoActions && !stacked ? gap + backWidth : 0.0F);
        ImGui::SetCursorPos({width - footerInset - rowWidth, height - footerInset - actionsHeight});
        if (Ui::Button({.label = primaryLabel.c_str(),
                        .size = {primaryWidth / scale, 0.0F},
                        .variant = Ui::ButtonVariant::Secondary,
                        .font = ctx.theme.fonts.sansCompact,
                        .baseFontSize = Theme::FontPx::SansCompact,
                        .componentSize = Ui::ComponentSize::Medium})) {
            if (state.hasFailed)
                command = state.canRetry ? Retry : Back;
            else {
                state.isCancelled = true;
                state.statusText = ProjectLoadingTextScope.Text(ctx.localization, "status.cancelling");
                command = Cancel;
            }
        }
        if (twoActions) {
            if (stacked)
                ImGui::SetCursorPos({width - footerInset - backWidth, height - footerInset - metrics.minimumHeight});
            else
                ImGui::SameLine(0.0F, gap);
            if (Ui::Button({.label = backLabel.c_str(),
                            .size = {backWidth / scale, 0.0F},
                            .variant = Ui::ButtonVariant::Secondary,
                            .font = ctx.theme.fonts.sansCompact,
                            .baseFontSize = Theme::FontPx::SansCompact,
                            .componentSize = Ui::ComponentSize::Medium}))
                command = Back;
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return command;
    }
}  // namespace Horo::Editor
