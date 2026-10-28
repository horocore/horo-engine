#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserToolbar.h"

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorIcons.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserInteractionSession.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserPane.h"
#include "editor/screens/workspace/panels/global_dock/panes/asset_browser/AssetBrowserPaneLayout.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <ranges>
#include <string>
#include <vector>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] float Scale() noexcept {
            return Theme::GetActiveTokens().sizes.uiScale;
        }

        [[nodiscard]] ImVec4 ControlSurface() noexcept {
            return Theme::BottomDockControlSurface();
        }

        [[nodiscard]] ImFont *ResolveFont(ImFont *preferred) {
            return preferred != nullptr ? preferred : ImGui::GetFont();
        }

        struct ToolbarLayout {
            float scale;
            float gap;
            float control;
            float typeWidth;
            float sortWidth;
            float viewWidth;
            float importWidth;
            bool compact;
            bool narrow;
        };

        [[nodiscard]] ToolbarLayout ResolveToolbarLayout(const float availableWidth) noexcept {
            const float scale = Scale();
            const float safeWidth = std::max(1.0F, availableWidth);
            const bool compact = safeWidth < 640.0F * scale;
            const bool narrow = safeWidth < 900.0F * scale;
            return {.scale = scale,
                    .gap = AssetBrowserLayout::ToolbarGap * scale,
                    .control = AssetBrowserLayout::ToolbarControlHeight * scale,
                    .typeWidth = (narrow ? 94.0F : 106.0F) * scale,
                    .sortWidth = (narrow ? 104.0F : 154.0F) * scale,
                    .viewWidth = 66.0F * scale,
                    .importWidth = (compact ? 34.0F : 84.0F) * scale,
                    .compact = compact,
                    .narrow = narrow};
        }

        [[nodiscard]] ImVec4 ToolbarIconColor(const bool enabled, const bool hovered) noexcept {
            ImVec4 color = Theme::Muted();
            if (enabled && hovered)
                color = Theme::Text();
            if (!enabled)
                color.w *= 0.35F;
            return color;
        }

        [[nodiscard]] AssetBrowserViewMode ViewModeForIndex(const int index) noexcept {
            return index == 0 ? AssetBrowserViewMode::Grid : AssetBrowserViewMode::List;
        }

        [[nodiscard]] Ui::UiIcon ViewIconForIndex(const int index) noexcept {
            return index == 0 ? Ui::UiIcon::GridView : Ui::UiIcon::ViewList;
        }

        [[nodiscard]] const char *ViewLabelKey(const int index) noexcept {
            return index == 0 ? "view.grid" : "view.list";
        }

        [[nodiscard]] bool DrawToolbarButton(const ImVec2 position, const char *id, const Ui::UiIcon icon, const bool enabled,
                                             const char *tooltip, ImFont *iconFont) {
            const float scale = Scale();
            const float size = AssetBrowserLayout::ToolbarControlHeight * scale;
            ImGui::SetCursorScreenPos(position);
            ImGui::PushID(id);
            ImGui::BeginDisabled(!enabled);
            const bool clicked = ImGui::InvisibleButton("##button", {size, size});
            const bool hovered = enabled && ImGui::IsItemHovered();
            const bool active = enabled && ImGui::IsItemActive();
            ImGui::EndDisabled();

            ImDrawList *drawList = ImGui::GetWindowDrawList();
            const ImVec2 maximum{position.x + size, position.y + size};
            drawList->AddRectFilled(position, maximum, Theme::U32(hovered || active ? Theme::Hover() : ControlSurface()), 4.0F * scale);
            drawList->AddRect(position, maximum, Theme::U32(hovered ? Theme::BorderStrong() : Theme::Border()), 4.0F * scale);
            const ImVec4 iconColor = ToolbarIconColor(enabled, hovered);
            const float iconSize = 16.0F * scale;
            Ui::DrawEditorIcon(drawList, icon, {position.x + (size - iconSize) * 0.5F, position.y + (size - iconSize) * 0.5F},
                               {iconSize, iconSize}, Theme::U32(iconColor), iconFont);
            if (hovered && tooltip != nullptr && tooltip[0] != '\0')
                Ui::ShowTooltip(tooltip);
            ImGui::PopID();
            return enabled && clicked;
        }

        [[nodiscard]] float DrawBreadcrumb(const ImVec2 position, const ContentBrowserDirectory &directory,
                                           EditorWorkspaceViewCommandData &command, ImFont *font, const float maximumWidth) {
            if (directory.breadcrumbs.empty() || maximumWidth <= 0.0F)
                return 0.0F;

            const float scale = Scale();
            const float fontSize = GlobalDockLabelFontSize();
            const float slashWidth = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0F, "/").x;
            const float gap = 6.0F * scale;
            const float minimumWidth = 70.0F * scale;
            float x = position.x;
            const float right = position.x + maximumWidth;
            for (std::size_t index = 0; index < directory.breadcrumbs.size(); ++index) {
                const ContentBrowserBreadcrumb &segment = directory.breadcrumbs[index];
                const float labelWidth = font->CalcTextSizeA(fontSize, FLT_MAX, 0.0F, segment.label.c_str()).x;
                if (x + labelWidth > right)
                    break;
                ImGui::SetCursorScreenPos({x, position.y});
                ImGui::PushID(segment.absolutePath.c_str());
                if (ImGui::InvisibleButton("##crumb", {labelWidth, AssetBrowserLayout::ToolbarControlHeight * scale}))
                    command = AssetBrowserInteractionSession::Navigate(segment.absolutePath);
                const bool hovered = ImGui::IsItemHovered();
                ImGui::GetWindowDrawList()->AddText(font, fontSize,
                                                    {x, position.y + (AssetBrowserLayout::ToolbarControlHeight * scale - fontSize) * 0.5F},
                                                    Theme::U32(hovered ? Theme::Text() : Theme::Muted()), segment.label.c_str());
                ImGui::PopID();
                x += labelWidth;
                if (index + 1 < directory.breadcrumbs.size() && x + gap + slashWidth <= right) {
                    x += gap;
                    ImGui::GetWindowDrawList()->AddText(font, fontSize,
                                                        {x,
                                                         position.y + (AssetBrowserLayout::ToolbarControlHeight * scale - fontSize) * 0.5F},
                                                        Theme::U32(Theme::Dim()), "/");
                    x += slashWidth + gap;
                }
            }
            return std::max(minimumWidth, x - position.x);
        }

        void DrawSearch(const ImVec2 position, const float width, AssetBrowserInteractionState &state, const EditorGuiContext &context,
                        const bool showShortcut) {
            const float scale = Scale();
            const float height = AssetBrowserLayout::ToolbarControlHeight * scale;
            const float fontSize = GlobalDockLabelFontSize();
            if ((ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper) && ImGui::IsKeyPressed(ImGuiKey_K))
                ImGui::SetKeyboardFocusHere();
            ImGui::SetCursorScreenPos(position);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, {32.0F * scale, (height - fontSize) * 0.5F});
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0F * scale);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0F);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ControlSurface());
            ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Theme::Hover());
            ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ControlSurface());
            ImGui::PushStyleColor(ImGuiCol_Border, Theme::Border());
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::Text());
            ImGui::PushStyleColor(ImGuiCol_TextDisabled, Theme::Dim());
            ImGui::PushItemWidth(width);
            {
                Theme::ScopedTextStyle textStyle{context.theme.fonts.sans, fontSize, Theme::FontPx::Sans};
                static_cast<void>(ImGui::InputTextWithHint("##ContentBrowserSearch",
                                                           AssetBrowserPane::kTextScope.Text(context.localization, "search").c_str(),
                                                           state.search.data(), state.search.size()));
            }
            ImGui::PopItemWidth();
            ImGui::PopStyleColor(6);
            ImGui::PopStyleVar(3);

            ImDrawList *drawList = ImGui::GetWindowDrawList();
            const float iconSize = 16.0F * scale;
            Ui::DrawEditorIcon(drawList, Ui::UiIcon::Search, {position.x + 7.0F * scale, position.y + (height - iconSize) * 0.5F},
                               {iconSize, iconSize}, Theme::U32(Theme::Dim()), context.theme.fonts.icon);
            if (showShortcut) {
                ImFont *font = ResolveFont(context.theme.fonts.sansCompact);
                constexpr const char *shortcut = "⌘K";
                const float shortcutFontSize = AssetBrowserLayout::SecondaryFontSize();
                const ImVec2 textSize = font->CalcTextSizeA(shortcutFontSize, FLT_MAX, 0.0F, shortcut);
                const ImVec2 badgeMin{position.x + width - textSize.x - 17.0F * scale, position.y + 6.0F * scale};
                const ImVec2 badgeMax{position.x + width - 7.0F * scale, position.y + height - 6.0F * scale};
                drawList->AddRect(badgeMin, badgeMax, Theme::U32(Theme::Border()), 4.0F * scale);
                drawList->AddText(font, shortcutFontSize, {badgeMin.x + 5.0F * scale, badgeMin.y + 1.0F * scale}, Theme::U32(Theme::Dim()),
                                  shortcut);
            }
        }

        void DrawViewOption(const ImVec2 position, const int index, AssetBrowserInteractionState &state, const EditorGuiContext &context) {
            const float scale = Scale();
            ImGui::SetCursorScreenPos(position);
            ImGui::PushID(index);
            if (ImGui::InvisibleButton("##view", {32.0F * scale, 28.0F * scale}))
                state.viewMode = ViewModeForIndex(index);
            const bool active = state.viewMode == ViewModeForIndex(index);
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList *drawList = ImGui::GetWindowDrawList();
            if (active || hovered)
                drawList->AddRectFilled(position, {position.x + 32.0F * scale, position.y + 28.0F * scale},
                                        Theme::U32(active ? Theme::Hover() : Theme::AccentSoft()));
            const float iconSize = 16.0F * scale;
            Ui::DrawEditorIcon(drawList, ViewIconForIndex(index),
                               {position.x + (32.0F * scale - iconSize) * 0.5F, position.y + (28.0F * scale - iconSize) * 0.5F},
                               {iconSize, iconSize}, Theme::U32(active || hovered ? Theme::Text() : Theme::Dim()),
                               context.theme.fonts.icon);
            if (hovered)
                Ui::ShowTooltip(AssetBrowserPane::kTextScope.Text(context.localization, ViewLabelKey(index)).c_str(), &context.theme.fonts);
            ImGui::PopID();
        }

        void DrawViewSwitch(const ImVec2 position, AssetBrowserInteractionState &state, const EditorGuiContext &context) {
            const float scale = Scale();
            const ImVec2 size{66.0F * scale, AssetBrowserLayout::ToolbarControlHeight * scale};
            ImDrawList *drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(position, {position.x + size.x, position.y + size.y}, Theme::U32(ControlSurface()), 4.0F * scale);
            DrawViewOption({position.x + scale, position.y + scale}, 0, state, context);
            DrawViewOption({position.x + 33.0F * scale, position.y + scale}, 1, state, context);
            drawList->AddLine({position.x + 33.0F * scale, position.y}, {position.x + 33.0F * scale, position.y + size.y},
                              Theme::U32(Theme::Border()));
            drawList->AddRect(position, {position.x + size.x, position.y + size.y}, Theme::U32(Theme::Border()), 4.0F * scale);
        }

        [[nodiscard]] bool DrawImportButton(const ImVec2 position, const bool compact, const EditorGuiContext &context) {
            const float scale = Scale();
            const float width = (compact ? 34.0F : 84.0F) * scale;
            const float height = AssetBrowserLayout::ToolbarControlHeight * scale;
            ImGui::SetCursorScreenPos(position);
            const bool clicked = ImGui::InvisibleButton("##ContentBrowserImport", {width, height});
            const bool hovered = ImGui::IsItemHovered();
            ImDrawList *drawList = ImGui::GetWindowDrawList();
            const ImU32 top = Theme::U32(hovered ? Theme::AccentHover() : Theme::Accent());
            const ImU32 bottom = Theme::U32(hovered ? Theme::Accent() : Theme::AccentActive());
            drawList->AddRectFilledMultiColor(position, {position.x + width, position.y + height}, top, top, bottom, bottom);
            drawList->AddRect(position, {position.x + width, position.y + height}, Theme::U32(Theme::AccentActive()), 4.0F * scale);
            const float iconSize = 16.0F * scale;
            const float iconCenterX = position.x + (compact ? width * 0.5F : 15.0F * scale);
            Ui::DrawEditorIcon(drawList, Ui::UiIcon::Create, {iconCenterX - iconSize * 0.5F, position.y + (height - iconSize) * 0.5F},
                               {iconSize, iconSize}, Theme::U32(Theme::DarkText()), context.theme.fonts.icon);
            if (!compact) {
                ImFont *font = ResolveFont(context.theme.fonts.sansCompact);
                const std::string &label = AssetBrowserPane::kTextScope.Text(context.localization, "action.import");
                const float fontSize = GlobalDockLabelFontSize();
                drawList->AddText(font, fontSize, {position.x + 29.0F * scale, position.y + (height - fontSize) * 0.5F},
                                  Theme::U32(Theme::DarkText()), label.c_str());
            }
            return clicked;
        }

        [[nodiscard]] float DrawNavigation(const ImVec2 position, const EditorWorkspaceViewModel &viewModel,
                                           EditorWorkspaceViewCommandData &command, const EditorGuiContext &context,
                                           const ToolbarLayout &layout) {
            float x = position.x;
            const auto button = [&](const char *id, const Ui::UiIcon icon, const bool enabled, const char *key,
                                    const EditorWorkspaceViewCommand action) {
                if (const std::string &tooltip = AssetBrowserPane::kTextScope.Text(context.localization, key);
                    DrawToolbarButton({x, position.y}, id, icon, enabled, tooltip.c_str(), context.theme.fonts.icon))
                    command = AssetBrowserInteractionSession::Navigate(action);
                x += layout.control + layout.gap;
            };
            button("ContentBrowserBack", Ui::UiIcon::ArrowBack, viewModel.contentBrowserCanNavigateBack, "navigation.back",
                   EditorWorkspaceViewCommand::NavigateContentBrowserBack);
            button("ContentBrowserForward", Ui::UiIcon::ArrowForward, viewModel.contentBrowserCanNavigateForward, "navigation.forward",
                   EditorWorkspaceViewCommand::NavigateContentBrowserForward);
            button("ContentBrowserUp", Ui::UiIcon::ArrowUpward,
                   viewModel.contentBrowser.absoluteCurrentPath != viewModel.contentBrowser.absoluteRootPath, "navigation.up",
                   EditorWorkspaceViewCommand::NavigateContentBrowserUp);
            if (layout.narrow)
                return x;
            ImGui::GetWindowDrawList()->AddLine({x, position.y + 5.0F * layout.scale}, {x, position.y + 25.0F * layout.scale},
                                                Theme::U32(Theme::Border()));
            x += layout.scale + layout.gap;
            return x +
                   DrawBreadcrumb({x, position.y}, viewModel.contentBrowser, command, ResolveFont(context.theme.fonts.sansCompact),
                                  220.0F * layout.scale) +
                   layout.gap;
        }

        [[nodiscard]] std::vector<std::string> BuildTypeLabels(const EditorWorkspaceViewModel &viewModel, const EditorGuiContext &context) {
            std::vector<std::string> labels{AssetBrowserPane::kTextScope.Text(context.localization, "filter.all_types")};
            for (const ContentBrowserEntry &entry : viewModel.contentBrowser.entries) {
                if (entry.kind == ContentBrowserEntryKind::Asset && !entry.assetType.empty() &&
                    std::ranges::find(labels, entry.assetType) == labels.end())
                    labels.push_back(entry.assetType);
            }
            std::ranges::sort(labels.begin() + 1, labels.end());
            return labels;
        }

        void DrawTypeFilter(const ImVec2 position, const float width, const EditorWorkspaceViewModel &viewModel,
                            AssetBrowserInteractionState &state, const EditorGuiContext &context) {
            std::vector<std::string> labels = BuildTypeLabels(viewModel, context);
            int selectedIndex = 0;
            if (!state.assetTypeFilter.empty()) {
                const auto selected = std::ranges::find(labels, state.assetTypeFilter);
                if (selected == labels.end())
                    state.assetTypeFilter.clear();
                else
                    selectedIndex = static_cast<int>(std::distance(labels.begin(), selected));
            }
            std::vector<const char *> items;
            items.reserve(labels.size());
            for (const std::string &label : labels)
                items.push_back(label.c_str());
            ImGui::SetCursorScreenPos(position);
            ImGui::SetNextItemWidth(width);
            if (Ui::ComboControl("ContentBrowserTypeFilter", &selectedIndex, items.data(), static_cast<int>(items.size()),
                                 context.theme.fonts,
                                 {.height = AssetBrowserLayout::ToolbarControlHeight,
                                  .componentSize = Ui::ComponentSize::Medium,
                                  .surface = Ui::ComboControlSurface::BottomDockToolbar}))
                state.assetTypeFilter = selectedIndex == 0 ? std::string{} : labels[static_cast<std::size_t>(selectedIndex)];
        }

        void DrawSortField(const ImVec2 position, const float width, AssetBrowserInteractionState &state, const EditorGuiContext &context) {
            using enum ContentBrowserSortField;
            const std::array labels{AssetBrowserPane::kTextScope.Text(context.localization, "sort.name"),
                                    AssetBrowserPane::kTextScope.Text(context.localization, "sort.type")};
            const std::array items{labels[0].c_str(), labels[1].c_str()};
            int selectedIndex = state.sortField == Name ? 0 : 1;
            ImGui::SetCursorScreenPos(position);
            ImGui::SetNextItemWidth(width);
            if (Ui::ComboControl("ContentBrowserSort", &selectedIndex, items.data(), static_cast<int>(items.size()), context.theme.fonts,
                                 {.height = AssetBrowserLayout::ToolbarControlHeight,
                                  .componentSize = Ui::ComponentSize::Medium,
                                  .surface = Ui::ComboControlSurface::BottomDockToolbar}))
                state.sortField = selectedIndex == 0 ? Name : Type;
        }

        void DrawTrailingActions(const ImVec2 position, const EditorWorkspaceViewModel &viewModel, EditorWorkspaceViewCommandData &command,
                                 AssetBrowserInteractionState &state, const EditorGuiContext &context, const ToolbarLayout &layout) {
            float x = position.x;
            if (!layout.compact) {
                DrawTypeFilter({x, position.y}, layout.typeWidth, viewModel, state, context);
                x += layout.typeWidth + layout.gap;
                DrawSortField({x, position.y}, layout.sortWidth, state, context);
                x += layout.sortWidth + layout.gap;
                DrawViewSwitch({x, position.y}, state, context);
                x += layout.viewWidth + layout.gap;
            }
            if (DrawImportButton({x, position.y}, layout.compact, context))
                command = AssetBrowserInteractionSession::ImportHere(viewModel.contentBrowser.absoluteCurrentPath);
            x += layout.importWidth + layout.gap;
            if (const std::string &tooltip = AssetBrowserPane::kTextScope.Text(context.localization, "action.create_folder");
                !DrawToolbarButton({x, position.y}, "ContentBrowserNewFolder", Ui::UiIcon::CreateNewFolder, true, tooltip.c_str(),
                                   context.theme.fonts.icon))
                return;
            state.createFolderBuffer.fill('\0');
            state.openCreateFolder = true;
        }
    }  // namespace

    /** @copydoc DrawAssetBrowserToolbar */
    void DrawAssetBrowserToolbar(const ImVec2 &position, const float availableWidth, const EditorWorkspaceViewModel &viewModel,
                                 EditorWorkspaceViewCommandData &command, AssetBrowserInteractionState &state,
                                 const EditorGuiContext &context) {
        const ToolbarLayout layout = ResolveToolbarLayout(availableWidth);
        const float safeWidth = std::max(1.0F, availableWidth);
        const float x = DrawNavigation(position, viewModel, command, context, layout);
        const float fixedWidth = layout.compact ? layout.importWidth + layout.gap + layout.control
                                                : layout.typeWidth + layout.gap + layout.sortWidth + layout.gap + layout.viewWidth +
                                                      layout.gap + layout.importWidth + layout.gap + layout.control;
        const float rightEdge = position.x + safeWidth;
        const float fixedX = rightEdge - fixedWidth;
        DrawSearch({x, position.y}, std::max(60.0F * layout.scale, fixedX - x - layout.gap), state, context, !layout.narrow);
        DrawTrailingActions({fixedX, position.y}, viewModel, command, state, context, layout);
    }
}  // namespace Horo::Editor
