#pragma once

/**
 * @file ReleaseRunHistory.h
 * @brief Bounded, restart-readable release job snapshots without credential-bearing text.
 */

#include "Horo/Foundation/Platform.h"
#include "Horo/Release/ReleaseJobTracker.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace Horo::Release {
    /** @brief Persisted public-safe projection of one release job. */
    struct ReleaseRunHistoryEntry final {
        ReleaseJobId job;
        ReleaseTargetId target;
        OperationId operation{};
        std::uint64_t revision{};
        ReleaseJobState state{ReleaseJobState::Queued};
        std::array<ReleaseStageState, ReleaseStageCount> stages{};
        std::array<std::optional<ReleaseStageAttemptId>, ReleaseStageCount> attempts{};
        std::optional<ReleaseCandidateId> candidate;
        std::int64_t createdUtcMilliseconds{};
        std::int64_t updatedUtcMilliseconds{};
        std::optional<std::int64_t> finishedUtcMilliseconds;
        bool interruptedByRestart{}; /**< A prior process ended without a durable terminal transition. */
    };

    /** @brief Host-owned durable history with one writer lock and bounded replacement snapshots. */
    class ReleaseRunHistory final {
    public:
        /**
         * @brief Opens a bounded history file and recovers valid snapshots under an exclusive writer lock.
         * @param files Host-owned durable filesystem, which must outlive the store.
         * @param path Absolute history-file path outside published release artifacts.
         * @param capacity Maximum retained jobs, from one to 1024.
         * @return Owned store or typed failure for unsafe, corrupt, or unavailable storage.
         */
        [[nodiscard]] static Result<std::unique_ptr<ReleaseRunHistory>> Open(DurableFileSystem &files, const std::filesystem::path &path,
                                                                             std::size_t capacity = 128U);

        /**
         * @brief Atomically replaces one job snapshot after durable file publication.
         * @param snapshot Authoritative live job snapshot; only typed identities and states are stored.
         * @param updatedUtcMilliseconds Host wall-clock timestamp.
         * @return Success, or failure with the previous in-memory and durable history intact.
         */
        [[nodiscard]] Result<void> Record(const ReleaseJobSnapshot &snapshot, std::int64_t updatedUtcMilliseconds);

        /** @brief Copies retained records in ascending job order. @return Restart-readable snapshots. */
        [[nodiscard]] std::vector<ReleaseRunHistoryEntry> List() const;
        /** @brief Counts snapshots removed by bounded retention. @return Cumulative count. */
        [[nodiscard]] std::uint64_t Dropped() const;
        /** @brief Returns the largest candidate identity ever persisted, including evicted records. @return High-water mark. */
        [[nodiscard]] std::uint64_t HighestCandidate() const;

    private:
        ReleaseRunHistory(DurableFileSystem &files, std::filesystem::path path, std::size_t capacity, ExclusiveFileLock lock,
                          std::vector<ReleaseRunHistoryEntry> entries, std::uint64_t dropped, std::uint64_t highestCandidate);

        DurableFileSystem &files_;
        const std::filesystem::path path_;
        const std::size_t capacity_;
        ExclusiveFileLock lock_;
        mutable std::mutex mutex_;
        std::vector<ReleaseRunHistoryEntry> entries_;
        std::uint64_t dropped_{};
        std::uint64_t highestCandidate_{};
    };
}  // namespace Horo::Release
