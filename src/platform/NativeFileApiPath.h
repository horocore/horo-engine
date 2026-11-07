#pragma once

#include <filesystem>
#include <string>

namespace Horo::PlatformFileInternal {
    /** @brief Converts canonical absolute Windows paths only at the native I/O boundary, without changing caller path authority. */
    [[nodiscard]] inline std::filesystem::path NativeApiPath(const std::filesystem::path &path) {
#if defined(_WIN32)
        if (!path.is_absolute() || path != path.lexically_normal())
            return path;
        auto preferred = path;
        preferred.make_preferred();
        const auto &text = preferred.native();
        if (text.starts_with(L"\\\\?\\") || text.starts_with(L"\\\\.\\"))
            return preferred;
        if (text.starts_with(L"\\\\"))
            return std::filesystem::path{std::wstring{L"\\\\?\\UNC\\"} + text.substr(2)};
        return std::filesystem::path{std::wstring{L"\\\\?\\"} + text};
#else
        return path;
#endif
    }
}  // namespace Horo::PlatformFileInternal
