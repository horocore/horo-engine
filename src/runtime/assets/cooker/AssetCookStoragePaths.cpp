/**
 * @file
 * @brief Enforces shared plain-path and portable artifact-filename safety for cook storage.
 */
#include "AssetCookStorageInternal.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace Horo::Assets::CookStorageDetail {
    /** @copydoc IsSafeArtifactFile */
    [[nodiscard]] bool IsSafeArtifactFile(const std::string_view file) noexcept {
        if (file.empty() || file.size() > 256)
            return false;
        for (const char c : file) {
            const auto character = static_cast<unsigned char>(c);
            if (character == '/' || character == '\\' || character == ':')
                return false;
            if (character < 0x20 || character == '"' || character == '<' || character == '>' || character == '|' || character == '?' ||
                character == '*')
                return false;
        }
        if (file.find("..") != std::string_view::npos)
            return false;
        // Windows strips trailing dots/spaces, silently renaming the file.
        if (file.back() == '.' || file.back() == ' ')
            return false;
        // Reserved DOS device basenames are invalid even with an extension.
        const auto stem = file.substr(0, file.find('.'));
        static constexpr std::array<std::string_view, 27> reserved{"CON",  "PRN",  "AUX",  "NUL",    "COM0",    "COM1",  "COM2",
                                                                   "COM3", "COM4", "COM5", "COM6",   "COM7",    "COM8",  "COM9",
                                                                   "LPT0", "LPT1", "LPT2", "LPT3",   "LPT4",    "LPT5",  "LPT6",
                                                                   "LPT7", "LPT8", "LPT9", "CONIN$", "CONOUT$", "CLOCK$"};
        for (const auto device : reserved)
            if (stem.size() == device.size() && std::equal(stem.begin(), stem.end(), device.begin(), [](const char a, const char b) {
                return std::toupper(static_cast<unsigned char>(a)) == b;
            }))
                return false;
        return true;
    }

    /** @copydoc IsSafePathWithin */
    [[nodiscard]] bool IsSafePathWithin(const std::filesystem::path &base, const std::filesystem::path &candidate) noexcept {
        if (base.empty() || candidate.empty() || !base.is_absolute() || !candidate.is_absolute())
            return false;
        std::error_code error;
        const auto canonicalBase = std::filesystem::weakly_canonical(base, error);
        if (error)
            return false;
        const auto canonicalCandidate = std::filesystem::weakly_canonical(candidate, error);
        if (error)
            return false;
        const auto relative = canonicalCandidate.lexically_relative(canonicalBase);
        return !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
    }

    /** @copydoc HasPlainPath */
    [[nodiscard]] bool HasPlainPath(const std::filesystem::path &path) {
        if (path.empty() || !path.is_absolute())
            return false;
        std::filesystem::path componentPath;
        for (const auto &component : path) {
            if (component == "." || component == "..")
                return false;
            componentPath /= component;
            std::error_code error;
            const auto status = std::filesystem::symlink_status(componentPath, error);
            if (error && error != std::errc::no_such_file_or_directory)
                return false;
            if (std::filesystem::is_symlink(status))
                return false;
        }
        return true;
    }

    /** @copydoc IsPlainFile */
    [[nodiscard]] bool IsPlainFile(const std::filesystem::path &path) {
        if (!HasPlainPath(path))
            return false;
        std::error_code error;
        if (const auto status = std::filesystem::symlink_status(path, error); error || !std::filesystem::is_regular_file(status))
            return false;
        return std::filesystem::hard_link_count(path, error) == 1U && !error;
    }
}  // namespace Horo::Assets::CookStorageDetail
