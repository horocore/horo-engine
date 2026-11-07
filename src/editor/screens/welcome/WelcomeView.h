#pragma once

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/Localization/LocalizedGui.h"
#include "Horo/Editor/WelcomeController.h"

#include <array>
#include <filesystem>
#include <imgui.h>
#include <vector>

namespace Horo::Editor {
    /** @brief Shared route-screen and view localization scope. */
    inline constexpr GuiLocalizationScope WelcomeTextScope{"welcome"};
    struct GuiContentRegion;

    /** @brief Validated welcome geometry resolved from active design tokens each frame. */
    struct WelcomeLayout {
        float sidebarWidth{};
        float sidebarPaddingX{};
        float sidebarPaddingY{};
        float bodyPaddingX{};
        float bodyPaddingY{};
        float cardPadding{};
        float gridGap{};
        float sectionGap{};
        std::uint32_t newsColumns{2};
    };

    /** @brief Texture handles required by the welcome view renderer. */
    struct WelcomeViewAssets {
        ImTextureID logo = 0;
    };

    /** @brief Commands emitted by the welcome view renderer. */
    enum class WelcomeViewCommand {
        None,
        NewProject,
        OpenSettings,
        OpenRecentProject,  ///< @see WelcomeViewResult::openRecentIndex
        OpenProject,
        RemoveRecentProject,
        DeleteRecentProject,
    };

    /**
     * @brief Result of a single welcome view render frame.
     *
     * Carries the command and, for commands that carry a payload (e.g.
     * `OpenRecentProject`), the associated index into
     * `WelcomeViewModel::recentProjects`.
     */
    struct WelcomeViewResult {
        WelcomeViewCommand command = WelcomeViewCommand::None;
        /// Zero-based index into WelcomeViewModel::recentProjects.
        /// Valid only when command == OpenRecentProject.
        int openRecentIndex = -1;
    };

    /** @brief Welcome-only search, sort, and cached project modification times. */
    struct WelcomeViewState {
        std::array<char, 256> search{};
        int sortSelection{};
        std::vector<std::filesystem::file_time_type> projectModifiedTimes;
        std::vector<std::size_t> visibleProjectIndices;
        bool visibleProjectsDirty{true};
    };

    /** @brief Returns indices into the original project list after search and sort. */
    [[nodiscard]] std::vector<std::size_t> VisibleWelcomeProjectIndices(const WelcomeViewModel &model, const WelcomeViewState &state);

    /** @brief Resolves active theme geometry for the current content region.
     * @param region Current available content bounds.
     * @return Finite, bounded geometry shared by measurement and drawing in this frame.
     */
    [[nodiscard]] WelcomeLayout ResolveWelcomeLayout(const GuiContentRegion &region);

    /**
     * @brief Draws the HoroEditor welcome view using design-system components.
     * @param viewModel Immutable welcome screen data.
     * @param ctx Editor GUI context and fonts.
     * @param assets Texture handles used by the view.
     * @return Result describing the user action that occurred this frame.
     */
    [[nodiscard]] WelcomeViewResult DrawWelcomeView(const WelcomeViewModel &viewModel, WelcomeViewState &state, const EditorGuiContext &ctx,
                                                    const WelcomeViewAssets &assets, const GuiContentRegion &contentRegion);
}  // namespace Horo::Editor
