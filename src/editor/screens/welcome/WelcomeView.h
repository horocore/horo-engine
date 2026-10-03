#pragma once

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/WelcomeController.h"

#include <array>
#include <filesystem>
#include <imgui.h>
#include <vector>

namespace Horo::Editor {
    struct GuiContentRegion;

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
