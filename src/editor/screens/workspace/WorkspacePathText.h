#pragma once

/** @file WorkspacePathText.h @brief Explicit native-path and UTF-8 workspace presentation boundaries. */

#include <filesystem>
#include <string>
#include <string_view>

namespace Horo::Editor {
    /** @brief Encodes a native path for workspace text and command payloads without an ANSI fallback.
     * @param path Native path. @return UTF-8 text retaining native separators. */
    [[nodiscard]] inline std::string WorkspacePathText(const std::filesystem::path &path) {
        const auto encoded = path.u8string();
        return {encoded.begin(), encoded.end()};
    }

    /** @brief Encodes a native path as portable UTF-8 with forward separators.
     * @param path Native path. @return UTF-8 portable path text. */
    [[nodiscard]] inline std::string WorkspacePortablePathText(const std::filesystem::path &path) {
        const auto encoded = path.generic_u8string();
        return {encoded.begin(), encoded.end()};
    }

    /** @brief Decodes workspace UTF-8 path text before native filesystem admission.
     * @param text Valid UTF-8 workspace path text. @return Native path; no access authority is granted. */
    [[nodiscard]] inline std::filesystem::path WorkspaceNativePath(const std::string_view text) {
        return std::filesystem::path{std::u8string{text.begin(), text.end()}};
    }
}  // namespace Horo::Editor
