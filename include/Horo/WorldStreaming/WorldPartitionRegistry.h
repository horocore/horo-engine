#pragma once

/**
 * @file WorldPartitionRegistry.h
 * @brief Generation-pinned immutable partition registry snapshots and bounded queries.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/StrongId.h"
#include "Horo/WorldStreaming/WorldPartitionDescriptor.h"
#include "Horo/WorldStreaming/WorldStreamingRuntimeComposition.h"

#include <array>
#include <atomic>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace Horo::WorldStreaming {
    namespace Detail {
        struct WorldPartitionRegistryIdTag;
        struct WorldPartitionRegistryRevisionTag;
    }  // namespace Detail

    /** @brief Stable host-issued identity of one partition-registry lifetime. */
    using WorldPartitionRegistryId =
        Foundation::Detail::NonZeroId64<Detail::WorldPartitionRegistryIdTag, WorldStreamingErrors::IdentityInvalid>;
    /** @brief Monotonic immutable publication generation of one partition registry. */
    using WorldPartitionRegistryRevision =
        Foundation::Detail::NonZeroId64<Detail::WorldPartitionRegistryRevisionTag, WorldStreamingErrors::IdentityInvalid>;

    /** @brief Exact publication fence captured by every lookup and spatial-query result. */
    struct WorldPartitionRegistryBinding final {
        WorldPartitionRegistryId registry{};       /**< Stable registry lifetime. */
        WorldPartitionRegistryRevision revision{}; /**< Exact immutable publication. */
        StreamingRuntimeOwnerToken owner{};        /**< Exact mounted partition incarnation. */

        /** @brief Checks all identity and owner components. @return True when structurally usable. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] constexpr auto operator<=>(const WorldPartitionRegistryBinding &) const noexcept = default;
    };

    /** @brief Generation-pinned dense handle into exactly one immutable registry snapshot. */
    struct WorldPartitionCellHandle final {
        WorldPartitionRegistryBinding binding{}; /**< Exact registry publication. */
        std::uint32_t slot{};                    /**< Dense canonical cell slot. */
        StreamingCellId cell{};                  /**< Stable manifest cell identity. */

        /** @brief Checks representation, not snapshot membership. @return True when the fence and cell are usable. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] constexpr auto operator<=>(const WorldPartitionCellHandle &) const noexcept = default;
    };

    /** @brief Mandatory storage and query ceilings for a partition registry. */
    struct WorldPartitionRegistryLimits final {
        static constexpr std::uint32_t MaximumCells = 1'048'576;
        static constexpr std::uint32_t MaximumQueryResults = 65'536;

        std::uint32_t cells{};        /**< Maximum cells copied into one publication. */
        std::uint32_t queryResults{}; /**< Maximum handles emitted by one query. */

        /** @brief Checks positive implementation-bounded ceilings. @return True when both limits are usable. */
        [[nodiscard]] bool IsValid() const noexcept;
    };

    /** @brief Exact inclusive spatial query with optional manifest layer and LOD filters. */
    struct WorldPartitionSpatialQuery final {
        WorldPartitionBounds bounds{};           /**< Inclusive canonical-millimeter query bounds. */
        std::optional<StreamingLayerId> layer{}; /**< Exact layer filter, or every declared layer. */
        std::optional<std::uint8_t> lod{};       /**< Exact LOD filter, or every declared LOD. */
    };

    /** @brief Bounded spatial-index query evidence for one immutable registry publication. */
    struct WorldPartitionSpatialQueryResult final {
        WorldPartitionRegistryBinding binding{}; /**< Exact immutable publication evaluated by the query. */
        std::size_t matches{};                   /**< Canonically ordered handles written to caller storage. */
        std::size_t candidatesExamined{};        /**< Exact cell candidates tested after index pruning. */
        std::size_t nodesVisited{};              /**< Immutable spatial-index nodes visited. */
    };

    /** @brief Fixed-storage continuation issued by one immutable spatial index; copies advance independently. */
    class WorldPartitionSpatialCursor final {
    public:
        /** @brief Reports terminal traversal. @return True after every intersecting branch was visited. */
        [[nodiscard]] bool Complete() const noexcept {
            return complete_;
        }

    private:
        friend class WorldPartitionRegistrySnapshot;
        WorldPartitionRegistryBinding binding_{};
        WorldPartitionSpatialQuery query_{};
        std::array<std::uint32_t, 64> pending_{};
        std::uint32_t pendingCount_{};
        std::uint32_t leafOffset_{};
        bool complete_{};
    };

    /** @brief Per-call spatial work ceilings; neither exhausted work nor a full output is a failure. */
    struct WorldPartitionSpatialWorkBudget final {
        std::uint32_t nodes{};      /**< Maximum BVH nodes visited, including rejected branches. */
        std::uint32_t candidates{}; /**< Maximum leaf candidates examined, including filtered cells. */
    };

    /** @brief One resumable spatial batch; handles are deterministic BVH order, not canonical cell order. */
    struct WorldPartitionSpatialBatch final {
        std::size_t matches{};
        std::size_t nodesVisited{};
        std::size_t candidatesExamined{};
        bool complete{};
    };

    /** @brief Lifecycle gate for registry publication and new snapshot capture. */
    enum class WorldPartitionRegistryState : std::uint8_t {
        Active,
        Cancelling,
        Closed,
    };

    class WorldPartitionRegistry;

    /** @brief Owning immutable partition index safe to retain across replacement and registry shutdown. */
    class WorldPartitionRegistrySnapshot final {
    public:
        struct State;
        WorldPartitionRegistrySnapshot() = default;

        /** @brief Reports whether this value pins a publication. @return True for a registry-issued snapshot. */
        [[nodiscard]] bool IsValid() const noexcept;
        /** @brief Returns the exact registry, revision, partition and epoch fence. @return Invalid binding for an empty snapshot. */
        [[nodiscard]] const WorldPartitionRegistryBinding &Binding() const noexcept;
        /** @brief Returns canonical cells owned by this snapshot. @return Immutable span valid for this snapshot's lifetime. */
        [[nodiscard]] std::span<const WorldPartitionCellDescriptor> Cells() const noexcept;

        /**
         * @brief Looks up one exact manifest cell in logarithmic time without allocation.
         * @param cell Stable cell identity.
         * @return Generation-pinned handle or a typed invalid/unavailable failure.
         */
        [[nodiscard]] Result<WorldPartitionCellHandle> Find(StreamingCellId cell) const;

        /**
         * @brief Resolves one handle only against the publication that issued it.
         * @param handle Exact registry-publication handle.
         * @return Borrowed immutable cell record or a typed invalid/stale failure.
         */
        [[nodiscard]] Result<const WorldPartitionCellDescriptor *> Resolve(const WorldPartitionCellHandle &handle) const;

        /**
         * @brief Executes a bounded allocation-free intersection query in canonical cell order.
         * @param query Ordered exact bounds and optional layer/LOD filters.
         * @param output Caller-owned handle storage; no element is modified on failure.
         * @return Publication-fenced written-count and bounded index-work evidence, or a typed failure.
         */
        [[nodiscard]] Result<WorldPartitionSpatialQueryResult> Query(const WorldPartitionSpatialQuery &query,
                                                                     std::span<WorldPartitionCellHandle> output) const;

        /** @brief Returns the immutable grid or null for an empty snapshot. @return Snapshot-owned grid. */
        [[nodiscard]] const WorldCellQuantizationPolicy *Grid() const noexcept;
        /** @brief Returns immutable canonical layer metadata. @return Snapshot-owned layers. */
        [[nodiscard]] std::span<const WorldLayerDescriptor> Layers() const noexcept;
        /**
         * @brief Starts a fixed-storage resumable query without traversing the index.
         * @param query Ordered inclusive bounds and declared filters.
         * @return Exact-publication continuation or typed invalid/unsupported failure.
         */
        [[nodiscard]] Result<WorldPartitionSpatialCursor> BeginQuery(const WorldPartitionSpatialQuery &query) const;
        /**
         * @brief Advances a query under explicit independent node and candidate ceilings without allocation.
         * @param cursor Continuation issued by this exact publication; advanced only on success.
         * @param budget Positive node and candidate work ceilings.
         * @param output Nonempty caller storage; matching handles occupy the returned prefix.
         * @return Work evidence and completion, or typed invalid/stale failure with cursor/output unchanged.
         * @details Output order is deterministic BVH traversal order. Canonical consumers must reduce/sort their bounded staging rows.
         */
        [[nodiscard]] Result<WorldPartitionSpatialBatch> QueryNext(WorldPartitionSpatialCursor &cursor,
                                                                   WorldPartitionSpatialWorkBudget budget,
                                                                   std::span<WorldPartitionCellHandle> output) const;

    private:
        friend class WorldPartitionRegistry;

        explicit WorldPartitionRegistrySnapshot(std::shared_ptr<const State> state) noexcept : state_(std::move(state)) {}

        std::shared_ptr<const State> state_;
    };

    /** @brief Explicit owner-thread publisher of immutable partition registry snapshots. */
    class WorldPartitionRegistry final {
    public:
        WorldPartitionRegistry(const WorldPartitionRegistry &) = delete;
        WorldPartitionRegistry &operator=(const WorldPartitionRegistry &) = delete;

        /** @brief Opaque construction gate restricted to validated registry creation. */
        class ConstructionKey final {
            friend class WorldPartitionRegistry;
            ConstructionKey() = default;
        };

        /** @brief Adopts validated construction facts through the creation-only gate. */
        WorldPartitionRegistry(ConstructionKey, WorldPartitionRegistryId registry, const StreamingRuntimeOwnerToken &owner,
                               WorldPartitionRegistryLimits limits) noexcept;

        /**
         * @brief Creates an empty active registry without discovering or mounting a partition.
         * @param registry Stable host-issued registry lifetime.
         * @param owner Exact mounted partition authority.
         * @param limits Mandatory publication and query ceilings.
         * @return Unique registry owner or a typed invalid/storage failure.
         */
        [[nodiscard]] static Result<std::unique_ptr<WorldPartitionRegistry>> Create(WorldPartitionRegistryId registry,
                                                                                    const StreamingRuntimeOwnerToken &owner,
                                                                                    WorldPartitionRegistryLimits limits);

        /**
         * @brief Transactionally publishes a complete descriptor as the exact next immutable revision.
         * @param descriptor Validated descriptor moved from only by successful publication.
         * @param revision Revision one for the first publication, then the exact non-wrapping successor.
         * @return Success or a typed invalid/stale/unsupported/capacity/lifecycle/storage failure.
         * @pre Called by the single registry composition owner; concurrent publication is forbidden.
         * @post Failure preserves the previous publication and all previously issued snapshots.
         */
        [[nodiscard]] Result<void> Publish(WorldPartitionDescriptor &&descriptor, WorldPartitionRegistryRevision revision);

        /**
         * @brief Pins the current immutable publication for concurrent query consumers.
         * @return Owning snapshot or a typed unavailable/lifecycle failure.
         */
        [[nodiscard]] Result<WorldPartitionRegistrySnapshot> Snapshot() const;

        /** @brief Idempotently closes publication and new snapshot admission while retained snapshots remain valid. */
        void BeginCancellation() noexcept;
        /** @brief Idempotently closes the registry and releases its reference to the current publication. */
        void Shutdown() noexcept;
        /** @brief Calls Shutdown without invalidating independently retained snapshots. */
        ~WorldPartitionRegistry() noexcept;

        /** @brief Returns the current lifecycle state. @return Active, cancelling, or closed. */
        [[nodiscard]] WorldPartitionRegistryState Lifecycle() const noexcept;

    private:
        WorldPartitionRegistryId registry_{};
        StreamingRuntimeOwnerToken owner_{};
        WorldPartitionRegistryLimits limits_{};
        std::shared_ptr<const WorldPartitionRegistrySnapshot::State> state_;
        std::atomic<WorldPartitionRegistryState> lifecycle_{WorldPartitionRegistryState::Active};
    };

    /** @brief Advances a registry revision without wrapping. @param current Current revision. @return Successor or exhaustion. */
    [[nodiscard]] Result<WorldPartitionRegistryRevision> NextWorldPartitionRegistryRevision(WorldPartitionRegistryRevision current);
}  // namespace Horo::WorldStreaming
