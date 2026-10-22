#pragma once

/**
 * @file StreamingCellState.h
 * @brief Canonical cell residency and generation authority for World Streaming.
 */

#include "Horo/WorldStreaming/StreamingCellOperation.h"
#include "Horo/WorldStreaming/WorldStreamingRuntimeComposition.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace Horo::WorldStreaming {
    /** @brief Canonical residency fact, distinct from provider barriers and cell-operation phases. */
    enum class StreamingCellState : std::uint8_t {
        Unloaded,
        Loading,
        Resident,
        Active,
        Evicting,
        Failed,
    };

    /** @brief Exact residency snapshot for one mounted cell-attempt generation. */
    struct StreamingCellStateRecord final {
        StreamingCellOperationHandle operation; /**< Last reconciled canonical operation and exact attempt fence. */
        StreamingCellState state{};             /**< Current canonical residency fact. */

        /** @brief Checks the complete record representation. @return True for a valid handle and supported state. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] constexpr auto operator<=>(const StreamingCellStateRecord &) const noexcept = default;
    };

    /** @brief Bounded construction facts for one partition authority's residency ledger. */
    struct StreamingCellStateLedgerConfig final {
        /** @brief Hard implementation ceiling for simultaneously tracked current or retiring attempts. */
        static constexpr std::uint32_t MaximumTrackedAttempts = 1024;

        StreamingRuntimeOwnerToken owner;       /**< Exact mounted partition and authority lifetime. */
        std::uint32_t maximumTrackedAttempts{}; /**< Positive bounded attempt/tombstone capacity. */

        /** @brief Checks construction facts. @return True when owner and attempt capacity are valid. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Admission lifecycle for the authority-owned residency ledger. */
    enum class StreamingCellStateLedgerLifecycle : std::uint8_t {
        Active,
        Draining,
        Closed,
    };

    /**
     * @brief Bounded authority that reconciles canonical operations into fenced residency facts.
     * @details Mutating calls are confined to StreamingAuthorityRole. Terminal Unloaded and Failed records retain the last generation
     *          as a bounded tombstone until a fresh generation reuses that cell slot. Retiring generations stay independently tracked.
     */
    class StreamingCellStateLedger final {
    public:
        StreamingCellStateLedger(const StreamingCellStateLedger &) = delete;
        StreamingCellStateLedger &operator=(const StreamingCellStateLedger &) = delete;
        /** @brief Transfers unique authority ownership and closes the moved-from ledger. @param other Ledger to transfer. */
        StreamingCellStateLedger(StreamingCellStateLedger &&other) noexcept;
        StreamingCellStateLedger &operator=(StreamingCellStateLedger &&) = delete;

        /**
         * @brief Creates an active preallocated residency ledger.
         * @param config Exact owner lifetime and mandatory tracked-attempt ceiling.
         * @return Empty ledger or a typed invalid/capacity failure.
         */
        [[nodiscard]] static Result<StreamingCellStateLedger> Create(const StreamingCellStateLedgerConfig &config);

        /**
         * @brief Reconciles one ledger-owned canonical operation snapshot transactionally.
         * @param owner Exact current authority lifetime.
         * @param operation Canonical admitted, progressing, retiring, or terminal operation snapshot.
         * @return Resulting residency record or typed invalid, stale, unsupported, transition, capacity, or lifecycle failure.
         * @post Failure leaves every tracked record unchanged.
         */
        [[nodiscard]] Result<StreamingCellStateRecord> Apply(const StreamingRuntimeOwnerToken &owner,
                                                             const StreamingCellOperation &operation);

        /**
         * @brief Resolves one exact mounted cell-attempt generation.
         * @param owner Exact current authority lifetime.
         * @param fence Exact partition, epoch, cell and generation.
         * @return Owned snapshot, CellStateUnresolved, or a typed invalid/stale failure.
         */
        [[nodiscard]] Result<StreamingCellStateRecord> Resolve(const StreamingRuntimeOwnerToken &owner, const StreamingFence &fence) const;

        /**
         * @brief Closes new loading and publication while retained attempts drain through normal retirement.
         * @param owner Exact current authority lifetime.
         * @return Success, including an idempotent repeat, or a typed invalid/stale failure.
         */
        [[nodiscard]] Result<void> BeginShutdown(const StreamingRuntimeOwnerToken &owner) noexcept;

        /** @brief Returns the exact mounted authority lifetime. @return Immutable owner token. */
        [[nodiscard]] const StreamingRuntimeOwnerToken &Owner() const noexcept;
        /** @brief Returns Active, Draining, or derived Closed after all resource-bearing attempts retire. @return Lifecycle state. */
        [[nodiscard]] StreamingCellStateLedgerLifecycle Lifecycle() const noexcept;
        /** @brief Returns current and retiring attempts plus bounded generation tombstones. @return Tracked record count. */
        [[nodiscard]] std::size_t TrackedAttemptCount() const noexcept;
        /** @brief Borrows actual owner-lane records for immutable diagnostic capture.
         * @return Ledger-owned attempts, including retiring generations; expires on Apply or ledger destruction. */
        [[nodiscard]] std::span<const StreamingCellStateRecord> Records() const noexcept;

    private:
        StreamingCellStateLedger(const StreamingCellStateLedgerConfig &config, std::vector<StreamingCellStateRecord> records) noexcept;

        StreamingCellStateLedgerConfig config_;
        std::vector<StreamingCellStateRecord> records_;
        StreamingCellStateLedgerLifecycle lifecycle_{StreamingCellStateLedgerLifecycle::Active};
    };
}  // namespace Horo::WorldStreaming
