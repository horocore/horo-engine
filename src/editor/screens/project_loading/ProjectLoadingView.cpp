#include "ProjectLoadingView.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/GuiScreenHost.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/design_system/components/EditorUiLabelWrap.h"

#include <algorithm>
#include <format>
#include <imgui.h>
#include <imgui_internal.h>

namespace Horo::Editor {
    namespace {
        /** @brief Chooses the existing localized action key without evaluating another state transition. */
        const char *PrimaryProjectLoadingKey(const ProjectLoadingViewState &state) {
            if (state.hasFailed)
                return state.canRetry ? "action.retry" : "action.back";
            return "action.cancel";
        }

        /** @brief Applies the loading view's primary action after its button is activated. */
        ProjectLoadingViewCommand PrimaryProjectLoadingCommand(ProjectLoadingViewState &state, const EditorGuiContext &ctx) {
            using enum ProjectLoadingViewCommand;
            if (state.hasFailed)
                return state.canRetry ? Retry : Back;
            state.isCancelled = true;
            state.statusText = ProjectLoadingTextScope.Text(ctx.localization, "status.cancelling");
            return Cancel;
        }

        struct LoadingActions final {
            Ui::WrappedButtonLabel primary;
            Ui::WrappedButtonLabel back;
            bool stacked{};
            float width{};
            float height{};
        };

        /** @brief Measures and arranges the recovery actions at the current viewport width. */
        LoadingActions MeasureLoadingActions(const ProjectLoadingViewState &state, const EditorGuiContext &ctx,
                                             const DesignSystem::ComponentSizeMetrics &metrics, float available, float gap) {
            const auto *key = PrimaryProjectLoadingKey(state);
            Theme::ScopedTextStyle text(ctx.theme.fonts.sansCompact, metrics.fontSize, Theme::FontPx::SansCompact);
            LoadingActions actions;
            const float minimumWidth = Ui::ScaledLayoutValue(128.0F);
            actions.primary =
                Ui::MeasureWrappedButtonLabel(ProjectLoadingTextScope.Text(ctx.localization, key), key, available, metrics, minimumWidth);
            actions.width = actions.primary.width;
            actions.height = actions.primary.height;
            if (!state.hasFailed || !state.canRetry)
                return actions;
            actions.back = Ui::MeasureWrappedButtonLabel(ProjectLoadingTextScope.Text(ctx.localization, "action.back"), "action.back",
                                                         available, metrics, minimumWidth);
            actions.stacked = actions.primary.width + gap + actions.back.width > available;
            if (actions.stacked)
                actions.height += gap + actions.back.height;
            else {
                actions.width += gap + actions.back.width;
                actions.height = std::max(actions.height, actions.back.height);
            }
            return actions;
        }

        /** @brief Draws an action using the shared button while retaining its semantic ID after wrapping. */
        bool DrawLoadingAction(const Ui::WrappedButtonLabel &action, const Theme::Fonts &fonts) {
            const float scale = Theme::GetActiveTokens().sizes.uiScale;
            return Ui::Button({.label = action.label.c_str(),
                               .size = {action.width / scale, action.height / scale},
                               .variant = Ui::ButtonVariant::Secondary,
                               .font = fonts.sansCompact,
                               .baseFontSize = Theme::FontPx::SansCompact,
                               .componentSize = Ui::ComponentSize::Medium});
        }

        /** @brief Emits a command only for the recovery control that actually received activation. */
        ProjectLoadingViewCommand DrawLoadingActions(ProjectLoadingViewState &state, const EditorGuiContext &ctx,
                                                     const LoadingActions &actions, ImVec2 footer, float gap) {
            using enum ProjectLoadingViewCommand;
            ImGui::SetCursorPos({footer.x - actions.width, footer.y - actions.height});
            auto command = DrawLoadingAction(actions.primary, ctx.theme.fonts) ? PrimaryProjectLoadingCommand(state, ctx) : None;
            if (actions.back.label.empty())
                return command;
            if (actions.stacked)
                ImGui::SetCursorPos({footer.x - actions.back.width, footer.y - actions.back.height});
            else
                ImGui::SameLine(0.0F, gap);
            if (DrawLoadingAction(actions.back, ctx.theme.fonts))
                command = Back;
            return command;
        }

        struct LoadingBody final {
            std::string title;
            std::string percentage;
            float percentageWidth{};
            float barHeight{};
            float height{};
        };

        /** @brief Measures the labels and progress bar at the active UI scale. */
        LoadingBody MeasureLoadingBody(const ProjectLoadingViewState &state, const EditorGuiContext &ctx, float bodyWidth, float inset,
                                       float gap) {
            LoadingBody body;
            body.title = ProjectLoadingTextScope.Text(ctx.localization, "title.opening") + " '" + state.projectName + "'";
            body.percentage = std::format("{}%", static_cast<int>(state.progress));
            float percentageHeight{};
            float statusHeight{};
            {
                Theme::ScopedTextStyle text(ctx.theme.fonts.sans, Theme::TextPx::Body(), Theme::FontPx::Sans);
                const auto percentageSize = ImGui::CalcTextSize(body.percentage.c_str());
                body.percentageWidth = percentageSize.x;
                percentageHeight = percentageSize.y;
                statusHeight =
                    ImGui::CalcTextSize(state.statusText.c_str(), nullptr, false, std::max(1.0F, bodyWidth - body.percentageWidth - gap)).y;
            }
            float titleHeight{};
            {
                Theme::ScopedTextStyle text(ctx.theme.fonts.sansEmphasis, Theme::TextPx::Title(), Theme::FontPx::SansEmphasis);
                titleHeight = ImGui::CalcTextSize(body.title.c_str(), nullptr, false, bodyWidth).y;
            }
            body.barHeight = Ui::ScaledLayoutValue(6.0F);
            body.height = titleHeight + inset + std::max(statusHeight, percentageHeight) + gap + body.barHeight;
            return body;
        }

        /** @brief Draws the labels and progress bar within the measured body width. */
        void DrawLoadingBody(const ProjectLoadingViewState &state, const EditorGuiContext &ctx, const LoadingBody &body, ImVec2 bodyPadding,
                             float bodyWidth, float inset, float gap) {
            const auto progressColor = state.hasFailed || state.isCancelled ? Theme::Err() : Theme::Accent();
            ImGui::SetCursorPos(bodyPadding);
            {
                Theme::ScopedTextStyle text(ctx.theme.fonts.sansEmphasis, Theme::TextPx::Title(), Theme::FontPx::SansEmphasis);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + bodyWidth);
                ImGui::TextUnformatted(body.title.c_str());
                ImGui::PopTextWrapPos();
            }
            ImGui::Dummy({0.0F, inset});
            ImGui::SetCursorPosX(bodyPadding.x);
            const ImVec2 statusPosition = ImGui::GetCursorScreenPos();
            Ui::WrappedText(std::max(1.0F, bodyWidth - body.percentageWidth - gap), state.statusText,
                            state.hasFailed ? Theme::Err() : Theme::Muted(), ctx.theme.fonts);
            const float statusBottom = ImGui::GetCursorScreenPos().y;
            ImGui::SetCursorScreenPos({statusPosition.x + bodyWidth - body.percentageWidth, statusPosition.y});
            {
                Theme::ScopedTextStyle text(ctx.theme.fonts.sans, Theme::TextPx::Body(), Theme::FontPx::Sans);
                ImGui::PushStyleColor(ImGuiCol_Text, progressColor);
                ImGui::TextUnformatted(body.percentage.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::SetCursorScreenPos({statusPosition.x, std::max(statusBottom, ImGui::GetCursorScreenPos().y) + gap});
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Theme::Bg2());
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, progressColor);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Theme::GetActiveTokens().radii.control);
            ImGui::ProgressBar(state.progress / 100.0F, {bodyWidth, body.barHeight}, "");
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
        }
    }  // namespace

    /** @copydoc DrawProjectLoadingView */
    ProjectLoadingViewCommand DrawProjectLoadingView(ProjectLoadingViewState &state, const EditorGuiContext &ctx,
                                                     const GuiContentRegion &contentRegion) {
        const auto &tokens = Theme::GetActiveTokens();
        const auto &metrics = DesignSystem::MetricsFor(tokens, Ui::ComponentSize::Medium);
        const float inset = DesignSystem::SpacingFor(tokens, DesignSystem::SpacingSize::Small);
        const float gap = DesignSystem::SpacingFor(tokens, DesignSystem::SpacingSize::Medium);
        const float footerInset = DesignSystem::SpacingFor(tokens, DesignSystem::SpacingSize::Large);
        // Font scaling during measurement must belong to the loading canvas, not ImGui's implicit debug window.
        ImGui::SetNextWindowPos({contentRegion.x, contentRegion.y});
        ImGui::SetNextWindowSize({contentRegion.width, contentRegion.height});
        constexpr auto windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize |
                                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Bg0());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{});
        ImGui::Begin("ProjectLoadingCanvas", nullptr, windowFlags);

        const float width = std::max(1.0F, std::min(Ui::ScaledLayoutValue(480.0F), contentRegion.width - inset * 2.0F));
        const float actionSpace = std::max(1.0F, width - footerInset * 2.0F);
        const auto actions = MeasureLoadingActions(state, ctx, metrics, actionSpace, gap);
        const ImVec2 bodyPadding{tokens.spacing.bodyPaddingX, tokens.spacing.bodyPaddingY};
        const float bodyWidth = std::max(1.0F, width - bodyPadding.x * 2.0F);
        const auto body = MeasureLoadingBody(state, ctx, bodyWidth, inset, gap);
        const float height = bodyPadding.y + body.height + gap + actions.height + footerInset + ImGui::GetStyle().ItemSpacing.y * 3.0F;

        ImGui::SetCursorPos({std::max(0.0F, (contentRegion.width - width) * 0.5F), std::max(0.0F, (contentRegion.height - height) * 0.5F)});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Bg1());
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, tokens.radii.modal);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{inset, inset});
        constexpr auto childFlags =
            ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        ImGui::BeginChild("LoadingModal", {width, height}, true, childFlags);

        DrawLoadingBody(state, ctx, body, bodyPadding, bodyWidth, inset, gap);

        const auto command = DrawLoadingActions(state, ctx, actions, {width - footerInset, height - footerInset}, gap);
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        ImGui::End();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return command;
    }
}  // namespace Horo::Editor
