#pragma once

#include "Horo/Application/ProjectCompatibility.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace Horo::Editor {

    /**
     * @file RecentProject.h
     * @brief Recent project value types used by HoroEditor startup screens.
     */

    /** @brief Freshness of compatibility metadata projected into one recent-project card. */
    enum class RecentProjectInspectionState : std::uint8_t {
        Cached,
        Refreshing,
        Fresh,
    };

    /** @brief Display-only typed compatibility projection; never project-open authority. */
    struct RecentProjectCompatibilityProjection {
        std::optional<Horo::Application::EngineReleaseVersion> projectVersion;
        Horo::Application::ProjectCompatibilityStatus status{Horo::Application::ProjectCompatibilityStatus::Inaccessible};
        Horo::Application::EngineReleaseVersion targetVersion;
        RecentProjectInspectionState inspectionState{RecentProjectInspectionState::Cached};
    };

    /**
     * @brief Recent project entry shown by Welcome and Project Browser screens.
     */
    struct RecentProjectEntry {
        std::string name;
        std::string rootPath;
        std::string lastOpenedLabel;
        std::string thumbnailKey;
        std::optional<RecentProjectCompatibilityProjection> compatibility;
    };

    /**
     * @brief Returns true when an entry has the minimum data needed for display and navigation.
     * @param entry Entry to validate.
     * @return True when the name is non-empty and the root path is absolute.
     */
    [[nodiscard]] bool IsDisplayableRecentProject(const RecentProjectEntry &entry) noexcept;

    /**
     * @brief Loads the recent project entries from user local storage (~/.horo/recent_projects.json).
     * @return Vector of valid recent project entries.
     */
    [[nodiscard]] std::vector<RecentProjectEntry> LoadRecentProjectsFromDisk();

    /**
     * @brief Saves the given recent projects vector to user local storage (~/.horo/recent_projects.json).
     * @param projects List of recent project entries to persist.
     * @return True on success, false if writing fails.
     */
    bool SaveRecentProjectsToDisk(const std::vector<RecentProjectEntry> &projects);

    /**
     * @brief Deletes a validated, standalone Horo project directory and its contents.
     * @param root Absolute project root selected by the user after confirmation.
     * @return True when the project directory was removed without filesystem errors.
     */
    bool DeleteRecentProjectFiles(const std::filesystem::path &root);

}  // namespace Horo::Editor
