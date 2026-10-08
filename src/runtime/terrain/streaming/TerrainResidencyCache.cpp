#include "Horo/TerrainStreaming/TerrainResidencyCache.h"

#include <algorithm>
#include <cassert>
#include <limits>

namespace Horo::Terrain {
    namespace WST = WorldStreaming;

    /** @copydoc TerrainResidencyKey::IsValid */
    bool TerrainResidencyKey::IsValid() const noexcept {
        if (!runtime.IsValid() || !content.IsValid() || !capability.IsValid())
            return false;
        if (const auto *tile = std::get_if<TerrainTileId>(&payload))
            return tile->IsValid() && tile->dataset == runtime.dataset;
        return std::get<FoliageClusterId>(payload).IsValid();
    }

    /** @copydoc TerrainResidencyCache::TerrainResidencyCache */
    TerrainResidencyCache::TerrainResidencyCache(TerrainResidencyOwnerId owner, WST::StreamingFeatureBudgetReservations &authority,
                                                 TerrainResidencyLimits limits, ConstructionKey)
        : owner_(owner), authority_(authority), budgetOwner_(authority.Context().owner), limits_(limits) {
        entries_.reserve(limits.entries);
        leases_.reserve(limits.leases);
    }

    /** @copydoc TerrainResidencyCache::~TerrainResidencyCache */
    TerrainResidencyCache::~TerrainResidencyCache() {
        assert(IsDrained());
    }

    /** @copydoc TerrainResidencyCache::Create */
    Result<std::unique_ptr<TerrainResidencyCache>> TerrainResidencyCache::Create(TerrainResidencyOwnerId owner,
                                                                                 WST::StreamingFeatureBudgetReservations &authority,
                                                                                 TerrainResidencyLimits limits) {
        if (!owner.IsValid() || limits.entries == 0 || limits.entries > WST::SharedAssetResidencyLimits::MaximumEntries ||
            limits.leases == 0 || limits.leases > WST::SharedAssetResidencyLimits::MaximumLeases || limits.maximumPayloadBytes == 0)
            return Result<std::unique_ptr<TerrainResidencyCache>>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
        if (authority.State() != WST::StreamingSchedulerAdmissionState::Accepting)
            return Result<std::unique_ptr<TerrainResidencyCache>>::Failure(MakeError(TerrainErrors::LifecycleUnavailable));
        return Result<std::unique_ptr<TerrainResidencyCache>>::Success(
            std::make_unique<TerrainResidencyCache>(owner, authority, limits, ConstructionKey{}));
    }

    /** @brief Validates local admission before touching the canonical WST authority. */
    Result<void> TerrainResidencyCache::ValidateAdmission(const TerrainResidencyKey &key, TerrainResidencyRetention retention) const {
        if (!key.IsValid() || retention >= TerrainResidencyRetention::Count)
            return Result<void>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
        if (closed_ || authority_.Context().owner != budgetOwner_)
            return Result<void>::Failure(MakeError(TerrainErrors::LifecycleUnavailable));
        if (leases_.size() == limits_.leases)
            return Result<void>::Failure(MakeError(TerrainErrors::CapacityExceeded));
        if (nextAccess_ == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(TerrainErrors::GenerationExhausted));
        return Result<void>::Success();
    }

    /** @copydoc TerrainResidencyCache::Insert */
    Result<TerrainResidencyLease> TerrainResidencyCache::Insert(const TerrainResidencyKey &key,
                                                                const WST::StreamingFeatureBudgetReservation &reservation,
                                                                const WST::SharedAssetKey &allocation,
                                                                const WST::StreamingBudgetAmounts &cost,
                                                                std::vector<std::uint8_t> &&payload, TerrainResidencyRetention retention) {
        if (const auto valid = ValidateAdmission(key, retention); valid.HasError())
            return Result<TerrainResidencyLease>::Failure(valid.ErrorValue());
        if (!allocation.IsValid() || payload.empty())
            return Result<TerrainResidencyLease>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
        const auto cpu = cost.Value(WST::StreamingBudgetDimension::CpuResidentBytes).Value();
        if (payload.capacity() > limits_.maximumPayloadBytes || cpu < payload.capacity() || cpu > limits_.maximumPayloadBytes ||
            cpu > std::numeric_limits<std::uint64_t>::max() - cpuBytes_ || entries_.size() == limits_.entries)
            return Result<TerrainResidencyLease>::Failure(MakeError(TerrainErrors::CapacityExceeded));
        for (const auto &amount : cost.Entries()) {
            if (amount.dimension != WST::StreamingBudgetDimension::CpuResidentBytes && amount.value != 0)
                return Result<TerrainResidencyLease>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
        }
        for (const auto &entry : entries_) {
            if (entry.key == key || entry.allocation == allocation)
                return Result<TerrainResidencyLease>::Failure(MakeError(TerrainErrors::IdentityConflict));
        }
        // A new owned buffer cannot masquerade as a previously charged cache allocation.
        const auto existing = authority_.SharedAssets().Inspect(allocation);
        if (existing.HasError())
            return Result<TerrainResidencyLease>::Failure(existing.ErrorValue());
        if (existing.Value())
            return Result<TerrainResidencyLease>::Failure(MakeError(TerrainErrors::IdentityConflict));
        const auto feature = std::holds_alternative<TerrainTileId>(key.payload) ? WST::StreamingBudgetFeature::Terrain
                                                                                : WST::StreamingBudgetFeature::Foliage;
        const auto realized =
            authority_.RealizeShared(authority_.Context(), reservation,
                                     {feature, allocation, cost, cost, {reservation.scheduler.operation.fence, std::nullopt}});
        if (realized.HasError())
            return Result<TerrainResidencyLease>::Failure(realized.ErrorValue());
        TerrainResidencyLease lease{owner_, nextAccess_, key, retention, realized.Value()};
        entries_.emplace_back(key, allocation, cost, lease.budget.charge, std::move(payload), nextAccess_++, 1, std::nullopt);
        leases_.push_back(lease);
        cpuBytes_ += cpu;
        return Result<TerrainResidencyLease>::Success(lease);
    }

    /** @copydoc TerrainResidencyCache::Acquire */
    Result<TerrainResidencyLease> TerrainResidencyCache::Acquire(const TerrainResidencyKey &key,
                                                                 const WST::StreamingFeatureBudgetReservation &reservation,
                                                                 const WST::StreamingBudgetAmounts &peakPortion,
                                                                 TerrainResidencyRetention retention) {
        if (const auto valid = ValidateAdmission(key, retention); valid.HasError())
            return Result<TerrainResidencyLease>::Failure(valid.ErrorValue());
        const auto entry = std::ranges::find(entries_, key, &Entry::key);
        if (entry == entries_.end() || entry->retirement)
            return Result<TerrainResidencyLease>::Failure(MakeError(TerrainErrors::IdentityUnknown));
        const auto existing = std::ranges::find_if(leases_, [&](const TerrainResidencyLease &lease) {
            return lease.key == key && lease.budget.consumer.fence == reservation.scheduler.operation.fence;
        });
        std::optional<WST::SharedAssetLease> budget;
        if (existing != leases_.end()) {
            // WST owns one lease per allocation/cell consumer; local readers share that exact retention.
            const auto operation = authority_.Runtime().Scheduler().Inspect(reservation.scheduler);
            if (operation.HasError())
                return Result<TerrainResidencyLease>::Failure(operation.ErrorValue());
            if (reservation.owner != budgetOwner_ || authority_.State() != WST::StreamingSchedulerAdmissionState::Accepting ||
                operation.Value().IsTerminal() || operation.Value().State() == WST::StreamingCellOperationState::Retiring)
                return Result<TerrainResidencyLease>::Failure(MakeError(WST::WorldStreamingErrors::FeatureBudgetLifecycleUnavailable));
            if (!peakPortion.IsZero())
                return Result<TerrainResidencyLease>::Failure(MakeError(WST::WorldStreamingErrors::SharedAssetConflict));
            budget = existing->budget;
        } else {
            const auto feature = std::holds_alternative<TerrainTileId>(key.payload) ? WST::StreamingBudgetFeature::Terrain
                                                                                    : WST::StreamingBudgetFeature::Foliage;
            const auto realized = authority_.RealizeShared(authority_.Context(), reservation,
                                                           {feature,
                                                            entry->allocation,
                                                            entry->cost,
                                                            peakPortion,
                                                            {reservation.scheduler.operation.fence, std::nullopt}});
            if (realized.HasError())
                return Result<TerrainResidencyLease>::Failure(realized.ErrorValue());
            budget = realized.Value();
        }
        TerrainResidencyLease lease{owner_, nextAccess_, key, retention, *budget};
        leases_.push_back(lease);
        entry->access = nextAccess_++;
        ++entry->readers;
        return Result<TerrainResidencyLease>::Success(lease);
    }

    /** @brief Checks the complete retained token, including retention kind and WST charge incarnation. */
    bool TerrainResidencyCache::HasLease(const TerrainResidencyLease &lease) const noexcept {
        return lease.owner == owner_ && std::ranges::find(leases_, lease) != leases_.end();
    }

    /** @copydoc TerrainResidencyCache::Read */
    Result<std::span<const std::uint8_t>> TerrainResidencyCache::Read(const TerrainResidencyLease &lease) const {
        if (!HasLease(lease))
            return Result<std::span<const std::uint8_t>>::Failure(MakeError(TerrainErrors::GenerationStale));
        const auto entry = std::ranges::find(entries_, lease.key, &Entry::key);
        return Result<std::span<const std::uint8_t>>::Success(entry->payload);
    }

    /** @copydoc TerrainResidencyCache::Release */
    Result<void> TerrainResidencyCache::Release(const TerrainResidencyLease &lease) {
        if (!HasLease(lease))
            return Result<void>::Failure(MakeError(TerrainErrors::GenerationStale));
        if (const bool lastReader = std::ranges::none_of(leases_,
                                                         [&](const TerrainResidencyLease &other) {
            return other.reader != lease.reader && other.budget == lease.budget;
        });
            lastReader) {
            const auto released = authority_.ReleaseShared(authority_.Context(), lease.budget);
            if (released.HasError())
                return released;
        }
        --std::ranges::find(entries_, lease.key, &Entry::key)->readers;
        leases_.erase(std::ranges::find(leases_, lease));
        return Result<void>::Success();
    }

    /** @copydoc TerrainResidencyCache::EvictTo */
    Result<TerrainResidencyEviction> TerrainResidencyCache::EvictTo(std::uint64_t targetCpuBytes, std::uint32_t maximumEvictions) {
        if (maximumEvictions == 0 || maximumEvictions > limits_.entries)
            return Result<TerrainResidencyEviction>::Failure(MakeError(TerrainErrors::DescriptorInvalid));
        TerrainResidencyEviction progress{};
        while (cpuBytes_ > targetCpuBytes && progress.evicted < maximumEvictions) {
            auto oldest = entries_.end();
            for (auto it = entries_.begin(); it != entries_.end(); ++it) {
                const bool retained = it->readers != 0;
                if (!retained && (oldest == entries_.end() || it->access < oldest->access))
                    oldest = it;
            }
            if (oldest == entries_.end())
                break;
            if (!oldest->retirement) {
                const auto retiring = authority_.BeginRetireShared(authority_.Context(), oldest->allocation, oldest->charge);
                if (retiring.HasError())
                    return Result<TerrainResidencyEviction>::Failure(retiring.ErrorValue());
                oldest->retirement = retiring.Value();
                std::vector<std::uint8_t>{}.swap(oldest->payload);
            }
            if (const auto retired = authority_.AcknowledgeSharedRetired(authority_.Context(), *oldest->retirement); retired.HasError())
                return Result<TerrainResidencyEviction>::Failure(retired.ErrorValue());
            const auto bytes = oldest->cost.Value(WST::StreamingBudgetDimension::CpuResidentBytes).Value();
            cpuBytes_ -= bytes;
            progress.releasedCpuBytes += bytes;
            ++progress.evicted;
            entries_.erase(oldest);
        }
        progress.targetReached = cpuBytes_ <= targetCpuBytes;
        return Result<TerrainResidencyEviction>::Success(progress);
    }

    /** @copydoc TerrainResidencyCache::BeginShutdown */
    void TerrainResidencyCache::BeginShutdown() noexcept {
        closed_ = true;
    }

    /** @copydoc TerrainResidencyCache::IsDrained */
    bool TerrainResidencyCache::IsDrained() const noexcept {
        return entries_.empty() && leases_.empty();
    }

    /** @copydoc TerrainResidencyCache::CpuBytes */
    std::uint64_t TerrainResidencyCache::CpuBytes() const noexcept {
        return cpuBytes_;
    }

    /** @copydoc TerrainResidencyCache::EntryCount */
    std::size_t TerrainResidencyCache::EntryCount() const noexcept {
        return entries_.size();
    }

    /** @copydoc TerrainResidencyCache::LeaseCount */
    std::size_t TerrainResidencyCache::LeaseCount() const noexcept {
        return leases_.size();
    }
}  // namespace Horo::Terrain
