#include "Horo/Vfx/EffectInstancePool.h"

#include "Horo/Vfx/VfxErrors.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <new>
#include <thread>
#include <utility>

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

        struct EffectInstancePoolState final {
            EffectPoolPlan plan{};
            EffectPoolPolicy policy{};
            VfxIdentityScope scene{};
            std::thread::id ownerThread{};
            std::unique_ptr<EffectPoolSlot[]> slots;
            std::unique_ptr<std::uint32_t[]> freeSlots;
            std::unique_ptr<DelayedEffectRequest[]> delayed;
            std::uint32_t freeCount{};
            std::uint32_t delayedHead{};
            std::uint32_t delayedCount{};
            std::uint32_t active{};
            std::uint32_t retiring{};
            std::uint32_t cosmeticOccupied{};
            std::uint32_t permanentlyRetired{};
            std::uint32_t peakActive{};
            std::uint64_t rejected{};
            std::uint64_t expired{};
            std::uint64_t cancelled{};
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

        [[nodiscard]] EffectPoolStatus ValidateHandle(const EffectInstancePoolState &state, const EffectSystemId instance,
                                                      EffectPoolSlot *&slot) noexcept {
            if (!instance.IsValid() || instance.scope != state.scene || instance.slot >= state.plan.capacity)
                return EffectPoolStatus::InvalidHandle;
            slot = &state.slots[instance.slot];
            if (instance.generation != slot->generation)
                return EffectPoolStatus::StaleHandle;
            if (slot->state == EffectSlotState::Free || slot->state == EffectSlotState::PermanentlyRetired)
                return EffectPoolStatus::InvalidHandle;
            return EffectPoolStatus::Admitted;
        }

        [[nodiscard]] EffectSystemId Identity(const EffectInstancePoolState &state, const std::uint32_t index) noexcept {
            return {state.scene, index, state.slots[index].generation};
        }

        [[nodiscard]] std::uint32_t AvailableSlot(const EffectInstancePoolState &state, const VfxRequirementClass requirement) noexcept {
            const std::uint32_t usableCapacity = state.plan.capacity - state.permanentlyRetired;
            if (requirement == VfxRequirementClass::Cosmetic &&
                (usableCapacity <= state.plan.requiredReserve || state.cosmeticOccupied >= usableCapacity - state.plan.requiredReserve))
                return state.plan.capacity;
            return state.freeCount == 0 ? state.plan.capacity : state.freeSlots[0];
        }

        [[nodiscard]] EffectPoolOutcome Admit(EffectInstancePoolState &state, const EffectPlaybackRequest &request,
                                              const std::uint64_t delayedTicket = 0) noexcept {
            const std::uint32_t index = AvailableSlot(state, request.requirement);
            if (index == state.plan.capacity)
                return {.status = EffectPoolStatus::Rejected};
            std::pop_heap(state.freeSlots.get(), state.freeSlots.get() + state.freeCount, std::greater<>{});
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
    }  // namespace

    EffectInstancePool::EffectInstancePool(std::unique_ptr<EffectInstancePoolState> state) noexcept : state_(std::move(state)) {}

    EffectInstancePool::EffectInstancePool(EffectInstancePool &&other) noexcept = default;
    EffectInstancePool &EffectInstancePool::operator=(EffectInstancePool &&other) noexcept = default;
    EffectInstancePool::~EffectInstancePool() = default;

    /** @copydoc EffectInstancePool::Prepare */
    Result<EffectInstancePool> EffectInstancePool::Prepare(const EffectPoolDescriptor &descriptor, const EffectPoolBudget &budget,
                                                           const EffectPoolPolicy &policy) {
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

        std::unique_ptr<EffectInstancePoolState> state{new (std::nothrow) EffectInstancePoolState{}};
        if (!state)
            return Result<EffectInstancePool>::Failure(MakeError(VfxErrors::EffectPoolAllocationFailed));
        state->slots.reset(new (std::nothrow) EffectPoolSlot[capacity]);
        if (!state->slots)
            return Result<EffectInstancePool>::Failure(MakeError(VfxErrors::EffectPoolAllocationFailed));
        state->freeSlots.reset(new (std::nothrow) std::uint32_t[capacity]);
        if (!state->freeSlots)
            return Result<EffectInstancePool>::Failure(MakeError(VfxErrors::EffectPoolAllocationFailed));
        state->freeCount = capacity;
        for (std::uint32_t index = 0; index < capacity; ++index)
            state->freeSlots[index] = index;
        std::make_heap(state->freeSlots.get(), state->freeSlots.get() + capacity, std::greater<>{});
        if (delayedCapacity != 0) {
            state->delayed.reset(new (std::nothrow) Detail::DelayedEffectRequest[delayedCapacity]);
            if (!state->delayed)
                return Result<EffectInstancePool>::Failure(MakeError(VfxErrors::EffectPoolAllocationFailed));
        }
        state->scene = descriptor.scene;
        state->ownerThread = std::this_thread::get_id();
        state->policy = policy;
        state->plan = {.capacity = capacity,
                       .requiredReserve = budget.requiredReserve,
                       .delayedCapacity = delayedCapacity,
                       .reservedEmitterSlots = static_cast<std::uint64_t>(capacity) * descriptor.emittersPerInstance,
                       .reservedBytes = fixedBytes + static_cast<std::uint64_t>(capacity) * bytesPerSlot};
        return Result<EffectInstancePool>::Success(EffectInstancePool{std::move(state)});
    }

    /** @copydoc EffectInstancePool::Play */
    EffectPoolOutcome EffectInstancePool::Play(const EffectPlaybackRequest &request) noexcept {
        if (!state_)
            return {.status = EffectPoolStatus::ShutDown};
        if (!OwnerThread(*state_))
            return {.status = EffectPoolStatus::ThreadViolation};
        if (state_->shutDown)
            return {.status = EffectPoolStatus::ShutDown};
        if (!request.owner.IsValid() || request.ownerGeneration == 0 || request.requirement >= VfxRequirementClass::Count ||
            request.tick < state_->currentTick)
            return {.status = EffectPoolStatus::InvalidRequest};
        state_->currentTick = request.tick;
        if (request.requirement == VfxRequirementClass::GameplayRequired || state_->delayedCount == 0) {
            auto admission = Admit(*state_, request);
            if (admission.status == EffectPoolStatus::Admitted)
                return admission;
        }
        if (request.requirement == VfxRequirementClass::Cosmetic && state_->policy.overBudget == EffectPoolOverBudgetPolicy::DelayBounded) {
            if (state_->delayedCount == state_->plan.delayedCapacity || state_->nextTicket == std::numeric_limits<std::uint64_t>::max()) {
                ++state_->rejected;
                return {.status = EffectPoolStatus::DelayQueueFull};
            }
            const auto tail = static_cast<std::uint32_t>((static_cast<std::uint64_t>(state_->delayedHead) + state_->delayedCount) %
                                                         state_->plan.delayedCapacity);
            const std::uint64_t ticket = state_->nextTicket++;
            state_->delayed[tail] = {.request = request, .ticket = ticket};
            ++state_->delayedCount;
            return {.status = EffectPoolStatus::Delayed, .delayedTicket = ticket};
        }
        ++state_->rejected;
        return {.status = EffectPoolStatus::Rejected};
    }

    /** @copydoc EffectInstancePool::PumpDelayed */
    EffectPoolOutcome EffectInstancePool::PumpDelayed(const std::uint64_t tick) noexcept {
        if (!state_)
            return {.status = EffectPoolStatus::ShutDown};
        if (!OwnerThread(*state_))
            return {.status = EffectPoolStatus::ThreadViolation};
        if (state_->shutDown)
            return {.status = EffectPoolStatus::ShutDown};
        if (tick < state_->currentTick)
            return {.status = EffectPoolStatus::InvalidRequest};
        state_->currentTick = tick;
        if (state_->delayedCount == 0)
            return {.status = EffectPoolStatus::Empty};
        const auto &front = state_->delayed[state_->delayedHead];
        const std::uint64_t ticket = front.ticket;
        if (front.cancelled) {
            PopDelayed(*state_);
            ++state_->cancelled;
            return {.status = EffectPoolStatus::Cancelled, .delayedTicket = ticket};
        }
        if (tick == front.request.tick)
            return {.status = EffectPoolStatus::Waiting, .delayedTicket = ticket};
        if (tick - front.request.tick >= state_->policy.maximumDelayTicks) {
            PopDelayed(*state_);
            ++state_->expired;
            return {.status = EffectPoolStatus::DelayExpired, .delayedTicket = ticket};
        }
        auto ready = front.request;
        ready.tick = tick;
        auto admitted = Admit(*state_, ready, ticket);
        if (admitted.status == EffectPoolStatus::Rejected)
            return {.status = EffectPoolStatus::Waiting, .delayedTicket = ticket};
        PopDelayed(*state_);
        return admitted;
    }

    /** @copydoc EffectInstancePool::Restart */
    EffectPoolOutcome EffectInstancePool::Restart(const EffectSystemId instance, const std::uint64_t tick) noexcept {
        if (!state_)
            return {.status = EffectPoolStatus::ShutDown};
        if (!OwnerThread(*state_))
            return {.status = EffectPoolStatus::ThreadViolation};
        if (state_->shutDown)
            return {.status = EffectPoolStatus::ShutDown};
        if (tick < state_->currentTick)
            return {.status = EffectPoolStatus::InvalidRequest};
        EffectPoolSlot *slot{};
        const auto status = ValidateHandle(*state_, instance, slot);
        if (status != EffectPoolStatus::Admitted)
            return {.status = status};
        if (slot->state != EffectSlotState::Active)
            return {.status = EffectPoolStatus::InvalidHandle};
        if (slot->retainedReaders != 0)
            return {.status = EffectPoolStatus::RetentionPending};
        if (slot->generation == std::numeric_limits<std::uint32_t>::max())
            return {.status = EffectPoolStatus::GenerationExhausted};
        state_->currentTick = tick;
        ++slot->generation;
        slot->admittedTick = tick;
        return {.status = EffectPoolStatus::Admitted, .instance = Identity(*state_, instance.slot)};
    }

    /** @copydoc EffectInstancePool::Stop */
    EffectPoolStatus EffectInstancePool::Stop(const EffectSystemId instance) noexcept {
        if (!state_)
            return EffectPoolStatus::ShutDown;
        if (!OwnerThread(*state_))
            return EffectPoolStatus::ThreadViolation;
        EffectPoolSlot *slot{};
        const auto status = ValidateHandle(*state_, instance, slot);
        if (status != EffectPoolStatus::Admitted)
            return status;
        if (slot->state != EffectSlotState::Active)
            return EffectPoolStatus::InvalidHandle;
        StopSlot(*state_, *slot);
        return EffectPoolStatus::Admitted;
    }

    /** @copydoc EffectInstancePool::Retain */
    EffectPoolStatus EffectInstancePool::Retain(const EffectSystemId instance) noexcept {
        if (!state_)
            return EffectPoolStatus::ShutDown;
        if (!OwnerThread(*state_))
            return EffectPoolStatus::ThreadViolation;
        if (state_->shutDown)
            return EffectPoolStatus::ShutDown;
        EffectPoolSlot *slot{};
        const auto status = ValidateHandle(*state_, instance, slot);
        if (status != EffectPoolStatus::Admitted)
            return status;
        if (slot->state != EffectSlotState::Active || slot->retainedReaders == std::numeric_limits<std::uint32_t>::max())
            return EffectPoolStatus::InvalidRequest;
        ++slot->retainedReaders;
        return EffectPoolStatus::Admitted;
    }

    /** @copydoc EffectInstancePool::Acknowledge */
    EffectPoolStatus EffectInstancePool::Acknowledge(const EffectSystemId instance) noexcept {
        if (!state_)
            return EffectPoolStatus::ShutDown;
        if (!OwnerThread(*state_))
            return EffectPoolStatus::ThreadViolation;
        EffectPoolSlot *slot{};
        const auto status = ValidateHandle(*state_, instance, slot);
        if (status != EffectPoolStatus::Admitted)
            return status;
        if (slot->retainedReaders == 0)
            return EffectPoolStatus::InvalidRequest;
        --slot->retainedReaders;
        return EffectPoolStatus::Admitted;
    }

    /** @copydoc EffectInstancePool::CompleteRetirement */
    EffectPoolStatus EffectInstancePool::CompleteRetirement(const EffectSystemId instance) noexcept {
        if (!state_)
            return EffectPoolStatus::ShutDown;
        if (!OwnerThread(*state_))
            return EffectPoolStatus::ThreadViolation;
        EffectPoolSlot *slot{};
        const auto status = ValidateHandle(*state_, instance, slot);
        if (status != EffectPoolStatus::Admitted)
            return status;
        if (slot->state != EffectSlotState::Retiring)
            return EffectPoolStatus::InvalidRequest;
        if (slot->retainedReaders != 0)
            return EffectPoolStatus::RetentionPending;
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
            std::push_heap(state_->freeSlots.get(), state_->freeSlots.get() + state_->freeCount, std::greater<>{});
        }
        return EffectPoolStatus::Admitted;
    }

    /** @copydoc EffectInstancePool::CancelOwner */
    EffectPoolCancelOutcome EffectInstancePool::CancelOwner(const VfxIdentityScope owner, const std::uint64_t ownerGeneration) noexcept {
        if (!state_)
            return {.status = EffectPoolStatus::ShutDown};
        if (!OwnerThread(*state_))
            return {.status = EffectPoolStatus::ThreadViolation};
        if (state_->shutDown)
            return {.status = EffectPoolStatus::ShutDown};
        if (!owner.IsValid() || ownerGeneration == 0)
            return {.status = EffectPoolStatus::InvalidRequest};
        EffectPoolCancelOutcome outcome{.status = EffectPoolStatus::Cancelled};
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
        if (!state_)
            return EffectPoolStatus::ShutDown;
        if (!OwnerThread(*state_))
            return EffectPoolStatus::ThreadViolation;
        if (state_->shutDown)
            return EffectPoolStatus::Admitted;
        state_->shutDown = true;
        state_->cancelled += state_->delayedCount;
        state_->delayedCount = 0;
        for (std::uint32_t index = 0; index < state_->plan.capacity; ++index) {
            auto &slot = state_->slots[index];
            if (slot.state == EffectSlotState::Active)
                StopSlot(*state_, slot);
        }
        return EffectPoolStatus::Admitted;
    }

    /** @copydoc EffectInstancePool::Inspect */
    EffectPoolStatus EffectInstancePool::Inspect(const EffectSystemId instance, EffectInstanceSnapshot &output) const noexcept {
        if (!state_)
            return EffectPoolStatus::ShutDown;
        if (!OwnerThread(*state_))
            return EffectPoolStatus::ThreadViolation;
        EffectPoolSlot *slot{};
        const auto status = ValidateHandle(*state_, instance, slot);
        if (status != EffectPoolStatus::Admitted)
            return status;
        output = {.instance = instance,
                  .owner = slot->owner,
                  .ownerGeneration = slot->ownerGeneration,
                  .admittedTick = slot->admittedTick,
                  .requirement = slot->requirement,
                  .retainedReaders = slot->retainedReaders,
                  .retiring = slot->state == EffectSlotState::Retiring};
        return EffectPoolStatus::Admitted;
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
                .rejected = state_->rejected,
                .expired = state_->expired,
                .cancelled = state_->cancelled};
    }

    /** @copydoc EffectInstancePool::Quiescent */
    bool EffectInstancePool::Quiescent() const noexcept {
        return !state_ || (OwnerThread(*state_) && state_->active == 0 && state_->retiring == 0 && state_->delayedCount == 0);
    }
}  // namespace Horo::Vfx
