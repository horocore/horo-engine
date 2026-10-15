#pragma once

/** @file IncrementalSceneCellCook.h
 * @brief Owner-thread transactional incremental cooking of immutable Scene cell baselines.
 */
#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Scene/RuntimeSceneCellPayload.h"

namespace Horo::Runtime {
    namespace CellCookDetail {
        struct CacheState;
        struct PreparedState;
    }  // namespace CellCookDetail

    /** @brief Exact captured dependency artifact and its direct dependency identities; host owns artifact verification. */
    struct SceneCellCookDependency final {
        Assets::AssetId id;
        Sha256Digest content;
        std::uint64_t revision{}; /**< Positive publication revision. */
        std::span<const Assets::AssetId> dependencies;
    };

    /** @brief Mandatory aggregate ceilings, including dependency graph traversal. */
    struct SceneCellCookCacheLimits final {
        std::size_t maximumCells{};
        std::size_t maximumDependencies{};
        std::size_t maximumDependencyEdges{};
        std::size_t maximumSchemas{};       /**< Per-cell total captured component and behavior capabilities. */
        std::size_t maximumKeyBytes{};      /**< Per-cell canonical key stream ceiling, independent of native storage sizes. */
        std::size_t maximumRetainedBytes{}; /**< Aggregate payload logical bytes retained by the cache. */
    };

    /** @brief One captured cell snapshot and exact expected authored publication. */
    struct SceneCellCookInput final {
        SceneCellPayloadSource source;
        SceneCellPayloadIdentity expected;
    };

    /** @brief Immutable leased output; leases remain valid across cache replacement and shutdown. */
    struct SceneCellCookEntry final {
        Sha256Digest key;
        std::shared_ptr<const RuntimeSceneCellPayload> payload;
    };

    /** @brief Complete canonical cell result and actual cook/reuse counts. */
    struct SceneCellCookReport final {
        std::uint64_t revision{};
        std::size_t cooked{};
        std::size_t reused{};
        std::vector<SceneCellCookEntry> cells;
    };

    /**
     * @brief Detached complete cook with exclusive owner-thread cache admission until adoption or destruction.
     * @details Owns its cache state; destroying the original cache cannot leave a dangling transaction. Destruction
     * before adoption releases the reservation without changing the retained set or revision. No I/O is performed.
     */
    class PreparedSceneCellCook final {
    public:
        PreparedSceneCellCook(const PreparedSceneCellCook &) = delete;
        PreparedSceneCellCook &operator=(const PreparedSceneCellCook &) = delete;
        PreparedSceneCellCook(PreparedSceneCellCook &&) noexcept = default;
        /** @brief Releases any abandoned reservation before taking the other receipt. @param other Owned source receipt.
         * @return This receipt. */
        PreparedSceneCellCook &operator=(PreparedSceneCellCook &&other) noexcept;
        /** @brief Releases an unadopted reservation even when immutable report leases remain alive. */
        ~PreparedSceneCellCook();
        /** @brief Reads the preallocated candidate report. @return Complete report; requires a non-moved receipt. */
        [[nodiscard]] const SceneCellCookReport &Report() const noexcept;
        /** @brief Retains the immutable preallocated report without copying or moving its vector.
         * @return Aliasing owned lease valid across receipt destruction and adoption; requires a non-moved receipt. */
        [[nodiscard]] std::shared_ptr<const SceneCellCookReport> ReportLease() const noexcept;
        /**
         * @brief Seals exact cache freshness immediately before a caller's external publication point.
         * @param cancellation Observer checked before sealing; later cancellation cannot roll back committed external output.
         * @return Success or typed stale, closed or cancelled failure without changing cache output.
         * @details The caller must also validate its captured source authority before sealing. All other cache cooks are
         * refused while this receipt exists. No allocation is needed by successful admission.
         */
        [[nodiscard]] Result<void> Reserve(const CancellationToken &cancellation = {});
        /**
         * @brief Adopts the reserved, preallocated candidate after external publication has committed.
         * @pre Reserve succeeded on this non-moved receipt and adoption has not already occurred.
         * @details Call also for committed-but-not-durable external outcomes. Shutdown after Reserve keeps admission
         * closed and releases retained payload ownership, while still recording the committed revision.
         */
        void Adopt() noexcept;

    private:
        friend class IncrementalSceneCellCook;
        /** @brief Captures the validated detached candidate and its exclusive cache reservation. @param state Owned candidate. */
        explicit PreparedSceneCellCook(std::shared_ptr<CellCookDetail::PreparedState> state) noexcept;
        /** @brief Releases only this receipt's pending cache admission without mutating retained output. */
        void ReleaseReservation() noexcept;
        std::shared_ptr<CellCookDetail::PreparedState> state_;
    };

    /**
     * @brief Explicit tooling/load-time cache owner, confined to one thread with no ambient registration or I/O.
     * @details A successful request atomically replaces the entire retained cell set. Failure, cancellation or allocation
     * exception preserves the previous set and revision. Removed cells retire from the cache; caller leases remain owned.
     * Revision starts at zero and never wraps. No background work is scheduled; shutdown rejects later requests.
     */
    class IncrementalSceneCellCook final {
    public:
        /** @brief Captures mandatory owner ceilings. @param limits Positive ceilings validated by Cook before allocation. */
        explicit IncrementalSceneCellCook(const SceneCellCookCacheLimits &limits) noexcept;
        /** @brief Closes admission and releases owner retention; pending receipts remain safe to destroy. */
        ~IncrementalSceneCellCook();
        IncrementalSceneCellCook(const IncrementalSceneCellCook &) = delete;
        IncrementalSceneCellCook &operator=(const IncrementalSceneCellCook &) = delete;
        /**
         * @brief Reuses exact content hits and invokes CookRuntimeSceneCellPayload only for affected cells.
         * @param partition Current immutable topology authority; every requested cell must be present.
         * @param inputs Complete desired cell set, including unchanged cells; borrowed only until return.
         * @param dependencies Complete captured artifact catalog; missing edges, duplicates and cycles fail closed.
         * @param settings SHA-256 of host-captured byte-affecting target/profile/effective cook settings.
         * @param expectedRevision Exact cache revision captured by this operation; stale attempts cannot replace output.
         * @param payloadLimits Per-cell ceilings, rechecked on both hits and misses.
         * @param cancellation Cooperative observer checked during hashing, traversal, cooking and before replacement.
         * @return Owned report in canonical manifest cell order or typed SceneCellPayloadErrors failure; Closed uses Invalid.
         * @details Key version 1 hashes every typed source field, schema capability, durable identity and the canonical
         * transitive dependency artifact identities/revisions/digests. This is a semantic baseline cache, separate from
         * Assets CacheKeyV1 and HOROCELL container encoding. Dependency artifacts must already be cooked and verified by
         * their Assets owner; this operation cooks affected Scene baselines, never dependencies or external providers.
         * @throws std::bad_alloc on bounded scratch/output allocation failure without changing cache state.
         */
        [[nodiscard]] Result<SceneCellCookReport> Cook(const WorldStreaming::WorldPartitionDescriptor &partition,
                                                       std::span<const SceneCellCookInput> inputs,
                                                       std::span<const SceneCellCookDependency> dependencies, const Sha256Digest &settings,
                                                       std::uint64_t expectedRevision, SceneCellPayloadLimits payloadLimits,
                                                       const CancellationToken &cancellation = {});
        /**
         * @brief Prepares the same canonical cook as Cook without publishing the cache replacement.
         * @param partition Immutable topology authority.
         * @param inputs Complete desired captured cell set.
         * @param dependencies Complete verified artifact catalog.
         * @param settings Exact byte-affecting settings digest.
         * @param expectedRevision Exact cache revision fence.
         * @param payloadLimits Per-cell bounds rechecked for hits and misses.
         * @param cancellation Cooperative cancellation before reservation/publication.
         * @return Detached owned candidate or the same typed cook failure; a pending receipt rejects competing preparation.
         * @throws std::bad_alloc Without changing retained payloads or revision.
         */
        [[nodiscard]] Result<PreparedSceneCellCook> Prepare(const WorldStreaming::WorldPartitionDescriptor &partition,
                                                            std::span<const SceneCellCookInput> inputs,
                                                            std::span<const SceneCellCookDependency> dependencies,
                                                            const Sha256Digest &settings, std::uint64_t expectedRevision,
                                                            SceneCellPayloadLimits payloadLimits,
                                                            const CancellationToken &cancellation = {});
        /** @brief Returns the last successful replacement revision. @return Monotonic owner revision, initially zero. */
        [[nodiscard]] std::uint64_t Revision() const noexcept;
        /** @brief Releases cache ownership and permanently closes admission; outstanding immutable leases remain valid. */
        void Shutdown() noexcept;

    private:
        SceneCellCookCacheLimits limits_;
        bool closed_{};
        std::shared_ptr<CellCookDetail::CacheState> state_;
    };
}  // namespace Horo::Runtime
