#include "Horo/Vfx/EffectInstancePool.h"

#include "Horo/Vfx/VfxErrors.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace Horo::Vfx {
    namespace Detail {
        enum class EffectSlotState : std::uint8_t {
            Free,
            Active,
            Retiring,
            PermanentlyRetired
        };

        struct EffectPoolSlot final {
            VfxIdentityScope owner{};
            std::uint64_t ownerGeneration{};
            std::uint64_t admittedTick{};
            std::uint32_t generation{1};
            std::uint32_t retainedReaders{};
            VfxRequirementClass requirement{VfxRequirementClass::Cosmetic};
            EffectSlotState state{EffectSlotState::Free};
        };

        struct DelayedEffectRequest final {
            EffectPlaybackRequest request{};
            std::uint64_t ticket{};
            bool cancelled{};
        };

        /** @brief Lifetime overload totals independent of current slot occupancy. */
        struct EffectPoolTelemetry final {
            std::uint64_t rejected{};
            std::uint64_t expired{};
            std::uint64_t cancelled{};
        };

        struct EffectInstancePoolState final {
            /** @brief Keeps MSVC vector proxy allocations in a throwing constructor that preparation can catch. */
            EffectInstancePoolState(const std::uint32_t capacity, const std::uint32_t delayedCapacity)
                : slots(capacity), freeSlots(capacity), delayed(delayedCapacity) {}

            EffectPoolPlan plan{};
            EffectPoolPolicy policy{};
            VfxIdentityScope scene{};
            std::thread::id ownerThread{};
            std::vector<EffectPoolSlot> slots;
            std::vector<std::uint32_t> freeSlots;
            std::vector<DelayedEffectRequest> delayed;
            std::uint32_t freeCount{};
            std::uint32_t delayedHead{};
            std::uint32_t delayedCount{};
            std::uint32_t active{};
            std::uint32_t retiring{};
            std::uint32_t cosmeticOccupied{};
            std::uint32_t permanentlyRetired{};
            std::uint32_t peakActive{};
            EffectPoolTelemetry telemetry{};
            std::uint64_t currentTick{};
            std::uint64_t nextTicket{1};
            bool shutDown{};
        };
    }  // namespace Detail

    namespace {
        using Detail::EffectInstancePoolState;
        using Detail::EffectPoolSlot;
        using Detail::EffectSlotState;

        [[nodiscard]] bool OwnerThread(const EffectInstancePoolState &state) noexcept {
            return std::this_thread::get_id() == state.ownerThread;
        }

        /** @brief Resolves a generation-safe slot without discarding state constness. */
        template <typename State, typename Slot>
        [[nodiscard]] EffectPoolStatus ValidateHandle(State &state, const EffectSystemId instance, Slot *&slot) noexcept {
            using enum EffectPoolStatus;
            if (!instance.IsValid() || instance.scope != state.scene || instance.slot >= state.plan.capacity)
                return InvalidHandle;
            slot = &state.slots[instance.slot];
            if (instance.generation != slot->generation)
                return StaleHandle;
            if (slot->state == EffectSlotState::Free || slot->state == EffectSlotState::PermanentlyRetired)
                return InvalidHandle;
            return Admitted;
        }

        [[nodiscard]] EffectSystemId Identity(const EffectInstancePoolState &state, const std::uint32_t index) noexcept {
            return {state.scene, index, state.slots[index].generation};
        }

        [[nodiscard]] std::uint32_t AvailableSlot(const EffectInstancePoolState &state, const VfxRequirementClass requirement) noexcept {
            if (const std::uint32_t usableCapacity = state.plan.capacity - state.permanentlyRetired;
                requirement == VfxRequirementClass::Cosmetic &&
                (usableCapacity <= state.plan.requiredReserve || state.cosmeticOccupied >= usableCapacity - state.plan.requiredReserve))
                return state.plan.capacity;
            return state.freeCount == 0 ? state.plan.capacity : state.freeSlots[0];
        }

        [[nodiscard]] EffectPoolOutcome Admit(EffectInstancePoolState &state, const EffectPlaybackRequest &request,
                                              const std::uint64_t delayedTicket = 0) noexcept {
            const std::uint32_t index = AvailableSlot(state, request.requirement);
            if (index == state.plan.capacity)
                return {.status = EffectPoolStatus::Rejected};
            std::pop_heap(state.freeSlots.data(), state.freeSlots.data() + state.freeCount, std::greater<>{});
            --state.freeCount;
            EffectPoolSlot &slot = state.slots[index];
            slot.owner = request.owner;
            slot.ownerGeneration = request.ownerGeneration;
            slot.admittedTick = request.tick;
            slot.requirement = request.requirement;
            slot.state = EffectSlotState::Active;
            ++state.active;
            if (request.requirement == VfxRequirementClass::Cosmetic)
                ++state.cosmeticOccupied;
            state.peakActive = std::max(state.peakActive, state.active);
            return {.status = EffectPoolStatus::Admitted, .instance = Identity(state, index), .delayedTicket = delayedTicket};
        }

        void StopSlot(EffectInstancePoolState &state, EffectPoolSlot &slot) noexcept {
            slot.state = EffectSlotState::Retiring;
            --state.active;
            ++state.retiring;
        }

        /** @brief Adds one dependent lease to a live slot without releasing its reservation. */
        [[nodiscard]] EffectPoolStatus RetainSlot(EffectPoolSlot &slot) noexcept {
            using enum EffectPoolStatus;
            if (slot.state != EffectSlotState::Active || slot.retainedReaders == std::numeric_limits<std::uint32_t>::max())
                return InvalidRequest;
            ++slot.retainedReaders;
            return Admitted;
        }

        /** @brief Releases one dependent lease, including while the slot is retiring. */
        [[nodiscard]] EffectPoolStatus AcknowledgeSlot(EffectPoolSlot &slot) noexcept {
            using enum EffectPoolStatus;
            if (slot.retainedReaders == 0)
                return InvalidRequest;
            --slot.retainedReaders;
            return Admitted;
        }

        void PopDelayed(EffectInstancePoolState &state) noexcept {
            state.delayedHead =
                static_cast<std::uint32_t>((static_cast<std::uint64_t>(state.delayedHead) + 1U) % state.plan.delayedCapacity);
            --state.delayedCount;
        }

        [[nodiscard]] bool ValidPlanInputs(const EffectPoolDescriptor &descriptor, const EffectPoolBudget &budget,
                                           const EffectPoolPolicy &policy) noexcept {
            if (!descriptor.scene.IsValid() || descriptor.maximumInstances == 0 || descriptor.emittersPerInstance == 0 ||
                descriptor.bytesPerInstance == 0 || budget.maximumInstances == 0 || budget.maximumEmitterSlots == 0 ||
                budget.maximumBytes == 0 || policy.overBudget >= EffectPoolOverBudgetPolicy::Count)
                return false;
            if (policy.overBudget == EffectPoolOverBudgetPolicy::RejectNewest)
                return policy.maximumDelayTicks == 0;
            return budget.maximumDelayed > 0 && policy.maximumDelayTicks > 0;
        }

        enum class StoragePreparationStatus : std::uint8_t {
            Ready,
            Invalid,
            AllocationFailed
        };

        /** @brief Allocates contiguous fixed-capacity storage and charges any vector over-reservation. */
        [[nodiscard]] StoragePreparationStatus PrepareStorage(const std::uint32_t capacity, const std::uint32_t delayedCapacity,
                                                              const std::uint64_t maximumBytes, const std::uint64_t plannedBytes,
                                                              std::unique_ptr<EffectInstancePoolState> &state,
                                                              std::uint64_t &reservedBytes) noexcept {
            using enum StoragePreparationStatus;
            if (plannedBytes > maximumBytes)
                return Invalid;
            std::unique_ptr<EffectInstancePoolState> prepared;
            constexpr std::uint64_t maxVectorIndex = static_cast<std::uint64_t>(std::numeric_limits<std::ptrdiff_t>::max());
            if (capacity > maxVectorIndex / sizeof(EffectPoolSlot) || capacity > maxVectorIndex / sizeof(std::uint32_t) ||
                delayedCapacity > maxVectorIndex / sizeof(Detail::DelayedEffectRequest) ||
                capacity > std::numeric_limits<std::size_t>::max() / sizeof(EffectPoolSlot) ||
                capacity > std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t) ||
                delayedCapacity > std::numeric_limits<std::size_t>::max() / sizeof(Detail::DelayedEffectRequest))
                return Invalid;
            try {
                prepared = std::make_unique<EffectInstancePoolState>(capacity, delayedCapacity);
            } catch (const std::bad_alloc &) {
                return AllocationFailed;
            } catch (const std::length_error &) {
                return Invalid;
            }
            std::uint64_t remainingBytes = maximumBytes - plannedBytes;
            if (const auto chargeExtra =
                    [&remainingBytes](const std::size_t elements, const std::size_t elementBytes) noexcept {
                if (elements > remainingBytes / elementBytes)
                    return false;
                remainingBytes -= static_cast<std::uint64_t>(elements) * elementBytes;
                return true;
            };
                !chargeExtra(prepared->slots.capacity() - capacity, sizeof(EffectPoolSlot)) ||
                !chargeExtra(prepared->freeSlots.capacity() - capacity, sizeof(std::uint32_t)) ||
                !chargeExtra(prepared->delayed.capacity() - delayedCapacity, sizeof(Detail::DelayedEffectRequest)))
                return Invalid;
            reservedBytes = maximumBytes - remainingBytes;
            state = std::move(prepared);
            return Ready;
        }
    }  // namespace

    EffectInstancePool::EffectInstancePool(std::unique_ptr<EffectInstancePoolState> state) noexcept : state_(std::move(state)) {}

    EffectInstancePool::EffectInstancePool(EffectInstancePool &&other) noexcept = default;
    EffectInstancePool &EffectInstancePool::operator=(EffectInstancePool &&other) noexcept = default;
    EffectInstancePool::~EffectInstancePool() = default;

    /** @copydoc EffectInstancePool::Prepare */
    Result<EffectInstancePool> EffectInstancePool::Prepare(const EffectPoolDescriptor &descriptor, const EffectPoolBudget &budget,
                                                           const EffectPoolPolicy &policy) {
        using enum StoragePreparationStatus;
        if (!ValidPlanInputs(descriptor, budget, policy))
            return Result<EffectInstancePool>::Failure(MakeError(VfxErrors::EffectPoolInvalid));
        const std::uint32_t delayedCapacity = policy.overBudget == EffectPoolOverBudgetPolicy::DelayBounded ? budget.maximumDelayed : 0;
        constexpr std::uint64_t slotControlBytes = sizeof(EffectPoolSlot) + sizeof(std::uint32_t);
        if (delayedCapacity > std::numeric_limits<std::size_t>::max() / sizeof(Detail::DelayedEffectRequest) ||
            descriptor.bytesPerInstance > std::numeric_limits<std::uint64_t>::max() - slotControlBytes)
            return Result<EffectInstancePool>::Failure(MakeError(VfxErrors::EffectPoolInvalid));
        const std::uint64_t fixedBytes =
            sizeof(EffectInstancePoolState) + static_cast<std::uint64_t>(delayedCapacity) * sizeof(Detail::DelayedEffectRequest);
        const std::uint64_t bytesPerSlot = descriptor.bytesPerInstance + slotControlBytes;
        if (fixedBytes >= budget.maximumBytes)
            return Result<EffectInstancePool>::Failure(MakeError(VfxErrors::EffectPoolInvalid));
        const auto capacity = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            {descriptor.maximumInstances, budget.maximumInstances, budget.maximumEmitterSlots / descriptor.emittersPerInstance,
             (budget.maximumBytes - fixedBytes) / bytesPerSlot}));
        if (capacity == 0 || budget.requiredReserve > capacity ||
            capacity > std::numeric_limits<std::size_t>::max() / sizeof(EffectPoolSlot) ||
            capacity > std::numeric_limits<std::size_t>::max() / sizeof(std::uint32_t))
            return Result<EffectInstancePool>::Failure(MakeError(VfxErrors::EffectPoolInvalid));

        std::unique_ptr<EffectInstancePoolState> state;
        std::uint64_t reservedBytes{};
        const std::uint64_t plannedBytes = fixedBytes + static_cast<std::uint64_t>(capacity) * bytesPerSlot;
        const auto storageStatus = PrepareStorage(capacity, delayedCapacity, budget.maximumBytes, plannedBytes, state, reservedBytes);
        if (storageStatus == AllocationFailed)
            return Result<EffectInstancePool>::Failure(MakeError(VfxErrors::EffectPoolAllocationFailed));
        if (storageStatus != Ready)
            return Result<EffectInstancePool>::Failure(MakeError(VfxErrors::EffectPoolInvalid));
        state->freeCount = capacity;
        for (std::uint32_t index = 0; index < capacity; ++index)
            state->freeSlots[index] = index;
        std::make_heap(state->freeSlots.data(), state->freeSlots.data() + capacity, std::greater<>{});
        state->scene = descriptor.scene;
        state->ownerThread = std::this_thread::get_id();
        state->policy = policy;
        state->plan = {.capacity = capacity,
                       .requiredReserve = budget.requiredReserve,
                       .delayedCapacity = delayedCapacity,
                       .reservedEmitterSlots = static_cast<std::uint64_t>(capacity) * descriptor.emittersPerInstance,
                       .reservedBytes = reservedBytes};
        return Result<EffectInstancePool>::Success(EffectInstancePool{std::move(state)});
    }

    /** @copydoc EffectInstancePool::Play */
    EffectPoolOutcome EffectInstancePool::Play(const EffectPlaybackRequest &request) noexcept {
        using enum EffectPoolStatus;
        if (!state_)
            return {.status = ShutDown};
        if (!OwnerThread(*state_))
            return {.status = ThreadViolation};
        if (state_->shutDown)
            return {.status = ShutDown};
        if (!request.owner.IsValid() || request.ownerGeneration == 0 || request.requirement >= VfxRequirementClass::Count ||
            request.tick < state_->currentTick)
            return {.status = InvalidRequest};
        state_->currentTick = request.tick;
        if (request.requirement == VfxRequirementClass::GameplayRequired || state_->delayedCount == 0) {
            auto admission = Admit(*state_, request);
            if (admission.status == Admitted)
                return admission;
        }
        if (request.requirement == VfxRequirementClass::Cosmetic && state_->policy.overBudget == EffectPoolOverBudgetPolicy::DelayBounded) {
            if (state_->delayedCount == state_->plan.delayedCapacity || state_->nextTicket == std::numeric_limits<std::uint64_t>::max()) {
                ++state_->telemetry.rejected;
                return {.status = DelayQueueFull};
            }
            const auto tail = static_cast<std::uint32_t>((static_cast<std::uint64_t>(state_->delayedHead) + state_->delayedCount) %
                                                         state_->plan.delayedCapacity);
            const std::uint64_t ticket = state_->nextTicket++;
            state_->delayed[tail] = {.request = request, .ticket = ticket};
            ++state_->delayedCount;
            return {.status = Delayed, .delayedTicket = ticket};
        }
        ++state_->telemetry.rejected;
        return {.status = Rejected};
    }

    /** @copydoc EffectInstancePool::PumpDelayed */
    EffectPoolOutcome EffectInstancePool::PumpDelayed(const std::uint64_t tick) noexcept {
        using enum EffectPoolStatus;
        if (!state_)
            return {.status = ShutDown};
        if (!OwnerThread(*state_))
            return {.status = ThreadViolation};
        if (state_->shutDown)
            return {.status = ShutDown};
        if (tick < state_->currentTick)
            return {.status = InvalidRequest};
        state_->currentTick = tick;
        if (state_->delayedCount == 0)
            return {.status = Empty};
        const auto &front = state_->delayed[state_->delayedHead];
        const std::uint64_t ticket = front.ticket;
        if (front.cancelled) {
            PopDelayed(*state_);
            ++state_->telemetry.cancelled;
            return {.status = Cancelled, .delayedTicket = ticket};
        }
        if (tick == front.request.tick)
            return {.status = Waiting, .delayedTicket = ticket};
        if (tick - front.request.tick >= state_->policy.maximumDelayTicks) {
            PopDelayed(*state_);
            ++state_->telemetry.expired;
            return {.status = DelayExpired, .delayedTicket = ticket};
        }
        auto ready = front.request;
        ready.tick = tick;
        auto admitted = Admit(*state_, ready, ticket);
        if (admitted.status == Rejected)
            return {.status = Waiting, .delayedTicket = ticket};
        PopDelayed(*state_);
        return admitted;
    }

    /** @copydoc EffectInstancePool::Restart */
    EffectPoolOutcome EffectInstancePool::Restart(const EffectSystemId instance, const std::uint64_t tick) noexcept {
        using enum EffectPoolStatus;
        if (!state_)
            return {.status = ShutDown};
        if (!OwnerThread(*state_))
            return {.status = ThreadViolation};
        if (state_->shutDown)
            return {.status = ShutDown};
        if (tick < state_->currentTick)
            return {.status = InvalidRequest};
        EffectPoolSlot *slot{};
        if (const auto status = ValidateHandle(*state_, instance, slot); status != Admitted)
            return {.status = status};
        if (slot->state != EffectSlotState::Active)
            return {.status = InvalidHandle};
        if (slot->retainedReaders != 0)
            return {.status = RetentionPending};
        if (slot->generation == std::numeric_limits<std::uint32_t>::max())
            return {.status = GenerationExhausted};
        state_->currentTick = tick;
        ++slot->generation;
        slot->admittedTick = tick;
        return {.status = Admitted, .instance = Identity(*state_, instance.slot)};
    }

    /** @copydoc EffectInstancePool::Stop */
    EffectPoolStatus EffectInstancePool::Stop(const EffectSystemId instance) noexcept {
        using enum EffectPoolStatus;
        if (!state_)
            return ShutDown;
        if (!OwnerThread(*state_))
            return ThreadViolation;
        EffectPoolSlot *slot{};
        if (const auto status = ValidateHandle(*state_, instance, slot); status != Admitted)
            return status;
        if (slot->state != EffectSlotState::Active)
            return InvalidHandle;
        StopSlot(*state_, *slot);
        return Admitted;
    }

    /** @copydoc EffectInstancePool::Retain */
    EffectPoolStatus EffectInstancePool::Retain(const EffectSystemId instance) noexcept {
        using enum EffectPoolStatus;
        if (!state_)
            return ShutDown;
        if (!OwnerThread(*state_))
            return ThreadViolation;
        if (state_->shutDown)
            return ShutDown;
        EffectPoolSlot *slot{};
        if (const auto status = ValidateHandle(*state_, instance, slot); status != Admitted)
            return status;
        return RetainSlot(*slot);
    }

    /** @copydoc EffectInstancePool::Acknowledge */
    EffectPoolStatus EffectInstancePool::Acknowledge(const EffectSystemId instance) noexcept {
        using enum EffectPoolStatus;
        if (!state_)
            return ShutDown;
        if (!OwnerThread(*state_))
            return ThreadViolation;
        EffectPoolSlot *slot{};
        if (const auto status = ValidateHandle(*state_, instance, slot); status != Admitted)
            return status;
        return AcknowledgeSlot(*slot);
    }

    /** @copydoc EffectInstancePool::CompleteRetirement */
    EffectPoolStatus EffectInstancePool::CompleteRetirement(const EffectSystemId instance) noexcept {
        using enum EffectPoolStatus;
        if (!state_)
            return ShutDown;
        if (!OwnerThread(*state_))
            return ThreadViolation;
        EffectPoolSlot *slot{};
        if (const auto status = ValidateHandle(*state_, instance, slot); status != Admitted)
            return status;
        if (slot->state != EffectSlotState::Retiring)
            return InvalidRequest;
        if (slot->retainedReaders != 0)
            return RetentionPending;
        --state_->retiring;
        if (slot->requirement == VfxRequirementClass::Cosmetic)
            --state_->cosmeticOccupied;
        slot->owner = {};
        slot->ownerGeneration = 0;
        slot->admittedTick = 0;
        if (slot->generation == std::numeric_limits<std::uint32_t>::max()) {
            slot->state = EffectSlotState::PermanentlyRetired;
            ++state_->permanentlyRetired;
        } else {
            ++slot->generation;
            slot->state = EffectSlotState::Free;
            state_->freeSlots[state_->freeCount++] = instance.slot;
            std::push_heap(state_->freeSlots.data(), state_->freeSlots.data() + state_->freeCount, std::greater<>{});
        }
        return Admitted;
    }

    /** @copydoc EffectInstancePool::CancelOwner */
    EffectPoolCancelOutcome EffectInstancePool::CancelOwner(const VfxIdentityScope owner, const std::uint64_t ownerGeneration) noexcept {
        using enum EffectPoolStatus;
        if (!state_)
            return {.status = ShutDown};
        if (!OwnerThread(*state_))
            return {.status = ThreadViolation};
        if (state_->shutDown)
            return {.status = ShutDown};
        if (!owner.IsValid() || ownerGeneration == 0)
            return {.status = InvalidRequest};
        EffectPoolCancelOutcome outcome{.status = Cancelled};
        for (std::uint32_t index = 0; index < state_->plan.capacity; ++index) {
            auto &slot = state_->slots[index];
            if (slot.state == EffectSlotState::Active && slot.owner == owner && slot.ownerGeneration == ownerGeneration) {
                StopSlot(*state_, slot);
                ++outcome.stopped;
            }
        }
        for (std::uint32_t offset = 0; offset < state_->delayedCount; ++offset) {
            const auto index =
                static_cast<std::uint32_t>((static_cast<std::uint64_t>(state_->delayedHead) + offset) % state_->plan.delayedCapacity);
            auto &record = state_->delayed[index];
            if (!record.cancelled && record.request.owner == owner && record.request.ownerGeneration == ownerGeneration) {
                record.cancelled = true;
                ++outcome.cancelledDelayed;
            }
        }
        return outcome;
    }

    /** @copydoc EffectInstancePool::Shutdown */
    EffectPoolStatus EffectInstancePool::Shutdown() noexcept {
        using enum EffectPoolStatus;
        if (!state_)
            return ShutDown;
        if (!OwnerThread(*state_))
            return ThreadViolation;
        if (state_->shutDown)
            return Admitted;
        state_->shutDown = true;
        state_->telemetry.cancelled += state_->delayedCount;
        state_->delayedCount = 0;
        for (std::uint32_t index = 0; index < state_->plan.capacity; ++index) {
            auto &slot = state_->slots[index];
            if (slot.state == EffectSlotState::Active)
                StopSlot(*state_, slot);
        }
        return Admitted;
    }

    /** @copydoc EffectInstancePool::Inspect */
    EffectPoolStatus EffectInstancePool::Inspect(const EffectSystemId instance, EffectInstanceSnapshot &output) const noexcept {
        using enum EffectPoolStatus;
        if (!state_)
            return ShutDown;
        if (!OwnerThread(*state_))
            return ThreadViolation;
        const EffectPoolSlot *slot{};
        if (const auto status = ValidateHandle(*state_, instance, slot); status != Admitted)
            return status;
        output = {.instance = instance,
                  .owner = slot->owner,
                  .ownerGeneration = slot->ownerGeneration,
                  .admittedTick = slot->admittedTick,
                  .requirement = slot->requirement,
                  .retainedReaders = slot->retainedReaders,
                  .retiring = slot->state == EffectSlotState::Retiring};
        return Admitted;
    }

    /** @copydoc EffectInstancePool::Statistics */
    EffectPoolStatistics EffectInstancePool::Statistics() const noexcept {
        if (!state_ || !OwnerThread(*state_))
            return {};
        return {.plan = state_->plan,
                .active = state_->active,
                .retiring = state_->retiring,
                .permanentlyRetired = state_->permanentlyRetired,
                .delayed = state_->delayedCount,
                .peakActive = state_->peakActive,
                .rejected = state_->telemetry.rejected,
                .expired = state_->telemetry.expired,
                .cancelled = state_->telemetry.cancelled};
    }

    /** @copydoc EffectInstancePool::Quiescent */
    bool EffectInstancePool::Quiescent() const noexcept {
        return !state_ || (OwnerThread(*state_) && state_->active == 0 && state_->retiring == 0 && state_->delayedCount == 0);
    }
}  // namespace Horo::Vfx
