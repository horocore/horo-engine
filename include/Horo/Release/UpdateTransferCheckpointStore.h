#pragma once

/**
 * @file UpdateTransferCheckpointStore.h
 * @brief Durable private-file checkpoint persistence and fail-closed recovery.
 */

#include "Horo/Foundation/Platform.h"
#include "Horo/Release/UpdateTransfer.h"

#include <filesystem>
#include <optional>

namespace Horo::Release {
    /**
     * @brief Loads a private partial file's checkpoint only when both files exist and their lengths agree.
     * @param partialFile Host-owned private package file, never an installed-product path.
     * @param checkpointFile Host-owned private checkpoint path beside the package file.
     * @return No checkpoint when both files are absent, exact parsed evidence when consistent, or a typed error.
     */
    [[nodiscard]] Result<std::optional<UpdateTransferCheckpoint>> LoadUpdateTransferCheckpoint(const std::filesystem::path &partialFile,
                                                                                               const std::filesystem::path &checkpointFile);

    /**
     * @brief Atomically publishes checkpoint evidence only after matching package bytes are durable.
     * @param files Host-composed durable filesystem; it performs same-filesystem atomic replacement.
     * @param partialFile Already durable private package file.
     * @param checkpointFile Private checkpoint destination distinct from the partial file.
     * @param checkpoint Evidence whose durable byte count must equal the partial file length.
     * @return Success only after the complete canonical checkpoint is durably replaced.
     */
    [[nodiscard]] Result<void> SaveUpdateTransferCheckpoint(DurableFileSystem &files, const std::filesystem::path &partialFile,
                                                            const std::filesystem::path &checkpointFile,
                                                            const UpdateTransferCheckpoint &checkpoint);
}  // namespace Horo::Release
