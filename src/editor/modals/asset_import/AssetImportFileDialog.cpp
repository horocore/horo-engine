/** @brief Native source and destination picker adapters for asset import. */
#include "AssetImportFileDialog.h"

#include <cstdio>
#include <string>

namespace Horo::Editor {
    namespace {
        /** @brief Reads newline-separated native picker output into paths. */
        [[nodiscard]] std::vector<std::filesystem::path> ReadSelectedPaths(FILE *pipe) {
            std::vector<std::filesystem::path> paths;
            if (!pipe)
                return paths;
            char buffer[4096];
            while (fgets(buffer, sizeof(buffer), pipe)) {
                std::string line{buffer};
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
                    line.pop_back();
                if (!line.empty())
                    paths.emplace_back(line);
            }
            return paths;
        }
    }  // namespace

    /** @copydoc ChooseAssetImportFiles */
    [[nodiscard]] std::vector<std::filesystem::path> ChooseAssetImportFiles() {
#if defined(__APPLE__)
        FILE *pipe = popen("osascript -e 'set selectedFiles to choose file with multiple selections allowed' "
                           "-e 'set output to \"\"' -e 'repeat with selectedFile in selectedFiles' "
                           "-e 'set output to output & POSIX path of selectedFile & linefeed' "
                           "-e 'end repeat' -e 'return output' "
                           "2>/dev/null",
                           "r");
#elif defined(__linux__)
        FILE *pipe = popen("zenity --file-selection --multiple --separator='|' 2>/dev/null", "r");
#elif defined(_WIN32)
        FILE *pipe = _popen("powershell -NoProfile -Command \"Add-Type -AssemblyName System.Windows.Forms; "
                            "$d = New-Object System.Windows.Forms.OpenFileDialog; $d.Multiselect = $true; "
                            "if ($d.ShowDialog() -eq 'OK') { $d.FileNames }\" 2>nul",
                            "r");
#else
        return {};
#endif
        auto paths = ReadSelectedPaths(pipe);
#if defined(__linux__)
        if (paths.size() == 1) {
            const std::string combined = paths.front().string();
            paths.clear();
            for (std::size_t start = 0; start < combined.size();) {
                const auto end = combined.find('|', start);
                paths.emplace_back(combined.substr(start, end == std::string::npos ? end : end - start));
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
        }
#endif
#if defined(_WIN32)
        if (pipe)
            static_cast<void>(_pclose(pipe));
#else
        if (pipe)
            static_cast<void>(pclose(pipe));
#endif
        return paths;
    }

    /** @copydoc ChooseAssetImportFolder */
    [[nodiscard]] std::optional<std::filesystem::path> ChooseAssetImportFolder() {
#if defined(__APPLE__)
        FILE *pipe = popen("osascript -e 'POSIX path of (choose folder)' 2>/dev/null", "r");
#elif defined(__linux__)
        FILE *pipe = popen("zenity --file-selection --directory 2>/dev/null", "r");
#elif defined(_WIN32)
        FILE *pipe = _popen("powershell -NoProfile -Command \"Add-Type -AssemblyName System.Windows.Forms; "
                            "$d = New-Object System.Windows.Forms.FolderBrowserDialog; "
                            "if ($d.ShowDialog() -eq 'OK') { $d.SelectedPath }\" 2>nul",
                            "r");
#else
        return std::nullopt;
#endif
        auto paths = ReadSelectedPaths(pipe);
#if defined(_WIN32)
        if (pipe)
            static_cast<void>(_pclose(pipe));
#else
        if (pipe)
            static_cast<void>(pclose(pipe));
#endif
        return paths.empty() ? std::nullopt : std::optional{paths.front()};
    }

}  // namespace Horo::Editor
