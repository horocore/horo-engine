#pragma once

#include "Horo/Editor/GuiRoute.h"
#include "Horo/Editor/RecentProject.h"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace Horo::Editor {

    /**
     * @file WelcomeController.h
     * @brief View-model and command contract for the HoroEditor welcome screen controller.
     */

    /**
     * @brief User-facing action exposed by the welcome screen.
     */
    enum class WelcomeActionKind {
        CreateProject,
        OpenProject,
        OpenRecentProject,
        ShowProjectBrowser,
    };

    /**
     * @brief Navigation-producing welcome screen action.
     */
    struct WelcomeAction {
        WelcomeActionKind kind;
        GuiRoute route;
    };

    /**
     * @brief A single 'What's New' card entry shown on the Welcome Screen.
     *
     * Data is projected at build time from the reviewed release-notes snapshot via GeneratedBuildInfo.h.
     * Defined here (not in the generated header) so public consumers do not need
     * the generated include path.
     */
    struct WhatsNewEntry {
        const char *tag = "";    ///< Card category label, e.g. "Release Notes"
        const char *title = "";  ///< Card headline
        const char *body = "";   ///< Short description (≤120 chars)
    };

    /**
     * @brief Immutable data required by the welcome screen renderer.
     */
    struct WelcomeViewModel {
        std::string productName;
        std::string statusLabel;
        std::vector<RecentProjectEntry> recentProjects;
        /// Up to two 'What's New' entries projected from the reviewed release-notes snapshot at build time.
        std::array<WhatsNewEntry, 2> whatsNew{};
    };

    /**
     * @brief Minimal controller for HoroEditor startup/project-browser style screens.
     *
     * The controller owns no GUI backend state. It produces typed route requests that
     * `GuiScreenHost` can validate and execute. This keeps the startup project
     * selection experience inside HoroEditor instead of introducing a separate app.
     */
    class WelcomeScreenController {
    public:
        /**
         * @brief Creates a controller from already-loaded recent project entries.
         * @param recentProjects Recent project entries from user state.
         */
        explicit WelcomeScreenController(std::vector<RecentProjectEntry> recentProjects);

        /**
         * @brief Builds the immutable view model for rendering.
         * @return Data required by the welcome screen presentation layer.
         */
        [[nodiscard]] WelcomeViewModel BuildViewModel() const;

        /**
         * @brief Creates a route action for the project creation screen.
         * @return Navigation action targeting project creation.
         */
        [[nodiscard]] WelcomeAction RequestCreateProject() const;

        /**
         * @brief Creates a route action for the project browser screen.
         * @return Navigation action targeting project browsing/opening.
         */
        [[nodiscard]] WelcomeAction RequestOpenProject() const;

        /**
         * @brief Creates a route action for opening a recent project.
         * @param index Zero-based recent project index.
         * @return Navigation action when the index points at a displayable recent project.
         */
        [[nodiscard]] std::optional<WelcomeAction> RequestOpenRecentProject(std::size_t index) const;

    private:
        std::vector<RecentProjectEntry> recentProjects_;
    };

    /**
     * @brief Formats a deterministic text preview of the welcome screen.
     * @param viewModel View model to render.
     * @return Multi-line text intended for the temporary console host.
     */
    [[nodiscard]] std::string RenderWelcomeScreenText(const WelcomeViewModel &viewModel);

}  // namespace Horo::Editor
