#pragma once

/** @file SourceReviewHistory.h @brief Bounded local history for redacted source review applications. */
#include "Horo/Editor/ProjectMutation.h"
#include "Horo/Editor/ProposedChangeSet.h"

namespace Horo::Editor {
    /** @brief Maximum number of recent application records retained per project. */
    inline constexpr std::size_t SourceReviewHistoryCapacity = 16;

    /** @brief Reads the newest valid historical audit without opening or changing any source.
     * @param root Authorized project root. @param stop Cooperative cancellation.
     * @return The latest audit, an empty pointer for absent history, or a typed read/format error.
     */
    [[nodiscard]] Result<std::shared_ptr<const Proposals::SourceReviewAudit>> ReadSourceReviewHistory(const std::filesystem::path &root,
                                                                                                      const CancellationToken &stop = {});

    /** @brief Appends one applied audit and atomically retains the newest bounded records.
     * @param root Authorized project root. @param audit Content-free application receipt.
     * @param mutations Shared project authority. @param files Tracked filesystem. @param stop Cooperative cancellation.
     * @return Success only after confirmed durable publication. Corrupt history remains available for inspection.
     */
    [[nodiscard]] Result<void> AppendSourceReviewHistory(const std::filesystem::path &root, const Proposals::SourceReviewAudit &audit,
                                                         const ProjectMutationCoordinator &mutations, DurableFileSystem &files,
                                                         const CancellationToken &stop = {});
}  // namespace Horo::Editor
