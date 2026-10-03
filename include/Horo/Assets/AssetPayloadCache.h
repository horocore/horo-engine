#pragma once

/** @file AssetPayloadCache.h
 * @brief Bounded AssetPipeline ownership of shared immutable cooked payload bytes. */

#include "Horo/Foundation/Result.h"
#include "Horo/Foundation/Sha256.h"

#include <memory>
#include <span>
#include <vector>

namespace Horo::Assets {
    namespace Detail {
        struct AssetPayloadAllocation;
        struct AssetPayloadAccounting;
    }  // namespace Detail

    /** @brief Copyable immutable allocation pin; safe to retain after eviction or cache destruction. */
    class AssetPayloadLease final {
    public:
        /** @brief Construct an empty pin. */
        AssetPayloadLease() noexcept = default;
        /** @brief Read retained bytes. @return Empty for an empty lease; otherwise a borrow bounded by this lease. */
        [[nodiscard]] std::span<const std::byte> Bytes() const noexcept;
        /** @brief Read canonical content identity. @return Zero for an empty lease. */
        [[nodiscard]] Sha256Digest Digest() const noexcept;
        /** @brief Compare actual allocation ownership. @param other Other pin. @return True for the same allocation. */
        [[nodiscard]] bool SharesAllocationWith(const AssetPayloadLease &other) const noexcept;

    private:
        friend class AssetPayloadCache;
        explicit AssetPayloadLease(std::shared_ptr<const Detail::AssetPayloadAllocation> allocation) noexcept;
        std::shared_ptr<const Detail::AssetPayloadAllocation> allocation_;
    };

    /** @brief Memory evidence separates cache pins from physically live byte storage. */
    struct AssetPayloadCacheSnapshot final {
        std::size_t residentPayloadBytes{}; /**< Vector capacities held by cache pins. */
        std::size_t retainedPayloadBytes{}; /**< Capacities held by any pin, including evicted active queries. */
        std::size_t bookkeepingBytes{};     /**< Fixed index capacity; excludes allocator/control-block overhead. */
        std::size_t residentEntries{};
    };

    /**
     * @brief Owner-thread immutable byte cache; canonical SHA-256 identity is computed from admitted bytes.
     * @details Admission, eviction and snapshots run on one host owner thread. Immutable leases may cross
     * workers; an atomic retained-byte counter protects only final allocation release. Eviction does not
     * refund retained bytes until the last lease dies. This owns no AssetId catalog, cook key, provider,
     * filesystem or world lifecycle. Native provider allocations remain separately owned/accounted.
     */
    class AssetPayloadCache final {
    public:
        AssetPayloadCache(const AssetPayloadCache &) = delete;
        AssetPayloadCache &operator=(const AssetPayloadCache &) = delete;
        AssetPayloadCache(AssetPayloadCache &&) = delete;
        AssetPayloadCache &operator=(AssetPayloadCache &&) = delete;
        /** @brief Construct a bounded empty index. @param maximumEntries Positive live allocation bound.
         * @param maximumPayloadBytes Positive retained byte capacity budget.
         * @return Cache or typed invalid limit error. */
        [[nodiscard]] static Result<std::unique_ptr<AssetPayloadCache>> Create(std::size_t maximumEntries, std::size_t maximumPayloadBytes);
        /** @brief Retain exact bytes, deduplicating even when an existing allocation was evicted.
         * @param bytes Nonempty immutable cooked bytes. @return Pin or typed capacity/shutdown failure.
         * @throws std::bad_alloc When bounded allocation fails. */
        [[nodiscard]] Result<AssetPayloadLease> Admit(std::span<const std::byte> bytes);
        /** @brief Remove the cache pin for exact content. @param digest Canonical digest; missing is a no-op. */
        void Evict(const Sha256Digest &digest) noexcept;
        /** @brief Close admission and release every cache pin without waiting for workers. */
        void Shutdown() noexcept;
        /** @brief Obtain current storage evidence. @return Retained/resident payload and fixed bookkeeping bytes. */
        [[nodiscard]] AssetPayloadCacheSnapshot Snapshot() const noexcept;

        /** @brief Key required by the checked factory; callers cannot construct it. */
        class ConstructionKey final {
            friend class AssetPayloadCache;
            ConstructionKey() = default;
        };

        /** @brief Factory-only constructor. @param key Checked factory key. @param maximumEntries Index capacity.
         * @param maximumPayloadBytes Retained-byte budget. */
        AssetPayloadCache(ConstructionKey key, std::size_t maximumEntries, std::size_t maximumPayloadBytes);

    private:
        struct Entry final {
            Sha256Digest digest;
            std::weak_ptr<const Detail::AssetPayloadAllocation> allocation;
            std::shared_ptr<const Detail::AssetPayloadAllocation> resident;
        };

        std::vector<Entry> entries_;
        std::shared_ptr<Detail::AssetPayloadAccounting> accounting_;
        std::size_t maximumEntries_{};
        std::size_t maximumPayloadBytes_{};
        bool closed_{};
    };
}  // namespace Horo::Assets
