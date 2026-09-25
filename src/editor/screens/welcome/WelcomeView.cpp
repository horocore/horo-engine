#include "editor/screens/welcome/WelcomeView.h"

#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/GuiScreenHost.h"

#include <Horo/Editor/Localization/ILocalizationService.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <numeric>
#include <string_view>

namespace Horo::Editor {
    namespace {
        [[nodiscard]] const char *CompatibilityLabelKey(const RecentProjectCompatibilityProjection &projection) {
            if (projection.inspectionState == RecentProjectInspectionState::Refreshing)
                return "welcome.project.compatibility.refreshing";
            using enum Application::ProjectCompatibilityStatus;
            switch (projection.status) {
                case Current:
                    return "welcome.project.compatibility.current";
                case CompatibleReleaseLine:
                    return "welcome.project.compatibility.compatible";
                case AutomaticMigrationRequired:
                    return "welcome.project.compatibility.will_upgrade";
                case RecoveryRequired:
                    return "welcome.project.compatibility.recovery_required";
                case FutureVersion:
                    return "welcome.project.compatibility.newer_horo_required";
                case MigrationPathMissing:
                case RequiredProviderUnavailable:
                    return "welcome.project.compatibility.cannot_upgrade";
                case Corrupt:
                case Inaccessible:
                    return "welcome.project.compatibility.version_unavailable";
            }
            return "welcome.project.compatibility.version_unavailable";
        }

        [[nodiscard]] bool MatchesSearch(const RecentProjectEntry &project, const std::string_view query) {
            const auto contains = [query](const std::string_view value) {
                return std::search(value.begin(), value.end(), query.begin(), query.end(), [](const char left, const char right) {
                    return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
                }) != value.end();
            };
            return query.empty() || contains(project.name) || contains(project.rootPath);
        }

        /** @brief Draws the clipped project identity and compatibility text inside a card. */
        void DrawProjectCardDetails(const RecentProjectEntry &project, const EditorGuiContext &ctx, const ImVec2 contentMin,
                                    const float contentWidth, const float contentHeight) {
            using namespace Theme;
            auto *drawList = ImGui::GetWindowDrawList();
            drawList->AddRectFilled(contentMin, {contentMin.x + contentHeight, contentMin.y + contentHeight}, U32(Bg3()), Layout::Radius);

            std::string versionText;
            std::string statusText = project.lastOpenedLabel;
            if (project.compatibility.has_value()) {
                versionText = project.compatibility->projectVersion.has_value()
                                  ? "Horo " + Application::FormatHoroVersion(project.compatibility->projectVersion->value)
                                  : ctx.localization.Get("editor", "welcome.project.compatibility.version_unavailable");
                statusText = ctx.localization.Get("editor", CompatibilityLabelKey(*project.compatibility));
            }

            ImFont *const nameFont = ctx.theme.fonts.sans ? ctx.theme.fonts.sans : ImGui::GetFont();
            ImFont *const compactFont = ctx.theme.fonts.sansCompact ? ctx.theme.fonts.sansCompact : ImGui::GetFont();
            const float metaWidth = std::max(compactFont->CalcTextSizeA(TextPx::Caption(), FLT_MAX, 0.0F, versionText.c_str()).x,
                                             compactFont->CalcTextSizeA(TextPx::Caption(), FLT_MAX, 0.0F, statusText.c_str()).x);
            const float contentMaxX = contentMin.x + contentWidth;
            const float visibleMetaWidth = std::min(metaWidth, contentWidth * 0.38F);
            const float metaMinX = contentMaxX - visibleMetaWidth;
            constexpr float textGap = 12.0F;
            const float detailsMinX = contentMin.x + contentHeight + textGap;
            const float detailsMaxX = std::max(detailsMinX, metaMinX - textGap);
            const ImVec4 detailsClip{detailsMinX, contentMin.y, detailsMaxX, contentMin.y + contentHeight};
            const ImVec4 metaClip{metaMinX, contentMin.y, contentMaxX, contentMin.y + contentHeight};

            drawList->AddText(nameFont, TextPx::Label(), {detailsMinX, contentMin.y}, U32(Text()), project.name.c_str(), nullptr, 0.0F,
                              &detailsClip);
            drawList->AddText(compactFont, TextPx::Caption(), {detailsMinX, contentMin.y + 21.0F}, U32(Muted()), project.rootPath.c_str(),
                              nullptr, 0.0F, &detailsClip);

            const float metaY = contentMin.y + (versionText.empty() ? 11.0F : 0.0F);
            if (!versionText.empty()) {
                drawList->AddText(compactFont, TextPx::Caption(), {metaMinX, metaY}, U32(Muted()), versionText.c_str(), nullptr, 0.0F,
                                  &metaClip);
            }
            drawList->AddText(compactFont, TextPx::Caption(), {metaMinX, metaY + (versionText.empty() ? 0.0F : 21.0F)}, U32(Muted()),
                              statusText.c_str(), nullptr, 0.0F, &metaClip);
        }

        /// @brief Draws one recent-project card and its context menu.
        [[nodiscard]] WelcomeViewCommand DrawProjectCard(const RecentProjectEntry &project, const int index, const EditorGuiContext &ctx) {
            using namespace Theme;

            WelcomeViewCommand action = WelcomeViewCommand::None;
            ImGui::PushID(index);
            {
                Ui::ScopedCard card("ProjectCard", {-1.0F, 64.0F}, 14.0F, 12.0F);

                const ImVec2 contentMin = ImGui::GetCursorScreenPos();
                constexpr float contentHeight = 40.0F;
                const float contentWidth = ImGui::GetContentRegionAvail().x;
                if (ImGui::InvisibleButton("Project card###welcome_project_card", {contentWidth, contentHeight}))
                    action = WelcomeViewCommand::OpenRecentProject;
                if (Ui::BeginContextMenu("##welcome_project_context")) {
                    const auto menuItem = [&](const char *key, const Ui::ContextMenuItemTone tone = Ui::ContextMenuItemTone::Normal,
                                              const bool enabled = true) {
                        return Ui::ContextMenuItem(ctx.localization.Get("editor", key).c_str(), nullptr, ctx.theme.fonts, tone, {},
                                                   enabled);
                    };
                    if (menuItem("welcome.project.remove"))
                        action = WelcomeViewCommand::RemoveRecentProject;
                    if (menuItem("welcome.project.delete", Ui::ContextMenuItemTone::Danger))
                        action = WelcomeViewCommand::DeleteRecentProject;
                    Ui::ContextMenuSeparator();
                    static_cast<void>(menuItem("welcome.project.settings", Ui::ContextMenuItemTone::Normal, false));
                    Ui::EndContextMenu();
                }

                DrawProjectCardDetails(project, ctx, contentMin, contentWidth, contentHeight);
            }
            ImGui::PopID();
            return action;
        }

        [[nodiscard]] float NewsCardHeight(const char *tag, const char *title, const char *description, const float width,
                                           const EditorGuiContext &ctx) {
            const float padding = Ui::ScaledLayoutValue(14.0F);
            const ImGuiStyle &style = ImGui::GetStyle();
            const float contentWidth = std::max(1.0F, width - (padding + style.ChildBorderSize) * 2.0F);
            const float itemSpacing = style.ItemSpacing.y;
            const float detailGap = Ui::ScaledLayoutValue(2.0F);
            ImFont *const compactFont = ctx.theme.fonts.sansCompact ? ctx.theme.fonts.sansCompact : ImGui::GetFont();
            ImFont *const sansFont = ctx.theme.fonts.sans ? ctx.theme.fonts.sans : ImGui::GetFont();

            const float tagHeight = compactFont->CalcTextSizeA(Theme::TextPx::Label(), FLT_MAX, 0.0F, tag).y;
            const float titleHeight = sansFont->CalcTextSizeA(Theme::TextPx::Title(), FLT_MAX, 0.0F, title).y;
            const float descriptionHeight = sansFont->CalcTextSizeA(Theme::TextPx::Body(), FLT_MAX, contentWidth, description).y;

            // Match the exact sequence drawn below: three text items and one
            // explicit spacer, including the child border on both edges.
            const float contentHeight = tagHeight + titleHeight + descriptionHeight + detailGap + itemSpacing * 3.0F;
            return std::ceil((padding + style.ChildBorderSize) * 2.0F + contentHeight);
        }

        void DrawNewsCard(const char *tag, const char *title, const char *description, const ImVec2 size, const EditorGuiContext &ctx) {
            using namespace Theme;

            const float padding = Ui::ScaledLayoutValue(14.0F);
            Ui::ScopedCard card(title, size, padding, padding, ImVec4{0.0F, 0.0F, 0.0F, 0.0F});
            {
                ScopedTextStyle textStyle(ctx.theme.fonts.sansCompact, TextPx::Label(), FontPx::SansCompact);
                ImGui::PushStyleColor(ImGuiCol_Text, Accent());
                ImGui::TextUnformatted(tag);
                ImGui::PopStyleColor();
            }
            {
                ScopedTextStyle textStyle(ctx.theme.fonts.sans, TextPx::Title(), FontPx::Sans);
                ImGui::TextUnformatted(title);
            }
            ImGui::Dummy({0.0F, Ui::ScaledLayoutValue(2.0F)});
            {
                ScopedTextStyle textStyle(ctx.theme.fonts.sans, TextPx::Body(), FontPx::Sans);
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
                ImGui::TextDisabled("%s", description);
                ImGui::PopTextWrapPos();
            }
        }

        [[nodiscard]] bool DrawWelcomeActionButton(const char *label, const Ui::ButtonVariant variant, const EditorGuiContext &ctx) {
            return Ui::Button(Ui::ButtonProps{.label = label,
                                              .size = {0.0F, 42.0F},
                                              .variant = variant,
                                              .font = ctx.theme.fonts.sans,
                                              .componentSize = Ui::ComponentSize::Large,
                                              .style = {.width = Ui::StyleWidth::FillAvailable}});
        }

        /** @brief Draws the logo, title, and localized subtitle in the welcome sidebar. */
        void DrawWelcomeBrand(const WelcomeViewAssets &assets, const EditorGuiContext &ctx) {
            using namespace Theme;
            if (assets.logo != 0) {
                const float logoSize = Ui::ScaledLayoutValue(80.0F);
                ImGui::Image(assets.logo, {logoSize, logoSize});
            }
            ImGui::Dummy({0.0F, Ui::ScaledLayoutValue(4.0F)});

            {
                ImFont *titleFont = ctx.theme.fonts.sansEmphasis ? ctx.theme.fonts.sansEmphasis : ImGui::GetFont();
                const ImVec2 titlePos = ImGui::GetCursorScreenPos();
                const float titlePx = TextPx::Display();
                constexpr float titleSpacing = 2.0F;
                const char *title = "HORO";
                auto *dl = ImGui::GetWindowDrawList();
                float cursorX = titlePos.x;
                for (const char *c = title; *c != '\0'; ++c) {
                    const std::string glyph{*c};
                    dl->AddText(titleFont, titlePx, {cursorX, titlePos.y}, U32(Text()), glyph.c_str());
                    cursorX += titleFont->CalcTextSizeA(titlePx, FLT_MAX, 0.0F, glyph.c_str()).x + titleSpacing;
                }
                const ImVec2 titleSize = titleFont->CalcTextSizeA(titlePx, FLT_MAX, 0.0F, title);
                const float titleWidth = titleSize.x + titleSpacing * 3.0F;
                ImGui::Dummy({titleWidth, titleSize.y});
            }
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6.0F);
            {
                ImFont *subtitleFont = ctx.theme.fonts.sans ? ctx.theme.fonts.sans : ImGui::GetFont();
                const ImVec2 subtitlePos = ImGui::GetCursorScreenPos();
                const float subtitlePx = TextPx::Caption();
                const std::string subtitleText = ctx.localization.Get("editor", "welcome.subtitle");
                const char *subtitle = subtitleText.c_str();
                ImGui::GetWindowDrawList()->AddText(subtitleFont, subtitlePx, subtitlePos, U32(Muted()), subtitle);
                const ImVec2 subtitleSize = subtitleFont->CalcTextSizeA(subtitlePx, FLT_MAX, 0.0F, subtitle);
                ImGui::Dummy({subtitleSize.x, subtitleSize.y});
            }
            ImGui::Dummy({0.0F, 28.0F});
        }

        /** @brief Draws welcome navigation actions and records the selected command. */
        void DrawWelcomeSidebarActions(const EditorGuiContext &ctx, WelcomeViewResult &result) {
            const std::string newProject = ctx.localization.Get("editor", "welcome.new_project") + "###welcome_new_project";
            const std::string openProject = ctx.localization.Get("editor", "welcome.open_project") + "###welcome_open_project";
            const std::string openSettings = ctx.localization.Get("editor", "welcome.open_settings") + "###welcome_open_settings";
            const std::string pluginStore = ctx.localization.Get("editor", "welcome.plugin_store") + "###welcome_plugin_store";
            if (DrawWelcomeActionButton(newProject.c_str(), Ui::ButtonVariant::Primary, ctx)) {
                result.command = WelcomeViewCommand::NewProject;
            }
            if (DrawWelcomeActionButton(openProject.c_str(), Ui::ButtonVariant::Secondary, ctx)) {
                result.command = WelcomeViewCommand::OpenProject;
            }
            if (DrawWelcomeActionButton(openSettings.c_str(), Ui::ButtonVariant::Secondary, ctx)) {
                result.command = WelcomeViewCommand::OpenSettings;
            }
            static_cast<void>(Ui::Button(Ui::ButtonProps{.label = pluginStore.c_str(),
                                                         .size = {0.0F, 42.0F},
                                                         .variant = Ui::ButtonVariant::Secondary,
                                                         .enabled = false,
                                                         .font = ctx.theme.fonts.sans,
                                                         .componentSize = Ui::ComponentSize::Large,
                                                         .style = {.width = Ui::StyleWidth::FillAvailable}}));
        }

        /** @brief Draws the welcome sidebar within its own ImGui child window. */
        void DrawWelcomeSidebar(const WelcomeViewAssets &assets, const EditorGuiContext &ctx, WelcomeViewResult &result) {
            using namespace Theme;
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{Layout::WelcomePad, Layout::WelcomePad});
            ImGui::PushStyleColor(ImGuiCol_ChildBg, Bg2());
            ImGui::BeginChild("Side", {Layout::WelcomeSideW, 0.0F}, false,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_AlwaysUseWindowPadding);
            DrawWelcomeBrand(assets, ctx);
            DrawWelcomeSidebarActions(ctx, result);
            ImGui::EndChild();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar();
        }
    }  // namespace

    /** @copydoc VisibleWelcomeProjectIndices */
    std::vector<std::size_t> VisibleWelcomeProjectIndices(const WelcomeViewModel &model, const WelcomeViewState &state) {
        std::vector<std::size_t> indices(model.recentProjects.size());
        std::iota(indices.begin(), indices.end(), 0U);
        const std::string_view search{state.search.data()};
        std::erase_if(indices, [&](const std::size_t index) {
            return !MatchesSearch(model.recentProjects[index], search);
        });
        const auto compare = [&](const std::size_t left, const std::size_t right) {
            const RecentProjectEntry &lhs = model.recentProjects[left];
            const RecentProjectEntry &rhs = model.recentProjects[right];
            if (state.sortSelection == 1)
                return lhs.name == rhs.name ? left < right : lhs.name < rhs.name;
            if (state.sortSelection == 2)
                return lhs.rootPath == rhs.rootPath ? left < right : lhs.rootPath < rhs.rootPath;
            const auto modified = [&](const std::size_t index) {
                return index < state.projectModifiedTimes.size() ? state.projectModifiedTimes[index]
                                                                 : std::filesystem::file_time_type::min();
            };
            return modified(left) == modified(right) ? left < right : modified(left) > modified(right);
        };
        std::sort(indices.begin(), indices.end(), compare);
        return indices;
    }

    namespace {
        /** @brief Draws the search and sort controls and visible recent-project cards. */
        void DrawWelcomeProjects(const WelcomeViewModel &viewModel, WelcomeViewState &state, const EditorGuiContext &ctx,
                                 WelcomeViewResult &result) {
            using namespace Theme;
            const float scale = std::max(GetActiveTokens().sizes.uiScale, 0.01F);
            const float rowWidth = ImGui::GetContentRegionAvail().x;
            const float gap = Ui::ScaledLayoutValue(12.0F);
            const float sortWidth = std::min(Ui::ScaledLayoutValue(200.0F), rowWidth * 0.44F);
            const float searchWidth = std::max(1.0F, rowWidth - sortWidth - gap);
            const std::string &searchHint = ctx.localization.Get("editor", "welcome.search_projects");
            const bool searchChanged =
                Ui::InputTextControl("##welcome_project_search", state.search.data(), state.search.size(), ctx.theme.fonts,
                                     {.width = searchWidth / scale,
                                      .hint = searchHint.c_str(),
                                      .componentSize = Ui::ComponentSize::Medium});
            ImGui::SameLine(0.0F, gap);
            const std::array<const char *, 3> sortItems{ctx.localization.Get("editor", "welcome.sort.last_edited").c_str(),
                                                        ctx.localization.Get("editor", "welcome.sort.name").c_str(),
                                                        ctx.localization.Get("editor", "welcome.sort.path").c_str()};
            ImGui::SetNextItemWidth(sortWidth);
            const bool sortChanged =
                Ui::ComboControl("##welcome_project_sort", &state.sortSelection, sortItems.data(), static_cast<int>(sortItems.size()),
                                 ctx.theme.fonts, {.componentSize = Ui::ComponentSize::Medium});
            ImGui::Dummy({0.0F, 14.0F});

            if (state.visibleProjectsDirty || searchChanged || sortChanged) {
                state.visibleProjectIndices = VisibleWelcomeProjectIndices(viewModel, state);
                state.visibleProjectsDirty = false;
            }
            for (const std::size_t index : state.visibleProjectIndices) {
                const WelcomeViewCommand action = DrawProjectCard(viewModel.recentProjects[index], static_cast<int>(index), ctx);
                if (action != WelcomeViewCommand::None) {
                    result.command = action;
                    result.openRecentIndex = static_cast<int>(index);
                }
            }
        }

        /** @brief Draws localized news cards in paired rows. */
        void DrawWelcomeNews(const WelcomeViewModel &viewModel, const EditorGuiContext &ctx) {
            using namespace Theme;
            ImGui::Dummy({0.0F, 28.0F});
            {
                ScopedTextStyle textStyle(ctx.theme.fonts.sansEmphasis, TextPx::Label(), FontPx::SansEmphasis);
                const std::string whatsNewStr = ctx.localization.Get("editor", "welcome.whats_new");
                ImGui::TextDisabled("%s", whatsNewStr.c_str());
            }
            ImGui::Dummy({0.0F, 14.0F});

            const float newsGap = Ui::ScaledLayoutValue(12.0F);
            const float newsWidth = (ImGui::GetContentRegionAvail().x - newsGap) * 0.5F;
            for (std::size_t rowStart = 0; rowStart < viewModel.whatsNew.size(); rowStart += 2) {
                float rowHeight = 0.0F;
                const std::size_t rowEnd = std::min(rowStart + 2, viewModel.whatsNew.size());
                for (std::size_t i = rowStart; i < rowEnd; ++i) {
                    const auto &entry = viewModel.whatsNew[i];
                    rowHeight = std::max(rowHeight, NewsCardHeight(entry.tag, entry.title, entry.body, newsWidth, ctx));
                }

                for (std::size_t i = rowStart; i < rowEnd; ++i) {
                    if (i > rowStart)
                        ImGui::SameLine(0.0F, newsGap);
                    const auto &entry = viewModel.whatsNew[i];
                    ImGui::PushID(static_cast<int>(i));
                    DrawNewsCard(entry.tag, entry.title, entry.body, {newsWidth, rowHeight}, ctx);
                    ImGui::PopID();
                }
            }
        }
    }  // namespace

    [[nodiscard]] WelcomeViewResult DrawWelcomeView(const WelcomeViewModel &viewModel, WelcomeViewState &state, const EditorGuiContext &ctx,
                                                    const WelcomeViewAssets &assets, const GuiContentRegion &contentRegion) {
        using namespace Theme;

        ImGui::SetNextWindowPos(ImVec2{contentRegion.x, contentRegion.y});
        ImGui::SetNextWindowSize(ImVec2{contentRegion.width, contentRegion.height});
        constexpr auto flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                               ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

        WelcomeViewResult result;

        ImGui::PushStyleColor(ImGuiCol_WindowBg, Bg1());
        ImGui::Begin("Welcome", nullptr, flags);

        const ImVec2 available = ImGui::GetContentRegionAvail();

        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, Layout::RadiusModal);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0F, 0.0F});
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Bg1());
        ImGui::BeginChild("WelcomeCard", available, true, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(2);

        auto *cardDrawList = ImGui::GetWindowDrawList();
        const ImVec2 cardMin = ImGui::GetWindowPos();

        DrawWelcomeSidebar(assets, ctx, result);

        cardDrawList->AddLine({cardMin.x + Layout::WelcomeSideW, cardMin.y}, {cardMin.x + Layout::WelcomeSideW, cardMin.y + available.y},
                              U32(Border()), 1.0F);

        ImGui::SameLine(0.0F, 0.0F);

        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{Layout::WelcomePad, Layout::WelcomePad});
        ImGui::BeginChild("Main", {0.0F, 0.0F}, false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_AlwaysUseWindowPadding);

        DrawWelcomeProjects(viewModel, state, ctx, result);
        DrawWelcomeNews(viewModel, ctx);

        ImGui::EndChild();
        ImGui::PopStyleVar();

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::End();
        ImGui::PopStyleColor();

        return result;
    }
}  // namespace Horo::Editor
