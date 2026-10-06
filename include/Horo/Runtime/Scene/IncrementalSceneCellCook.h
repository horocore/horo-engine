#pragma once

/** @file IncrementalSceneCellCook.h
 * @brief Owner-thread transactional incremental cooking of immutable Scene cell baselines.
 */
#include "Horo/Foundation/Sha256.h"
#include "Horo/Runtime/Scene/RuntimeSceneCellPayload.h"

namespace Horo::Runtime {
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
     * @brief Explicit tooling/load-time cache owner, confined to one thread with no ambient registration or I/O.
     * @details A successful request atomically replaces the entire retained cell set. Failure, cancellation or allocation
     * exception preserves the previous set and revision. Removed cells retire from the cache; caller leases remain owned.
     * Revision starts at zero and never wraps. No background work is scheduled; shutdown rejects later requests.
     */
    class IncrementalSceneCellCook final {
    public:
        /** @brief Captures mandatory owner ceilings. @param limits Positive ceilings validated by Cook before allocation. */
        explicit IncrementalSceneCellCook(SceneCellCookCacheLimits limits) noexcept;
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
        /** @brief Returns the last successful replacement revision. @return Monotonic owner revision, initially zero. */
        [[nodiscard]] std::uint64_t Revision() const noexcept;
        /** @brief Releases cache ownership and permanently closes admission; outstanding immutable leases remain valid. */
        void Shutdown() noexcept;

    private:
        SceneCellCookCacheLimits limits_;
        std::uint64_t revision_{};
        bool closed_{};
        std::vector<SceneCellCookEntry> entries_;
    };
}  // namespace Horo::Runtime
