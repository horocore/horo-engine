#pragma once

/**
 * @file WorldPackageChunkAssignment.h
 * @brief Bounded release-chunk requirements and exact partial-content admission for cooked cells.
 */

#include "Horo/Assets/AssetChunkPlan.h"
#include "Horo/WorldStreaming/CookedWorldIndexManifest.h"

namespace Horo::WorldStreaming {
    namespace Detail {
        /** @brief Distinguishes release authority from unrelated runtime owners. */
        struct WorldPackageAssignmentIdTag;
        /** @brief Distinguishes release publication from availability revisions. */
        struct WorldPackageAssignmentRevisionTag;
        /** @brief Distinguishes verified installation publications. */
        struct WorldPackageAvailabilityRevisionTag;
    }  // namespace Detail

    /** @brief Host-owned identity of one world release assignment authority. */
    using WorldPackageAssignmentId =
        Foundation::Detail::NonZeroId64<Detail::WorldPackageAssignmentIdTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Exact non-zero revision of a release assignment. */
    using WorldPackageAssignmentRevision =
        Foundation::Detail::NonZeroId64<Detail::WorldPackageAssignmentRevisionTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Exact non-zero revision of installed/downloadable content facts. */
    using WorldPackageAvailabilityRevision =
        Foundation::Detail::NonZeroId64<Detail::WorldPackageAvailabilityRevisionTag, WorldStreamingErrors::IdentityInvalid>;

    /** @brief Stable authority and publication fence; replacement must advance revision or identity. */
    struct WorldPackageAssignmentBinding final {
        WorldPackageAssignmentId owner{};          /**< Stable host-owned release authority. */
        WorldPackageAssignmentRevision revision{}; /**< Non-wrapping revision changed for every replacement release input. */
        PartitionEpoch epoch{};                    /**< Exact mounted world incarnation. */
        [[nodiscard]] auto operator<=>(const WorldPackageAssignmentBinding &) const noexcept = default;
    };

    /** @brief Host ceilings for complete assignment storage, checked before publication. */
    struct WorldPackageChunkLimits final {
        std::size_t maximumCells{};             /**< Maximum complete cooked cell count. */
        std::size_t maximumChunks{};            /**< Maximum complete release chunk count. */
        std::size_t maximumReleaseAssets{};     /**< Maximum membership records indexed from the release plan. */
        std::size_t maximumTotalRequirements{}; /**< Maximum aggregate flattened cell-to-chunk requirements. */
    };

    /** @brief Exact cooked artifact and flat release-chunk requirement range for one cell. */
    struct WorldCellChunkAssignment final {
        StreamingCellId cell{};
        Assets::AssetId artifact{};
        Sha256Digest artifactHash{};
        Sha256Digest metadataHash{};     /**< Canonical artifact sizes/integrity and direct hard-dependency identity digest. */
        std::size_t requirementOffset{}; /**< First requirement in assignment-owned canonical storage. */
        std::size_t requirementCount{};  /**< Includes base, hard-cell and chunk prerequisites once each. */
    };

    /**
     * @brief Inert immutable mapping; owns no installation, download, scheduling or runtime lifecycle.
     * @details Construction is cook/load-time work. Views expire when this value is moved from or destroyed.
     */
    class WorldPackageChunkAssignment final {
    public:
        WorldPackageChunkAssignment(const WorldPackageChunkAssignment &) = delete;
        WorldPackageChunkAssignment &operator=(const WorldPackageChunkAssignment &) = delete;
        WorldPackageChunkAssignment(WorldPackageChunkAssignment &&) noexcept = default;
        WorldPackageChunkAssignment &operator=(WorldPackageChunkAssignment &&) = delete;

        /**
         * @brief Copies exact cell-to-chunk membership and transitive hard-cell/chunk dependency closure.
         * @param manifest Sole cooked-cell artifact authority; borrowed only during construction.
         * @param plan Validated Assets release-chunk membership authority; borrowed only during construction.
         * @param binding Host-owned exact release publication fence.
         * @param baseManifest Verified non-zero base-release manifest digest used by Assets compatibility validation.
         * @param limits Mandatory finite cell, chunk, release-asset and flat requirement ceilings.
         * @return Complete canonical assignment or typed failure without consuming or modifying inputs.
         * @throws std::bad_alloc if private construction storage cannot be allocated; no result is published.
         */
        [[nodiscard]] static Result<WorldPackageChunkAssignment> Create(const CookedWorldIndexManifest &manifest,
                                                                        const Assets::AssetChunkPlan &plan,
                                                                        WorldPackageAssignmentBinding binding,
                                                                        const Sha256Digest &baseManifest,
                                                                        const WorldPackageChunkLimits &limits);

        /** @brief Returns exact publication fence. @return Immutable binding. */
        [[nodiscard]] WorldPackageAssignmentBinding Binding() const noexcept {
            return binding_;
        }

        /** @brief Returns partition identity. @return Stable partition. */
        [[nodiscard]] WorldPartitionId Partition() const noexcept {
            return partition_;
        }

        /** @brief Returns canonical complete release chunk identities. @return Borrowed immutable IDs. */
        [[nodiscard]] std::span<const Assets::AssetChunkId> Chunks() const noexcept {
            return chunks_;
        }

        /** @brief Returns canonical cell records. @return Borrowed immutable assignments. */
        [[nodiscard]] std::span<const WorldCellChunkAssignment> Cells() const noexcept {
            return cells_;
        }

        /**
         * @brief Checks the requested cell and transitive hard-dependency metadata against the current manifest.
         * @param manifest Sole current cell authority, borrowed only for this bounded admission check.
         * @param cell Exact requested cell identity.
         * @return Success or typed stale/unassigned error; no reads or mutations are performed.
         * @details Visits only the co-load component, with private storage bounded by admitted cell count.
         */
        [[nodiscard]] Result<void> ValidateCell(const CookedWorldIndexManifest &manifest, StreamingCellId cell) const;

        /** @brief Returns one cell's unique canonical requirements. @param index Cell index. @return Empty when out of bounds. */
        [[nodiscard]] std::span<const Assets::AssetChunkId> Requirements(std::size_t index) const noexcept;

    private:
        /** @brief Stores already validated complete canonical release requirements. */
        WorldPackageChunkAssignment(WorldPartitionId partition, WorldPackageAssignmentBinding binding,
                                    std::vector<Assets::AssetChunkId> chunks, std::vector<WorldCellChunkAssignment> cells,
                                    std::vector<Assets::AssetChunkId> requirements) noexcept;
        WorldPartitionId partition_{};
        WorldPackageAssignmentBinding binding_{};
        std::vector<Assets::AssetChunkId> chunks_;
        std::vector<WorldCellChunkAssignment> cells_;
        std::vector<Assets::AssetChunkId> requirements_;
    };

    /** @brief Host-supplied verified content state; downloading or failed content is never readable. */
    enum class WorldPackageChunkState : std::uint8_t {
        Installed,
        Installable,
        Downloadable,
        Installing,
        Unavailable,
        Failed,
        Count
    };

    /** @brief Exact state of one declared release chunk. Installed means verified and mounted by Assets. */
    struct WorldPackageChunkAvailability final {
        Assets::AssetChunkId chunk;
        WorldPackageChunkState state{WorldPackageChunkState::Unavailable};
    };
    /** @brief New-read admission closes explicitly during cancellation and shutdown. */
    enum class WorldPackageContentLifecycle : std::uint8_t {
        Active,
        Cancelling,
        Closed,
        Count
    };

    /** @brief Complete owned immutable content facts for one exact release assignment. */
    class WorldPackageAvailabilitySnapshot final {
    public:
        WorldPackageAvailabilitySnapshot(const WorldPackageAvailabilitySnapshot &) = delete;
        WorldPackageAvailabilitySnapshot &operator=(const WorldPackageAvailabilitySnapshot &) = delete;
        WorldPackageAvailabilitySnapshot(WorldPackageAvailabilitySnapshot &&) noexcept = default;
        WorldPackageAvailabilitySnapshot &operator=(WorldPackageAvailabilitySnapshot &&) = delete;

        /**
         * @brief Validates and copies exactly one state for every assignment chunk.
         * @param assignment Exact release authority; borrowed only during construction.
         * @param revision Host publication revision, advanced when availability changes.
         * @param chunks Complete chunk states in any order; no caller storage retained.
         * @return Canonical owned snapshot or typed invalid/unsupported failure; no partial state escapes.
         */
        [[nodiscard]] static Result<WorldPackageAvailabilitySnapshot> Create(const WorldPackageChunkAssignment &assignment,
                                                                             WorldPackageAvailabilityRevision revision,
                                                                             std::span<const WorldPackageChunkAvailability> chunks);

        /** @brief Returns exact partition identity. @return Stable partition. */
        [[nodiscard]] WorldPartitionId Partition() const noexcept {
            return partition_;
        }

        /** @brief Returns release fence captured by this publication. @return Exact immutable binding. */
        [[nodiscard]] WorldPackageAssignmentBinding Binding() const noexcept {
            return binding_;
        }

        /** @brief Returns availability publication revision. @return Non-zero revision. */
        [[nodiscard]] WorldPackageAvailabilityRevision Revision() const noexcept {
            return revision_;
        }

        /** @brief Returns canonical owned states. @return Borrowed immutable state rows. */
        [[nodiscard]] std::span<const WorldPackageChunkAvailability> Chunks() const noexcept {
            return chunks_;
        }

    private:
        /** @brief Stores one validated complete canonical content publication. */
        WorldPackageAvailabilitySnapshot(WorldPartitionId partition, WorldPackageAssignmentBinding binding,
                                         WorldPackageAvailabilityRevision revision,
                                         std::vector<WorldPackageChunkAvailability> chunks) noexcept;
        WorldPartitionId partition_{};
        WorldPackageAssignmentBinding binding_{};
        WorldPackageAvailabilityRevision revision_{};
        std::vector<WorldPackageChunkAvailability> chunks_;
    };

    /** @brief Current host facts checked before planning or admitting reads; no ambient state is consulted. */
    struct WorldPackageContentContext final {
        WorldPackageAssignmentBinding assignment{};
        WorldPackageAvailabilityRevision availability{};
        WorldPackageContentLifecycle lifecycle{WorldPackageContentLifecycle::Closed};
    };

    /** @brief Complete normal partial-availability result with exact missing identities and causes. */
    struct WorldCellContentAvailability final {
        WorldPackageAssignmentBinding assignment{};
        WorldPackageAvailabilityRevision availability{};
        StreamingCellId cell{};
        std::vector<WorldPackageChunkAvailability> missing;

        /** @brief Reports whether every required chunk is verified and mounted. @return True only for complete content. */
        [[nodiscard]] bool IsAvailable() const noexcept {
            return missing.empty();
        }
    };

    /**
     * @brief Evaluates exact cell content without scheduling downloads, reads, or mutating installed state.
     * @param assignment Immutable cooked-cell requirements.
     * @param snapshot Complete immutable content facts.
     * @param cell Exact declared cell identity.
     * @param context Current assignment/revision/lifecycle supplied by the host.
     * @return Complete missing-content rows (a normal value), or typed invalid/stale/unsupported/lifecycle failure.
     * @details Retained snapshots remain readable after replacement; admission with superseded host context fails.
     */
    [[nodiscard]] Result<WorldCellContentAvailability> EvaluateWorldCellContent(const WorldPackageChunkAssignment &assignment,
                                                                                const WorldPackageAvailabilitySnapshot &snapshot,
                                                                                StreamingCellId cell,
                                                                                const WorldPackageContentContext &context);
}  // namespace Horo::WorldStreaming
