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

    /** @brief Appends a new application or updates the exact current history entry under the same lease.
     * @param root Authorized project root. @param audit Content-free application receipt.
     * @param sequence Zero for a new entry, otherwise the identity returned by the durable append.
     * @param outcome Exact latest observation of the application.
     * @param mutations Shared project authority. @param files Tracked filesystem. @param stop Cooperative cancellation.
     * @return Durable sequence, or a typed error that preserves corrupt or concurrently changed history.
     */
    [[nodiscard]] Result<std::uint64_t> WriteSourceReviewHistory(const std::filesystem::path &root,
                                                                 const Proposals::SourceReviewAudit &audit, std::uint64_t sequence,
                                                                 Proposals::SourceReviewDisposition outcome,
                                                                 const ProjectMutationCoordinator &mutations, DurableFileSystem &files,
                                                                 const CancellationToken &stop = {});
}  // namespace Horo::Editor
