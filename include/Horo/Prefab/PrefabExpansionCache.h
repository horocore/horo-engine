#pragma once

/** @file PrefabExpansionCache.h
 * @brief Bounded owner-thread memoization of immutable, complete source expansion.
 */
#include "Horo/Foundation/CancellationToken.h"
#include "Horo/Prefab/PrefabSourceResolver.h"

#include <memory>
#include <thread>

namespace Horo::SceneSource {
    class ScenePrefabExpansionOwner;
}

namespace Horo::Prefab {
    /** @brief Hard admission ceilings for one disposable authoring cache, not a cooked artifact cache. */
    struct PrefabExpansionCacheLimits final {
        std::size_t maximumEntries{8};
        std::size_t maximumRetainedBytes{64U * 1024U * 1024U};
        std::size_t maximumKeySourceBytes{32U * 1024U * 1024U};
    };

    /** @brief Immutable request identity, constructed only from exact resolver-owned semantic inputs. */
    class PrefabExpansionCacheKey final {
    public:
        /** @brief Returns exact publication evidence. @return Borrowed immutable reachable revisions. */
        [[nodiscard]] const PrefabResolutionRevision &Revision() const noexcept {
            return revision_;
        }

        [[nodiscard]] bool operator==(const PrefabExpansionCacheKey &) const noexcept = default;

    private:
        friend class PrefabExpansionCache;

        PrefabExpansionCacheKey(Assets::AssetId root, PrefabInstanceId instance, PrefabResolutionRevision revision,
                                PrefabProjectPolicy policy, std::vector<Sha256Digest> sources)
            : root_(root), instance_(instance), revision_(std::move(revision)), policy_(policy), sources_(std::move(sources)) {}

        Assets::AssetId root_;
        PrefabInstanceId instance_;
        PrefabResolutionRevision revision_;
        PrefabProjectPolicy policy_;
        std::vector<Sha256Digest> sources_; /**< Actual canonical source commitments; claimed revisions alone are insufficient. */
    };

    /**
     * @brief Owner-thread cache whose returned leases pin immutable results independently of eviction.
     * @details No source I/O, runtime activation, mutable document borrow, callbacks or worker ownership.
     * Every registry publication conservatively changes the key, including ordinary resource edits.
     * Limits bound cache-owned storage; caller-held retired leases have separate caller ownership.
     */
    class PrefabExpansionCache final {
    public:
        /** @brief Captures finite cache policy. @param limits Host-selected ceilings, validated at operation admission. */
        explicit PrefabExpansionCache(PrefabExpansionCacheLimits limits = {});
        PrefabExpansionCache(const PrefabExpansionCache &) = delete;
        PrefabExpansionCache &operator=(const PrefabExpansionCache &) = delete;
        PrefabExpansionCache(PrefabExpansionCache &&) = delete;
        PrefabExpansionCache &operator=(PrefabExpansionCache &&) = delete;

        /**
         * @brief Captures complete bounded key evidence without expanding a hierarchy.
         * @param resolver Owned immutable source publication. @param root Requested root. @param instance Occurrence identity.
         * @param limits Captured expansion settings. @param cancellation Cooperative request token.
         * @return Complete key or typed source, cancellation, budget or cache-policy failure.
         */
        [[nodiscard]] Result<PrefabExpansionCacheKey> CaptureKey(const PrefabSourceResolverSnapshot &resolver, Assets::AssetId root,
                                                                 PrefabInstanceId instance, const PrefabLimitProfile &limits,
                                                                 const CancellationToken &cancellation = {}) const;
        /** @brief Finds exact complete evidence on the owner thread. @param key Captured identity. @return Lease or empty on miss. */
        [[nodiscard]] std::shared_ptr<const EffectivePrefabCandidate> Find(const PrefabExpansionCacheKey &key) const;

    private:
        friend class Horo::SceneSource::ScenePrefabExpansionOwner;
        /**
         * @brief Admits only a complete matching immutable result; eviction never changes existing leases.
         * @param key Exact captured input. @param candidate Complete resolver output owned by the caller.
         * @return Immutable lease or typed mismatch/capacity/thread error. Failure preserves all cache entries.
         */
        [[nodiscard]] Result<std::shared_ptr<const EffectivePrefabCandidate>> Store(PrefabExpansionCacheKey key,
                                                                                    EffectivePrefabCandidate candidate);

    public:
        /**
         * @brief Uses exact memoization or the ordinary pure resolver, never a second expansion authority.
         * @param resolver Immutable source publication. @param root Root identity. @param instance Stable occurrence.
         * @param limits Complete project settings. @param cancellation Cooperative cancellation checked before cache publication.
         * @return Immutable equivalent-to-fresh candidate or typed failure; failed/cancelled results are not cached.
         * @details Cancellation is checked immediately before returning a hit and before cache insertion.
         * A later cancellation cannot revoke an already returned immutable lease; the consuming owner rechecks at publication.
         */
        [[nodiscard]] Result<std::shared_ptr<const EffectivePrefabCandidate>> Resolve(const PrefabSourceResolverSnapshot &resolver,
                                                                                      Assets::AssetId root, PrefabInstanceId instance,
                                                                                      const PrefabLimitProfile &limits,
                                                                                      const CancellationToken &cancellation = {});
        /** @brief Releases cache-owned leases on the owner thread. @return Success or typed wrong-thread error. */
        [[nodiscard]] Result<void> Clear();

        /** @brief Returns reserved cache-owned logical bytes, excluding allocator overhead and caller leases.
         * @pre Called only on the construction thread; this cache introduces no cross-thread query synchronization. */
        [[nodiscard]] std::size_t RetainedBytes() const noexcept {
            return retainedBytes_;
        }

    private:
        struct Entry final {
            PrefabExpansionCacheKey key;
            std::shared_ptr<const EffectivePrefabCandidate> candidate;
            std::size_t bytes{};
        };

        [[nodiscard]] bool IsOwner() const noexcept;
        /** @brief Measures full key/candidate ownership before any cache mutation. */
        [[nodiscard]] Result<std::size_t> MeasureEntry(const PrefabExpansionCacheKey &key, const EffectivePrefabCandidate &candidate) const;
        /** @brief Checks exact complete candidate identity before memoization. */
        [[nodiscard]] bool Matches(const PrefabExpansionCacheKey &key, const EffectivePrefabCandidate &candidate) const;
        [[nodiscard]] Result<PrefabExpansionCacheKey> CaptureKeyImpl(const PrefabSourceResolverSnapshot &resolver, Assets::AssetId root,
                                                                     PrefabInstanceId instance, const PrefabLimitProfile &limits,
                                                                     const CancellationToken &cancellation) const;
        [[nodiscard]] Result<std::shared_ptr<const EffectivePrefabCandidate>> StoreImpl(PrefabExpansionCacheKey key,
                                                                                        EffectivePrefabCandidate candidate);
        PrefabExpansionCacheLimits limits_;
        const std::thread::id owner_;
        std::vector<Entry> entries_;
        std::size_t retainedBytes_{};
    };
}  // namespace Horo::Prefab
