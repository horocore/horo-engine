#pragma once

/** @file TerrainResidencyCache.h
 * @brief Terrain-owned neutral CPU residency inside host-provided World Streaming reservations. */

#include "Horo/Terrain/TerrainIdentity.h"
#include "Horo/WorldStreaming/StreamingFeatureBudgetReservations.h"

#include <memory>
#include <span>
#include <variant>
#include <vector>

namespace Horo::Terrain {
    namespace Detail {
        struct TerrainResidencyOwnerTag;
    }

    /** @brief Host-issued unique cache lifetime; never reuse while old tokens may exist. */
    using TerrainResidencyOwnerId = Foundation::Detail::NonZeroId64<Detail::TerrainResidencyOwnerTag, TerrainErrors::IdentityInvalid>;

    /** @brief Exact neutral payload identity; revisions never alias across replacement generations. */
    struct TerrainResidencyKey final {
        TerrainRuntimeHandle runtime;
        TerrainContentRevision content;
        TerrainCapabilityRevision capability;
        std::variant<TerrainTileId, FoliageClusterId> payload;
        /** @brief Checks complete identity and tile dataset ownership. @return True for a usable typed key. */
        [[nodiscard]] bool IsValid() const noexcept;
        [[nodiscard]] auto operator<=>(const TerrainResidencyKey &) const noexcept = default;
    };

    /** @brief Every retention reason uses the same bounded non-evictable lease policy. */
    enum class TerrainResidencyRetention : std::uint8_t {
        Cell,
        Worker,
        Snapshot,
        Replacement,
        Consumer,
        Count
    };

    /** @brief Exact cache and WST consumer token; release only after readers and dependent work retire. */
    struct TerrainResidencyLease final {
        TerrainResidencyOwnerId owner;
        std::uint64_t reader{}; /**< Non-reused local reader incarnation, independent of a shared WST consumer lease. */
        TerrainResidencyKey key;
        TerrainResidencyRetention retention;
        WorldStreaming::SharedAssetLease budget;
        [[nodiscard]] auto operator<=>(const TerrainResidencyLease &) const noexcept = default;
    };

    /** @brief Bounded owner metadata; all payload memory is separately reserved and charged through WST. */
    struct TerrainResidencyLimits final {
        std::uint32_t entries{256};                          /**< Positive, at most WST's shared-entry hard bound. */
        std::uint32_t leases{1024};                          /**< Positive, at most WST's shared-lease hard bound. */
        std::uint64_t maximumPayloadBytes{16 * 1024 * 1024}; /**< Positive per-entry owned capacity ceiling. */
    };

    /** @brief Progress of bounded authority-requested local pressure work; pinned entries remain charged. */
    struct TerrainResidencyEviction final {
        std::uint32_t evicted{};
        std::uint64_t releasedCpuBytes{};
        bool targetReached{}; /**< False means more bounded work or WST-level pressure handling is required. */
    };

    /**
     * @brief Host-composed TerrainRuntime cache; World Streaming remains sole demand and aggregate budget authority.
     * @details All calls run on StreamingAuthorityRole after Terrain workers route completion to that lane. Creation
     * preallocates bounded metadata. Accepted payloads are immutable, validated neutral CPU data; no native resource,
     * durable foliage state, provider byte lease, source asset or cooked publication is owned here. The borrowed WST
     * authority must remain at its address and outlive this cache. Destroy only after shutdown and complete drain.
     * In-flight/replacement/snapshot/cell retention consumes bounded leases and blocks LRU eviction without exceptions.
     */
    class TerrainResidencyCache final {
    public:
        TerrainResidencyCache(const TerrainResidencyCache &) = delete;
        TerrainResidencyCache &operator=(const TerrainResidencyCache &) = delete;
        /** @brief Destroys a drained cache. @pre Every payload and lease has been retired through this owner. */
        ~TerrainResidencyCache();
        /** @brief Preallocates one empty cache against the exact authority lifetime.
         * @param owner Unique host-issued cache lifetime. @param authority Borrowed canonical WST ledger.
         * @param limits Positive bounded metadata and per-payload capacity limits.
         * @throws std::bad_alloc When bounded metadata allocation fails.
         * @return Cache or typed invalid/capacity/lifecycle error. */
        [[nodiscard]] static Result<std::unique_ptr<TerrainResidencyCache>> Create(
            TerrainResidencyOwnerId owner, WorldStreaming::StreamingFeatureBudgetReservations &authority,
            TerrainResidencyLimits limits = {});
        /** @brief Transfers a validated prepared payload under an already admitted peak, returning its first lease.
         * @param key Exact trusted manifest/runtime identity. @param reservation WST-admitted current operation.
         * @param allocation Exact physical cache allocation identity, unique to this payload/revision.
         * @param cost Complete CPU-only physical charge, including vector capacity; other owners charge their own memory.
         * @param payload Owned immutable candidate bytes prepared only after WST peak admission, nonempty.
         * @param retention First reader's explicit retention reason.
         * @return Lease or typed error without cache insertion or WST mutation. Caller retains rejected candidate ownership.
         * @pre Caller validated membership, digest, schema and decoded representation against the pinned manifest.
         * Candidate allocation was covered by the supplied reservation before allocation. */
        [[nodiscard]] Result<TerrainResidencyLease> Insert(const TerrainResidencyKey &key,
                                                           const WorldStreaming::StreamingFeatureBudgetReservation &reservation,
                                                           const WorldStreaming::SharedAssetKey &allocation,
                                                           const WorldStreaming::StreamingBudgetAmounts &cost,
                                                           std::vector<std::uint8_t> &&payload, TerrainResidencyRetention retention);
        /** @brief Retains an exact cache hit and resolves only the declared already-reserved peak portion.
         * @param key Exact requested identity; changed content/capability is a cache miss.
         * @param reservation WST-admitted current operation. @param peakPortion Explicit complete reuse portion, normally zero; must be
         * zero for another reader of the same WST cell consumer.
         * @param retention Reader's reason. @return Exact bounded lease or typed failure; no recook or fallback. */
        [[nodiscard]] Result<TerrainResidencyLease> Acquire(const TerrainResidencyKey &key,
                                                            const WorldStreaming::StreamingFeatureBudgetReservation &reservation,
                                                            const WorldStreaming::StreamingBudgetAmounts &peakPortion,
                                                            TerrainResidencyRetention retention);
        /** @brief Borrows immutable bytes without extending retention. @param lease Exact retained token.
         * @return View or typed stale error. @pre The view is used only until its lease is released; workers retain the lease. */
        [[nodiscard]] Result<std::span<const std::uint8_t>> Read(const TerrainResidencyLease &lease) const;
        /** @brief Releases one reader after dependent retirement; allocation remains charged until eviction.
         * @param lease Exact retained token. @return Success or typed stale/authority error with ownership preserved. */
        [[nodiscard]] Result<void> Release(const TerrainResidencyLease &lease);
        /** @brief Evicts oldest unleased entries toward an explicit WST-requested CPU target with bounded work.
         * @param targetCpuBytes Maximum retained physical CPU charge after work, including pinned entries.
         * @param maximumEvictions Positive bound no larger than configured entries; work is O(entries * maximumEvictions).
         * @return Progress or typed error. Stable accepted-access order determines LRU, independent of cell priority.
         * Bytes are destroyed before exact WST retirement acknowledgement; leased payloads are never selected.
         * Can run during shutdown; unreachable targets report pressure to the authority without forcing cell eviction. */
        [[nodiscard]] Result<TerrainResidencyEviction> EvictTo(std::uint64_t targetCpuBytes, std::uint32_t maximumEvictions);
        /** @brief Closes new insertion and leases idempotently; accepted readers and charges remain until explicit drain. */
        void BeginShutdown() noexcept;
        /** @brief Reports complete retirement. @return True when no payloads or leases remain. */
        [[nodiscard]] bool IsDrained() const noexcept;
        /** @brief Returns retained physical CPU charge including unleased cache entries. @return Charged bytes. */
        [[nodiscard]] std::uint64_t CpuBytes() const noexcept;
        /** @brief Returns retained payload count. @return Number of cache entries. */
        [[nodiscard]] std::size_t EntryCount() const noexcept;
        /** @brief Returns retained consumer count. @return Number of active leases. */
        [[nodiscard]] std::size_t LeaseCount() const noexcept;

    private:
        struct Entry final {
            TerrainResidencyKey key;
            WorldStreaming::SharedAssetKey allocation;
            WorldStreaming::StreamingBudgetAmounts cost;
            WorldStreaming::SharedAssetChargeId charge;
            std::vector<std::uint8_t> payload;
            std::uint64_t access;
            std::uint32_t readers;
            std::optional<WorldStreaming::SharedAssetRetirement> retirement;
        };

        /** @brief Only the validated Create path may issue this allocation key. */
        class ConstructionKey final {
            ConstructionKey() = default;
            friend class TerrainResidencyCache;
        };

    public:
        /** @brief Constructs prevalidated bounded storage through Create's private allocation key.
         * @param owner Validated cache lifetime. @param authority Admitted borrowed WST authority.
         * @param limits Validated finite metadata limits. @param key Private evidence of factory admission. */
        TerrainResidencyCache(TerrainResidencyOwnerId owner, WorldStreaming::StreamingFeatureBudgetReservations &authority,
                              TerrainResidencyLimits limits, ConstructionKey key);

    private:
        [[nodiscard]] Result<void> ValidateAdmission(const TerrainResidencyKey &key, TerrainResidencyRetention retention) const;
        [[nodiscard]] bool HasLease(const TerrainResidencyLease &lease) const noexcept;
        TerrainResidencyOwnerId owner_;
        WorldStreaming::StreamingFeatureBudgetReservations &authority_;
        WorldStreaming::StreamingRuntimeOwnerToken budgetOwner_;
        TerrainResidencyLimits limits_;
        std::vector<Entry> entries_;
        std::vector<TerrainResidencyLease> leases_;
        std::uint64_t nextAccess_{1};
        std::uint64_t cpuBytes_{};
        bool closed_{};
    };
}  // namespace Horo::Terrain
