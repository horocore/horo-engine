#pragma once

/** @file EnvironmentQueryCache.h
 * @brief Bounded Scene-owner reuse of complete adaptive tactical query results.
 */

#include "Horo/AI/EnvironmentQueryExecution.h"
#include "Horo/Foundation/Sha256.h"

namespace Horo::AI {
    /** @brief Exact version of one global or spatial dependency in the complete read footprint. */
    struct QueryCacheDependency final {
        QueryProviderId provider;
        std::uint64_t incarnation{};
        std::uint64_t dependency{};
        std::uint64_t scope{}; /**< Zero denotes a conservative global read. */
        std::uint64_t revision{};
        auto operator<=>(const QueryCacheDependency &) const = default;
    };

    /** @brief Borrowed semantic key supplied by trusted host composition. */
    struct QueryCacheKey final {
        QueryId query;
        Sha256Digest artifact;
        std::span<const std::byte>
            canonicalInputs; /**< Exact parameters, versions, seeds and effective policies not captured as contexts. */
        std::span<const QueryCacheDependency> dependencies; /**< Strictly ordered complete dependency set. */
        bool completeFootprint{};                           /**< True only if every dependency used by the query is represented. */
        bool sharedPointData{}; /**< Opt-in for requester-independent point data. The default isolates exact agents. */
    };

    /** @brief Explicit Scene cache allocation and eviction limits within the host's EQS budget. */
    struct QueryResultCacheLimits final {
        std::size_t maximumEntries{};
        std::size_t maximumBytes{};
        std::uint64_t lifetimeTicks{};
    };

    /** @brief Bounded owner-thread cache. No Scene, provider, or requester borrow is retained.
     * @details The host stores complete adaptive results after publication and attempts reuse after fresh input capture.
     * Caller-owned snapshots are historical and survive eviction. The host accounts for those copies separately.
     */
    class QueryResultCache final {
    public:
        /** @brief Creates the empty Scene cache with validated positive limits. @return Cache or typed limit failure. */
        [[nodiscard]] static Result<QueryResultCache> Create(Runtime::SceneRuntimeId scene, QueryResultCacheLimits limits);
        QueryResultCache(const QueryResultCache &) = delete;
        QueryResultCache &operator=(const QueryResultCache &) = delete;
        QueryResultCache(QueryResultCache &&other) noexcept;
        QueryResultCache &operator=(QueryResultCache &&) = delete;

        /** @brief Copies a complete current result after admission and evicts oldest entries as needed.
         * @param scene Fresh matching Scene view. @param inputs Exact request owner/context.
         * @param source Live authorization for the captured inputs. @param key Complete host-owned cache identity.
         * @param result Just-published adaptive result. @param tick Current simulation tick.
         * @return True if retained, false if uncacheable, or a typed storage limit failure.
         */
        [[nodiscard]] Result<bool> Store(const Runtime::RuntimeSceneView &scene, const QueryExecutionInputs &inputs,
                                         const IQueryExecutionSource &source, const QueryCacheKey &key, const QueryExecutionResult &result,
                                         std::uint64_t tick);

        /** @brief Reads a matching unexpired result after revalidating authorization and entity generations.
         * @param scene Fresh matching Scene view. @param inputs Exact current request owner/context.
         * @param source Live authorization for the captured inputs. @param key Complete host-owned cache identity.
         * @param tick Current simulation tick. @return Retained immutable result or null on miss.
         */
        [[nodiscard]] std::shared_ptr<const QueryExecutionResult> Lookup(const Runtime::RuntimeSceneView &scene,
                                                                         const QueryExecutionInputs &inputs,
                                                                         const IQueryExecutionSource &source, const QueryCacheKey &key,
                                                                         std::uint64_t tick) const;
        /** @brief Drops cache ownership on Scene retirement or explicit invalidation. */
        void Clear() noexcept;

    private:
        struct Entry final {
            QueryId query;
            Sha256Digest artifact;
            AgentHandle privateOwner;
            std::vector<QueryCapturedContext> contexts;
            std::vector<std::byte> canonicalInputs;
            std::vector<QueryCacheDependency> dependencies;
            std::shared_ptr<const QueryExecutionResult> result;
            std::size_t charge{};
        };

        QueryResultCache(Runtime::SceneRuntimeId scene, QueryResultCacheLimits limits);
        [[nodiscard]] bool Expired(const Entry &entry, std::uint64_t tick) const noexcept;
        [[nodiscard]] static bool Matches(const Entry &entry, const QueryCacheKey &key, const QueryExecutionInputs &inputs);
        Runtime::SceneRuntimeId scene_;
        QueryResultCacheLimits limits_;
        std::vector<Entry> entries_;
        std::size_t bytes_{};
    };
}  // namespace Horo::AI
