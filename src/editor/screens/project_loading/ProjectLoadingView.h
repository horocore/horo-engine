#pragma once

#include "Horo/Editor/EditorGuiContext.h"
#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/Localization/LocalizedGui.h"

#include <string>

namespace Horo::Editor {
    /** @brief Shared route-screen and view localization scope. */
    inline constexpr GuiLocalizationScope ProjectLoadingTextScope{"project_loading"};
    struct GuiContentRegion;

    enum class ProjectLoadingViewCommand {
        None,
        Cancel,
        Retry,
        Back
    };

    /**
     * @brief State for the project loading simulation/UI view.
     */
    struct ProjectLoadingViewState {
        std::string projectName;
        std::string projectRoot;
        float progress = 0.0f;
        std::string statusText = "Preparing workspace...";
        bool isCancelled = false;
        bool hasFailed = false;
        bool canRetry = false;
    };

    /**
     * @brief Draws the project loading screen overlay/view.
     * @param state The current loading state to visualize.
     * @param ctx Editor GUI context and fonts.
     * @return Command if the user interacts (e.g. Cancel).
     */
    [[nodiscard]] ProjectLoadingViewCommand DrawProjectLoadingView(ProjectLoadingViewState &state, const EditorGuiContext &ctx,
                                                                   const GuiContentRegion &contentRegion);
}  // namespace Horo::Editor
