#pragma once

/**
 * @file PathUtils.h
 * @brief OS-aware path utilities for directory creation, project path resolution,
 *        and cross-platform path normalization.
 *
 * These are thin, safe wrappers over std::filesystem that return Horo Result types
 * and enforce project-root containment.
 */

#include "Horo/Foundation/Paths.h"
#include "Horo/Foundation/Result.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace Horo::Foundation::Paths {

    /**
     * @brief Creates all directories in the given path (like mkdir -p).
     *
     * Wraps std::filesystem::create_directories with Horo error reporting.
     * Succeeds silently if the directory already exists.
     *
     * @param path Absolute or relative directory path to create.
     * @return Success, or a typed error on filesystem failure.
     */
    [[nodiscard]] Result<void> EnsureDirectory(const std::filesystem::path &path);

    /**
     * @brief Resolves a validated ProjectPath against a project root.
     *
     * Produces an absolute filesystem path without any possibility of
     * root escape (already enforced by ProjectPath construction).
     *
     * @param projectRoot Absolute path to the open project root.
     * @param relative A previously-parsed, validated ProjectPath.
     * @return The absolute filesystem path: projectRoot / relative segments.
     */
    [[nodiscard]] std::filesystem::path Resolve(const std::filesystem::path &projectRoot, const ProjectPath &relative);

    /**
     * @brief Parses and resolves a relative path string against a project root.
     *
     * Validates that the relative path does not escape the project root (no .. beyond
     * root), normalizes separators, and returns an absolute path.
     *
     * @param projectRoot Absolute path to the open project root.
     * @param relative A project-relative path string using forward slashes.
     * @return The resolved absolute path, or an error if the path escapes the root.
     */
    [[nodiscard]] Result<std::filesystem::path> Resolve(const std::filesystem::path &projectRoot, std::string_view relative);

    /**
     * @brief Normalizes path separators to the native OS form.
     *
     * On Windows, converts forward slashes to backslashes.
     * On POSIX, converts backslashes to forward slashes.
     * Does not modify the filesystem.
     *
     * @param path Path string to normalize in-place.
     */
    /**
     * @brief Checks if a path is located inside or equals a root directory.
     *
     * Handles cross-platform separator normalization, drive letter casing,
     * short 8.3 paths, and symlink resolution safely.
     *
     * @param root Base path prefix.
     * @param candidate Target path to check.
     * @return True if candidate resides within or equals root. Ordinary filesystem lookup failures retain
     * the canonical/lexical fallback; they are never interpreted as successful physical equivalence.
     * @throws std::bad_alloc Path normalization, traversal or comparison could not allocate. Callers that
     * publish state must complete this fallible admission before publication; allocation failure is not
     * converted to an ordinary filesystem mismatch.
     */
    [[nodiscard]] bool HasPathPrefix(const std::filesystem::path &root, const std::filesystem::path &candidate);

}  // namespace Horo::Foundation::Paths
