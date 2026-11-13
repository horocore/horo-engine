#pragma once

/** @file GuiLiveBuildRevision.h
 * @brief Host-build identity for private development compiler objects.
 */
#include <chrono>
#include <filesystem>
#include <string>

namespace Horo::Editor::Development {
    /** @brief Encodes native file timestamps without narrowing their full tick count.
     * @param revision Valid host image timestamp, with at most nanosecond resolution.
     * @return One path component preserving both seconds and the fractional timestamp.
     */
    [[nodiscard]] inline std::filesystem::path GuiLiveBuildRevision(const std::filesystem::file_time_type revision) {
        const auto elapsed = revision.time_since_epoch();
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed);
        const auto fraction = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed - seconds);
        return std::filesystem::path{std::to_string(seconds.count()) + "_" + std::to_string(fraction.count())};
    }
}  // namespace Horo::Editor::Development
