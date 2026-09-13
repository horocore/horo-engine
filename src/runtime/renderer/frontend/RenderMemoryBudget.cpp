#include "Horo/Runtime/Render/RenderMemoryBudget.h"

#include "Horo/Runtime/Render/RenderMemoryBudgetErrors.h"
#include "RenderMemoryBudgetInternals.h"

#include <algorithm>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Horo::Render {
    using namespace Detail;

    class RenderMemoryBudget::Impl final {
    public:
        Impl(const RenderResourceOwnerId renderer, const RenderMemoryBudgetConfig &config) : renderer_(renderer), config_(config) {
            pools_.reserve(config.maximumPools);
            blocks_.reserve(config.maximumBlocks);
            regions_.resize(config.maximumAllocations);
            sortedRegionsScratch_.reserve(config.maximumAllocations);
        }

        [[nodiscard]] Result<RenderMemoryReservationId> Reserve(const RenderMemoryScopeId scope, const ResourceOperationId attempt,
                                                                const RenderMemoryCostPlan &plan) {
            if (const Result<void> validation = ValidateReservationRequest(scope, attempt, plan); validation.HasError())
                return Result<RenderMemoryReservationId>::Failure(validation.ErrorValue());

            Pool *pool = FindPool(scope, plan.memoryClass, plan.compatibility);
            if (auto existing = TryReserveExistingBlock(pool, attempt, plan); existing.has_value())
                return std::move(*existing);
            return ReserveNewBlock(pool, scope, attempt, plan);
        }

        [[nodiscard]] Result<void> Cancel(const RenderMemoryReservationId reservation) {
            auto found = FindReservation(reservation);
            if (!found.has_value())
                return Failure<void>(RenderMemoryBudgetErrors::InvalidReservation,
                                     "Reservation is malformed, foreign, stale, or already consumed.");
            const Block &block = blocks_[found->block];
            regions_[found->region] = {};
            --reservationCount_;
            if (!block.committed && !HasRegions(block.id))
                RemoveBlock(found->block);
            return Result<void>::Success();
        }

        [[nodiscard]] Result<RenderMemoryPlacement> Placement(const RenderMemoryReservationId reservation) const {
            const auto found = FindReservation(reservation);
            if (!found.has_value())
                return Failure<RenderMemoryPlacement>(RenderMemoryBudgetErrors::InvalidReservation,
                                                      "Reservation is malformed, foreign, stale, or already consumed.");
            const Block &block = blocks_[found->block];
            const Region &region = regions_[found->region];
            const Pool *pool = FindPool(block.pool);
            return Result<RenderMemoryPlacement>::Success(
                {.pool = block.pool,
                 .scope = pool->scope,
                 .attempt = region.attempt,
                 .memoryClass = pool->memoryClass,
                 .compatibility = pool->compatibility,
                 .provenance = region.provenance,
                 .budgetRevision = region.budgetRevision,
                 .offsetBytes = region.offset,
                 .payloadBytes = region.payloadBytes,
                 .requiredBytes = region.requiredBytes,
                 .backingBytes = block.capacity,
                 .allocationClass = block.dedicated ? RenderMemoryAllocationClass::Dedicated : RenderMemoryAllocationClass::Suballocated});
        }

        [[nodiscard]] Result<RenderMemoryAllocation> Commit(const RenderMemoryReservationId reservation) {
            auto found = FindReservation(reservation);
            if (!found.has_value())
                return Failure<RenderMemoryAllocation>(RenderMemoryBudgetErrors::InvalidReservation,
                                                       "Reservation is malformed, foreign, stale, or already consumed.");
            Block &block = blocks_[found->block];
            Region &region = regions_[found->region];
            region.allocation = nextAllocation_++;
            region.reservation = 0;
            region.state = RegionState::Live;
            --reservationCount_;
            ++allocationCount_;
            block.committed = true;
            const Pool *pool = FindPool(block.pool);
            return Result<RenderMemoryAllocation>::Success(
                {.id = RenderMemoryAllocationId{renderer_, region.allocation},
                 .pool = block.pool,
                 .scope = pool->scope,
                 .attempt = region.attempt,
                 .memoryClass = pool->memoryClass,
                 .compatibility = pool->compatibility,
                 .provenance = region.provenance,
                 .budgetRevision = region.budgetRevision,
                 .offsetBytes = region.offset,
                 .payloadBytes = region.payloadBytes,
                 .requiredBytes = region.requiredBytes,
                 .backingBytes = block.capacity,
                 .allocationClass = block.dedicated ? RenderMemoryAllocationClass::Dedicated : RenderMemoryAllocationClass::Suballocated});
        }

        [[nodiscard]] Result<void> BeginRetire(const RenderMemoryAllocationId allocation) {
            Region *region = FindAllocation(allocation);
            if (region == nullptr || region->state != RegionState::Live)
                return InvalidAllocationResult();
            region->state = RegionState::Retiring;
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AcknowledgeRetirement(const RenderMemoryAllocationId allocation) {
            const auto found = FindAllocationLocation(allocation);
            if (!found.has_value() || regions_[found->region].state != RegionState::Retiring)
                return InvalidAllocationResult();
            regions_[found->region] = {};
            --allocationCount_;
            return Result<void>::Success();
        }

        [[nodiscard]] std::size_t ReclaimEmptyBlocks(const std::size_t maxBlocks) noexcept {
            std::size_t reclaimedBytes = 0;
            std::size_t reclaimedBlocks = 0;
            for (std::size_t index = 0; index < blocks_.size() && reclaimedBlocks < maxBlocks;) {
                if (blocks_[index].committed && !HasRegions(blocks_[index].id)) {
                    reclaimedBytes += blocks_[index].capacity;
                    RemoveBlock(index);
                    ++reclaimedBlocks;
                } else {
                    ++index;
                }
            }
            return reclaimedBytes;
        }

        [[nodiscard]] Result<void> ReviseHardCap(const std::size_t hardCapBytes, const std::uint64_t revision) {
            if (hardCapBytes == 0 || revision <= config_.revision)
                return Failure<void>(RenderMemoryBudgetErrors::InvalidConfiguration,
                                     "Budget revisions must advance and retain a finite non-zero hard cap.");
            config_.hardCapBytes = hardCapBytes;
            config_.revision = revision;
            return Result<void>::Success();
        }

        [[nodiscard]] RenderMemoryBudgetSnapshot Snapshot() const noexcept {
            RenderMemoryBudgetSnapshot snapshot{.revision = config_.revision,
                                                .hardCapBytes = config_.hardCapBytes,
                                                .peakChargedBytes = peakChargedBytes_,
                                                .failedReservationCount = failedReservationCount_,
                                                .poolCount = static_cast<std::uint32_t>(pools_.size()),
                                                .blockCount = static_cast<std::uint32_t>(blocks_.size()),
                                                .reservationCount = reservationCount_,
                                                .allocationCount = allocationCount_,
                                                .acceptingReservations = acceptingReservations_};
            for (const Block &block : blocks_) {
                if (block.committed)
                    snapshot.committedBackingBytes += block.capacity;
                else
                    snapshot.reservedUnallocatedBytes += block.capacity;
            }
            for (const Region &region : regions_) {
                if (!region.active)
                    continue;
                if (region.state == RegionState::Reserved)
                    snapshot.reservedPayloadBytes += region.payloadBytes;
                else if (region.state == RegionState::Live)
                    snapshot.livePayloadBytes += region.payloadBytes;
                else
                    snapshot.retiringPayloadBytes += region.payloadBytes;
            }
            const std::span<const Region *const> sortedRegions = SortActiveRegions();
            for (const Pool &pool : pools_) {
                const RenderMemoryPoolSnapshot poolSnapshot = MakePoolSnapshot(pool, sortedRegions);
                snapshot.reusableSlackBytes += poolSnapshot.reusableSlackBytes;
                snapshot.externalFragmentationBasisPoints =
                    std::max(snapshot.externalFragmentationBasisPoints, poolSnapshot.externalFragmentationBasisPoints);
            }
            snapshot.overBudget =
                snapshot.committedBackingBytes > snapshot.hardCapBytes ||
                snapshot.reservedUnallocatedBytes > snapshot.hardCapBytes - std::min(snapshot.committedBackingBytes, snapshot.hardCapBytes);
            return snapshot;
        }

        [[nodiscard]] Result<RenderMemoryPoolSnapshot> PoolSnapshot(const RenderMemoryPoolId pool) const {
            if (!pool.IsValid())
                return Failure<RenderMemoryPoolSnapshot>(RenderMemoryBudgetErrors::InvalidPool, "Memory pool identity is malformed.");
            const Pool *found = FindPool(pool);
            if (found == nullptr)
                return Failure<RenderMemoryPoolSnapshot>(RenderMemoryBudgetErrors::InvalidPool,
                                                         "Memory pool identity is stale or belongs to another ledger.");
            return Result<RenderMemoryPoolSnapshot>::Success(MakePoolSnapshot(*found, SortActiveRegions()));
        }

        void Shutdown() noexcept {
            if (!acceptingReservations_)
                return;
            acceptingReservations_ = false;
            blocks_.clear();
            pools_.clear();
            std::ranges::fill(regions_, Region{});
            reservationCount_ = 0;
            allocationCount_ = 0;
        }

    private:
        template <typename T> [[nodiscard]] Result<T> Failure(const ErrorCodeDescriptor &descriptor, const char *message) const {
            return Result<T>::Failure(MemoryBudgetError(descriptor, message));
        }

        template <typename T> [[nodiscard]] Result<T> CapacityFailure() {
            ++failedReservationCount_;
            return Failure<T>(RenderMemoryBudgetErrors::CapacityExceeded, "A bounded renderer memory record table is full.");
        }

        [[nodiscard]] Result<void> ValidateReservationRequest(const RenderMemoryScopeId scope, const ResourceOperationId attempt,
                                                              const RenderMemoryCostPlan &plan) {
            if (!acceptingReservations_)
                return Failure<void>(RenderMemoryBudgetErrors::Stopped, "Renderer memory reservation was requested during shutdown.");
            if (!scope.IsValid() || !attempt.IsValid())
                return Failure<void>(RenderMemoryBudgetErrors::InvalidRequest, "Memory scope or resource-attempt identity is invalid.");
            if (!plan.IsValid() || plan.alignment > config_.maximumAlignment)
                return Failure<void>(RenderMemoryBudgetErrors::InvalidCostPlan,
                                     "Memory size, alignment, allocation class, compatibility, or provenance is invalid.");
            if (ChargedBytes() > config_.hardCapBytes) {
                ++failedReservationCount_;
                return Failure<void>(RenderMemoryBudgetErrors::BudgetExceeded, "Existing renderer backing exceeds the revised hard cap.");
            }
            if (nextReservation_ == 0 || nextAllocation_ == 0 || reservationCount_ >= config_.maximumReservations ||
                reservationCount_ + allocationCount_ >= config_.maximumAllocations)
                return CapacityFailure<void>();
            return Result<void>::Success();
        }

        [[nodiscard]] std::optional<Result<RenderMemoryReservationId>> TryReserveExistingBlock(const Pool *pool,
                                                                                               const ResourceOperationId attempt,
                                                                                               const RenderMemoryCostPlan &plan) {
            if (plan.allocationClass != RenderMemoryAllocationClass::Suballocated)
                return std::nullopt;
            if (plan.requiredBytes > config_.maximumBlockBytes)
                return Failure<RenderMemoryReservationId>(RenderMemoryBudgetErrors::UnsupportedAllocation,
                                                          "Suballocated requirements exceed the configured maximum block size.");
            if (pool == nullptr)
                return std::nullopt;
            for (Block &block : blocks_) {
                if (block.pool != pool->id || block.dedicated)
                    continue;
                if (const auto offset = Detail::FindFirstMemoryFit(block, regions_, plan.requiredBytes, plan.alignment); offset.has_value())
                    return AddReservation(block, *offset, attempt, plan);
            }
            return std::nullopt;
        }

        [[nodiscard]] Result<RenderMemoryReservationId> ReserveNewBlock(const Pool *pool, const RenderMemoryScopeId scope,
                                                                        const ResourceOperationId attempt,
                                                                        const RenderMemoryCostPlan &plan) {
            if (blocks_.size() >= config_.maximumBlocks)
                return CapacityFailure<RenderMemoryReservationId>();
            std::size_t backingBytes = plan.requiredBytes;
            if (plan.allocationClass == RenderMemoryAllocationClass::Suballocated) {
                const auto aligned = Detail::AlignMemoryUp(std::max(config_.defaultBlockBytes, plan.requiredBytes), plan.alignment);
                if (!aligned.has_value() || *aligned > config_.maximumBlockBytes)
                    return Failure<RenderMemoryReservationId>(RenderMemoryBudgetErrors::UnsupportedAllocation,
                                                              "Aligned suballocation block exceeds the configured maximum block size.");
                backingBytes = *aligned;
            }
            if (!CanCharge(backingBytes)) {
                ++failedReservationCount_;
                return Failure<RenderMemoryReservationId>(RenderMemoryBudgetErrors::BudgetExceeded,
                                                          "Whole backing capacity would exceed the renderer hard cap.");
            }
            if (pool == nullptr) {
                if (pools_.size() >= config_.maximumPools || nextPool_ == 0)
                    return CapacityFailure<RenderMemoryReservationId>();
                pools_.push_back(Pool{.id = RenderMemoryPoolId{renderer_, nextPool_++},
                                      .scope = scope,
                                      .memoryClass = plan.memoryClass,
                                      .compatibility = plan.compatibility});
                pool = &pools_.back();
            }
            if (nextBlock_ == 0)
                return CapacityFailure<RenderMemoryReservationId>();
            blocks_.push_back(Block{.id = nextBlock_++,
                                    .pool = pool->id,
                                    .capacity = backingBytes,
                                    .dedicated = plan.allocationClass == RenderMemoryAllocationClass::Dedicated});
            peakChargedBytes_ = std::max(peakChargedBytes_, ChargedBytes());
            return AddReservation(blocks_.back(), 0, attempt, plan);
        }

        [[nodiscard]] Result<void> InvalidAllocationResult() const {
            return Failure<void>(RenderMemoryBudgetErrors::InvalidAllocation,
                                 "Allocation is malformed, foreign, stale, or not in the required lifecycle state.");
        }

        [[nodiscard]] Result<RenderMemoryReservationId> AddReservation(const Block &block, const std::size_t offset,
                                                                       const ResourceOperationId attempt,
                                                                       const RenderMemoryCostPlan &plan) {
            const std::uint64_t reservation = nextReservation_++;
            const auto available = std::ranges::find(regions_, false, &Region::active);
            if (available == regions_.end())
                return CapacityFailure<RenderMemoryReservationId>();
            *available = {.active = true,
                          .block = block.id,
                          .pool = block.pool,
                          .reservation = reservation,
                          .attempt = attempt,
                          .budgetRevision = config_.revision,
                          .offset = offset,
                          .requiredBytes = plan.requiredBytes,
                          .payloadBytes = plan.payloadBytes,
                          .provenance = plan.provenance};
            ++reservationCount_;
            return Result<RenderMemoryReservationId>::Success({renderer_, reservation});
        }

        [[nodiscard]] bool HasRegions(const std::uint64_t block) const noexcept {
            return std::ranges::any_of(regions_, [block](const Region &region) {
                return region.active && region.block == block;
            });
        }

        [[nodiscard]] std::size_t ChargedBytes() const noexcept {
            std::size_t charged = 0;
            for (const Block &block : blocks_)
                charged += block.capacity;
            return charged;
        }

        [[nodiscard]] bool CanCharge(const std::size_t bytes) const noexcept {
            const std::size_t charged = ChargedBytes();
            return charged <= config_.hardCapBytes && bytes <= config_.hardCapBytes - charged;
        }

        [[nodiscard]] Pool *FindPool(const RenderMemoryScopeId scope, const RenderMemoryClass memoryClass,
                                     const RenderMemoryCompatibilityId compatibility) noexcept {
            const auto found = std::ranges::find_if(pools_, [scope, memoryClass, compatibility](const Pool &pool) {
                return pool.scope == scope && pool.memoryClass == memoryClass && pool.compatibility == compatibility;
            });
            return found == pools_.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] const Pool *FindPool(const RenderMemoryPoolId id) const noexcept {
            const auto found = std::ranges::find(pools_, id, &Pool::id);
            return found == pools_.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] std::optional<MemoryLocation> FindReservation(const RenderMemoryReservationId id) const noexcept {
            if (!id.IsValid() || id.renderer != renderer_)
                return std::nullopt;
            for (std::size_t regionIndex = 0; regionIndex < regions_.size(); ++regionIndex) {
                const Region &region = regions_[regionIndex];
                if (region.active && region.state == RegionState::Reserved && region.reservation == id.value) {
                    const auto block = std::ranges::find(blocks_, region.block, &Block::id);
                    if (block != blocks_.end())
                        return MemoryLocation{static_cast<std::size_t>(block - blocks_.begin()), regionIndex};
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] std::optional<MemoryLocation> FindAllocationLocation(const RenderMemoryAllocationId id) noexcept {
            if (!id.IsValid() || id.renderer != renderer_)
                return std::nullopt;
            for (std::size_t regionIndex = 0; regionIndex < regions_.size(); ++regionIndex) {
                const Region &region = regions_[regionIndex];
                if (region.active && region.state != RegionState::Reserved && region.allocation == id.value) {
                    const auto block = std::ranges::find(blocks_, region.block, &Block::id);
                    if (block != blocks_.end())
                        return MemoryLocation{static_cast<std::size_t>(block - blocks_.begin()), regionIndex};
                }
            }
            return std::nullopt;
        }

        [[nodiscard]] Region *FindAllocation(const RenderMemoryAllocationId id) noexcept {
            const auto found = FindAllocationLocation(id);
            return found.has_value() ? &regions_[found->region] : nullptr;
        }

        void RemoveBlock(const std::size_t index) noexcept {
            const RenderMemoryPoolId pool = blocks_[index].pool;
            blocks_.erase(blocks_.begin() + static_cast<std::ptrdiff_t>(index));
            if (std::ranges::none_of(blocks_, [pool](const Block &block) {
                return block.pool == pool;
            }))
                std::erase_if(pools_, [pool](const Pool &candidate) {
                    return candidate.id == pool;
                });
        }

        [[nodiscard]] std::span<const Region *const> SortActiveRegions() const noexcept {
            sortedRegionsScratch_.clear();
            for (const Region &region : regions_) {
                if (region.active)
                    sortedRegionsScratch_.push_back(&region);
            }
            std::ranges::sort(sortedRegionsScratch_, [](const Region *left, const Region *right) {
                return left->block < right->block || (left->block == right->block && left->offset < right->offset);
            });
            return sortedRegionsScratch_;
        }

        static void AccumulateRegion(const Region &region, RenderMemoryPoolSnapshot &snapshot) noexcept {
            if (region.state == RegionState::Reserved) {
                snapshot.reservedPayloadBytes += region.payloadBytes;
                ++snapshot.reservationCount;
                return;
            }
            ++snapshot.allocationCount;
            if (region.state == RegionState::Live)
                snapshot.livePayloadBytes += region.payloadBytes;
            else
                snapshot.retiringPayloadBytes += region.payloadBytes;
        }

        [[nodiscard]] RenderMemoryPoolSnapshot MakePoolSnapshot(const Pool &pool,
                                                                const std::span<const Region *const> sortedRegions) const noexcept {
            RenderMemoryPoolSnapshot snapshot{.pool = pool.id,
                                              .scope = pool.scope,
                                              .memoryClass = pool.memoryClass,
                                              .compatibility = pool.compatibility};
            std::size_t largestFreeRegion = 0;
            for (const Block &block : blocks_) {
                if (block.pool != pool.id)
                    continue;
                ++snapshot.blockCount;
                if (block.committed)
                    snapshot.committedBackingBytes += block.capacity;
                else
                    snapshot.reservedUnallocatedBytes += block.capacity;

                const auto blockId = [](const Region *region) {
                    return region->block;
                };
                const auto first = std::ranges::lower_bound(sortedRegions, block.id, {}, blockId);
                const auto last = std::ranges::upper_bound(sortedRegions, block.id, {}, blockId);
                std::size_t cursor = 0;
                for (auto region = first; region != last; ++region) {
                    AccumulateRegion(**region, snapshot);
                    if (!block.committed || block.dedicated)
                        continue;
                    if ((*region)->offset > cursor) {
                        const std::size_t freeBytes = (*region)->offset - cursor;
                        snapshot.reusableSlackBytes += freeBytes;
                        largestFreeRegion = std::max(largestFreeRegion, freeBytes);
                    }
                    cursor = (*region)->offset + (*region)->requiredBytes;
                }
                if (block.committed && !block.dedicated && cursor < block.capacity) {
                    const std::size_t freeBytes = block.capacity - cursor;
                    snapshot.reusableSlackBytes += freeBytes;
                    largestFreeRegion = std::max(largestFreeRegion, freeBytes);
                }
            }
            if (snapshot.reusableSlackBytes != 0) {
                const auto fragmented = static_cast<long double>(snapshot.reusableSlackBytes - largestFreeRegion);
                const auto total = static_cast<long double>(snapshot.reusableSlackBytes);
                snapshot.externalFragmentationBasisPoints = static_cast<std::uint32_t>((fragmented * 10'000.0L) / total);
            }
            return snapshot;
        }

        RenderResourceOwnerId renderer_;
        RenderMemoryBudgetConfig config_;
        std::vector<Pool> pools_;
        std::vector<Block> blocks_;
        std::vector<Region> regions_;
        mutable SortedMemoryRegions sortedRegionsScratch_;
        std::size_t peakChargedBytes_{0};
        std::uint64_t failedReservationCount_{0};
        std::uint32_t reservationCount_{0};
        std::uint32_t allocationCount_{0};
        std::uint64_t nextPool_{1};
        std::uint64_t nextBlock_{1};
        std::uint64_t nextReservation_{1};
        std::uint64_t nextAllocation_{1};
        bool acceptingReservations_{true};
    };

    /** @copydoc RenderMemoryBudget::Create */
    Result<std::unique_ptr<RenderMemoryBudget>> RenderMemoryBudget::Create(const RenderResourceOwnerId renderer,
                                                                           const RenderMemoryBudgetConfig &config) {
        if (!renderer.IsValid() || !config.IsValid()) {
            return Result<std::unique_ptr<RenderMemoryBudget>>::Failure(
                MemoryBudgetError(RenderMemoryBudgetErrors::InvalidConfiguration,
                                  "Renderer owner identity or finite memory configuration is invalid."));
        }
        try {
            return Result<std::unique_ptr<RenderMemoryBudget>>::Success(
                std::unique_ptr<RenderMemoryBudget>{new RenderMemoryBudget{std::make_unique<Impl>(renderer, config)}});  // NOSONAR
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<RenderMemoryBudget>>::Failure(
                MemoryBudgetError(RenderMemoryBudgetErrors::CapacityExceeded,
                                  "Renderer memory metadata tables cannot be allocated within host memory."));
        } catch (const std::length_error &) {
            return Result<std::unique_ptr<RenderMemoryBudget>>::Failure(
                MemoryBudgetError(RenderMemoryBudgetErrors::CapacityExceeded,
                                  "Renderer memory metadata table limits exceed the platform container capacity."));
        }
    }

    RenderMemoryBudget::RenderMemoryBudget(std::unique_ptr<Impl> implementation) noexcept : implementation_(std::move(implementation)) {}

    /** @copydoc RenderMemoryBudget::~RenderMemoryBudget */
    RenderMemoryBudget::~RenderMemoryBudget() = default;

    /** @copydoc RenderMemoryBudget::Reserve */
    Result<RenderMemoryReservationId> RenderMemoryBudget::Reserve(const RenderMemoryScopeId scope, const ResourceOperationId attempt,
                                                                  const RenderMemoryCostPlan &plan) {
        return implementation_->Reserve(scope, attempt, plan);
    }

    /** @copydoc RenderMemoryBudget::Cancel */
    Result<void> RenderMemoryBudget::Cancel(const RenderMemoryReservationId reservation) {
        return implementation_->Cancel(reservation);
    }

    /** @copydoc RenderMemoryBudget::Placement */
    Result<RenderMemoryPlacement> RenderMemoryBudget::Placement(const RenderMemoryReservationId reservation) const {
        return implementation_->Placement(reservation);
    }

    /** @copydoc RenderMemoryBudget::Commit */
    Result<RenderMemoryAllocation> RenderMemoryBudget::Commit(const RenderMemoryReservationId reservation) {
        return implementation_->Commit(reservation);
    }

    /** @copydoc RenderMemoryBudget::BeginRetire */
    Result<void> RenderMemoryBudget::BeginRetire(const RenderMemoryAllocationId allocation) {
        return implementation_->BeginRetire(allocation);
    }

    /** @copydoc RenderMemoryBudget::AcknowledgeRetirement */
    Result<void> RenderMemoryBudget::AcknowledgeRetirement(const RenderMemoryAllocationId allocation) {
        return implementation_->AcknowledgeRetirement(allocation);
    }

    /** @copydoc RenderMemoryBudget::ReclaimEmptyBlocks */
    std::size_t RenderMemoryBudget::ReclaimEmptyBlocks(const std::size_t maxBlocks) noexcept {
        return implementation_->ReclaimEmptyBlocks(maxBlocks);
    }

    /** @copydoc RenderMemoryBudget::ReviseHardCap */
    Result<void> RenderMemoryBudget::ReviseHardCap(const std::size_t hardCapBytes, const std::uint64_t revision) {
        return implementation_->ReviseHardCap(hardCapBytes, revision);
    }

    /** @copydoc RenderMemoryBudget::Snapshot */
    RenderMemoryBudgetSnapshot RenderMemoryBudget::Snapshot() const noexcept {
        return implementation_->Snapshot();
    }

    /** @copydoc RenderMemoryBudget::PoolSnapshot */
    Result<RenderMemoryPoolSnapshot> RenderMemoryBudget::PoolSnapshot(const RenderMemoryPoolId pool) const {
        return implementation_->PoolSnapshot(pool);
    }

    /** @copydoc RenderMemoryBudget::Shutdown */
    void RenderMemoryBudget::Shutdown() noexcept {
        implementation_->Shutdown();
    }
}  // namespace Horo::Render
