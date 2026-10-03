#include "Horo/Assets/AssetPayloadCache.h"

#include "../AssetErrors.h"

#include <algorithm>
#include <atomic>

namespace Horo::Assets {
    namespace Detail {
        /** @brief Shared counter survives the cache while worker-held allocations remain alive. */
        struct AssetPayloadAccounting final {
            std::atomic<std::size_t> retainedBytes{};
        };

        /** @brief Immutable vector and its exact capacity charge, released on the final lease thread. */
        struct AssetPayloadAllocation final {
            AssetPayloadAllocation() = default;
            AssetPayloadAllocation(const AssetPayloadAllocation &) = delete;
            AssetPayloadAllocation &operator=(const AssetPayloadAllocation &) = delete;
            AssetPayloadAllocation(AssetPayloadAllocation &&) = delete;
            AssetPayloadAllocation &operator=(AssetPayloadAllocation &&) = delete;
            std::vector<std::byte> bytes;
            Sha256Digest digest;
            std::shared_ptr<AssetPayloadAccounting> accounting;

            ~AssetPayloadAllocation() {
                if (accounting) {
                    const auto capacity = bytes.capacity();
                    // The owner may admit more bytes as soon as it observes the refund.
                    std::vector<std::byte>{}.swap(bytes);
                    accounting->retainedBytes.fetch_sub(capacity);
                }
            }
        };
    }  // namespace Detail

    /** @copydoc AssetPayloadLease::AssetPayloadLease */
    AssetPayloadLease::AssetPayloadLease(std::shared_ptr<const Detail::AssetPayloadAllocation> allocation) noexcept
        : allocation_(std::move(allocation)) {}

    /** @copydoc AssetPayloadLease::Bytes */
    std::span<const std::byte> AssetPayloadLease::Bytes() const noexcept {
        return allocation_ ? std::span<const std::byte>{allocation_->bytes} : std::span<const std::byte>{};
    }

    /** @copydoc AssetPayloadLease::Digest */
    Sha256Digest AssetPayloadLease::Digest() const noexcept {
        return allocation_ ? allocation_->digest : Sha256Digest{};
    }

    /** @copydoc AssetPayloadLease::SharesAllocationWith */
    bool AssetPayloadLease::SharesAllocationWith(const AssetPayloadLease &other) const noexcept {
        return allocation_ && allocation_ == other.allocation_;
    }

    /** @copydoc AssetPayloadCache::Create */
    Result<std::unique_ptr<AssetPayloadCache>> AssetPayloadCache::Create(const std::size_t maximumEntries,
                                                                         const std::size_t maximumPayloadBytes) {
        if (maximumEntries == 0 || maximumEntries > 65'536 || maximumPayloadBytes == 0) {
            return Result<std::unique_ptr<AssetPayloadCache>>::Failure(MakeError(CookErrors::TooLarge));
        }
        return Result<std::unique_ptr<AssetPayloadCache>>::Success(
            std::make_unique<AssetPayloadCache>(ConstructionKey{}, maximumEntries, maximumPayloadBytes));
    }

    /** @copydoc AssetPayloadCache::AssetPayloadCache */
    AssetPayloadCache::AssetPayloadCache(ConstructionKey, const std::size_t maximumEntries, const std::size_t maximumPayloadBytes)
        : accounting_(std::make_shared<Detail::AssetPayloadAccounting>()), maximumEntries_(maximumEntries),
          maximumPayloadBytes_(maximumPayloadBytes) {
        entries_.reserve(maximumEntries);
    }

    /** @copydoc AssetPayloadCache::Admit */
    Result<AssetPayloadLease> AssetPayloadCache::Admit(const std::span<const std::byte> bytes) {
        if (closed_) {
            return Result<AssetPayloadLease>::Failure(MakeError(AssetErrors::LoadShutdown));
        }
        if (bytes.empty() || bytes.size() > maximumPayloadBytes_) {
            return Result<AssetPayloadLease>::Failure(MakeError(CookErrors::TooLarge));
        }
        const auto digest = ComputeSha256(bytes);
        if (const auto found = std::ranges::find(entries_, digest, &Entry::digest); found != entries_.end()) {
            if (auto existing = found->allocation.lock()) {
                if (!std::ranges::equal(existing->bytes, bytes)) {
                    return Result<AssetPayloadLease>::Failure(MakeError(CookErrors::HashMismatch));
                }
                found->resident = existing;
                return Result<AssetPayloadLease>::Success(AssetPayloadLease{std::move(existing)});
            }
        }
        std::erase_if(entries_, [](const Entry &entry) {
            return entry.allocation.expired();
        });
        const auto retained = accounting_->retainedBytes.load();
        if (entries_.size() == maximumEntries_ || bytes.size() > maximumPayloadBytes_ - retained) {
            return Result<AssetPayloadLease>::Failure(MakeError(CookErrors::TooLarge));
        }
        auto allocation = std::make_shared<Detail::AssetPayloadAllocation>();
        allocation->bytes.assign(bytes.begin(), bytes.end());
        if (allocation->bytes.capacity() > maximumPayloadBytes_ - retained) {
            return Result<AssetPayloadLease>::Failure(MakeError(CookErrors::TooLarge));
        }
        allocation->digest = digest;
        accounting_->retainedBytes.fetch_add(allocation->bytes.capacity());
        allocation->accounting = accounting_;
        entries_.emplace_back(digest, allocation, allocation);
        return Result<AssetPayloadLease>::Success(AssetPayloadLease{std::move(allocation)});
    }

    /** @copydoc AssetPayloadCache::Evict */
    void AssetPayloadCache::Evict(const Sha256Digest &digest) noexcept {
        for (auto &entry : entries_) {
            if (entry.digest == digest) {
                entry.resident.reset();
            }
        }
    }

    /** @copydoc AssetPayloadCache::Shutdown */
    void AssetPayloadCache::Shutdown() noexcept {
        closed_ = true;
        entries_.clear();
    }

    /** @copydoc AssetPayloadCache::Snapshot */
    AssetPayloadCacheSnapshot AssetPayloadCache::Snapshot() const noexcept {
        AssetPayloadCacheSnapshot result;
        result.retainedPayloadBytes = accounting_->retainedBytes.load();
        result.bookkeepingBytes = entries_.capacity() * sizeof(Entry);
        for (const auto &entry : entries_) {
            if (entry.resident) {
                result.residentPayloadBytes += entry.resident->bytes.capacity();
                ++result.residentEntries;
            }
        }
        return result;
    }
}  // namespace Horo::Assets
