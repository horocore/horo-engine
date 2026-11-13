#pragma once

#include "Horo/Platform/FileManager.h"

#include <string>

namespace Horo::FileManagerInternal {
    /** @brief Validated native path plus the directory/file distinction needed by each desktop. */
    struct Target {
        std::filesystem::path path;
        bool directory{};
    };

    /** @brief Validates without resolving links or converting through the process locale.
     * @param path Absolute native filesystem path.
     * @return A regular-file/directory target or a typed rejection.
     */
    [[nodiscard]] Result<Target> PrepareTarget(const std::filesystem::path &path);

    /** @brief Encodes an absolute POSIX path as a local file URI, preserving only slashes and URI-unreserved bytes.
     * @param path Validated absolute directory path.
     * @return Percent-encoded file URI; no path bytes can become URI query, fragment or authority syntax.
     */
    [[nodiscard]] std::string DirectoryUri(const std::filesystem::path &path);

    /** @brief Dispatches one validated target using the selected desktop adapter.
     * @param target Borrowed request, consumed synchronously on the host main thread.
     * @return Native acceptance or a typed failure.
     */
    [[nodiscard]] Result<void> Dispatch(const Target &target);
}  // namespace Horo::FileManagerInternal
