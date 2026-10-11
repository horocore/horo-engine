#pragma once

/** @file NativeProjectReadFiles.h
 * @brief Explicit native composition of a read-only root authority.
 */
#include "Horo/Platform/ProjectReadFiles.h"

#include <filesystem>

namespace Horo::Platform {
    /** @brief Opens and retains one explicitly authorized directory identity.
     * Queries open descendants relative to retained handles and reject links/reparse points,
     * special files and regular files with multiple hard links before reading content.
     * Operations are synchronous and check stop/deadline between bounded native calls;
     * an operating-system call already in progress cannot be forcibly interrupted.
     * @param authorizedRoot Absolute host-authorized directory.
     * @param projectIdentity Exact authority identity. @param projectGeneration Nonzero authority generation.
     * @return Owned capability or typed admission failure without native path disclosure.
     */
    [[nodiscard]] Result<std::shared_ptr<const IProjectReadFiles>> CreateNativeProjectReadFiles(const std::filesystem::path &authorizedRoot,
                                                                                                std::string projectIdentity,
                                                                                                std::uint64_t projectGeneration);
}  // namespace Horo::Platform
