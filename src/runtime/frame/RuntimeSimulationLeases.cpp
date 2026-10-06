#include "../lifecycle/RuntimeErrors.h"
#include "Horo/Runtime/RuntimeSimulationTiming.h"
#include "internal/RuntimeSimulationTimingStorage.h"

#include <utility>

namespace Horo::Runtime {
    /** @copydoc RuntimeSimulationPolicyRead::RuntimeSimulationPolicyRead */
    RuntimeSimulationPolicyRead::RuntimeSimulationPolicyRead(std::shared_ptr<const SimulationTimingDetail::Storage> owner,
                                                             const RuntimeSimulationPolicy policy) noexcept
        : owner_(std::move(owner)), policy_(policy) {}

    /** @copydoc RuntimeSimulationPauseLease::RuntimeSimulationPauseLease */
    RuntimeSimulationPauseLease::RuntimeSimulationPauseLease(std::shared_ptr<SimulationTimingDetail::Storage> owner,
                                                             const std::uint32_t slot, const std::uint64_t generation) noexcept
        : owner_(std::move(owner)), slot_(slot), generation_(generation) {}

    RuntimeSimulationPauseLease::~RuntimeSimulationPauseLease() {
        Release();
    }

    RuntimeSimulationPauseLease::RuntimeSimulationPauseLease(RuntimeSimulationPauseLease &&other) noexcept
        : owner_(std::move(other.owner_)), slot_(other.slot_), generation_(other.generation_) {}

    RuntimeSimulationPauseLease &RuntimeSimulationPauseLease::operator=(RuntimeSimulationPauseLease &&other) noexcept {
        if (this != &other) {
            Release();
            owner_ = std::move(other.owner_);
            slot_ = other.slot_;
            generation_ = other.generation_;
        }
        return *this;
    }

    /** @copydoc RuntimeSimulationPauseLease::Release */
    void RuntimeSimulationPauseLease::Release() noexcept {
        if (!owner_)
            return;
        // This pin excludes slot reuse until the signal; no owner state or callback is touched by the releasing thread.
        auto &record = owner_->pauses[slot_];
        if (record.generation == generation_)
            record.released.store(true, std::memory_order_release);
        owner_.reset();
    }

    /** @copydoc RuntimeSimulationPauseLease::IsValid */
    bool RuntimeSimulationPauseLease::IsValid() const noexcept {
        return owner_ != nullptr;
    }

    /** @copydoc RuntimeSingleStepReceipt::RuntimeSingleStepReceipt */
    RuntimeSingleStepReceipt::RuntimeSingleStepReceipt(std::shared_ptr<SimulationTimingDetail::Storage> owner, const std::uint32_t slot,
                                                       const std::uint64_t generation) noexcept
        : owner_(std::move(owner)), slot_(slot), generation_(generation) {}

    RuntimeSingleStepReceipt::~RuntimeSingleStepReceipt() {
        Release();
    }

    RuntimeSingleStepReceipt::RuntimeSingleStepReceipt(RuntimeSingleStepReceipt &&other) noexcept
        : owner_(std::move(other.owner_)), slot_(other.slot_), generation_(other.generation_) {}

    RuntimeSingleStepReceipt &RuntimeSingleStepReceipt::operator=(RuntimeSingleStepReceipt &&other) noexcept {
        if (this != &other) {
            Release();
            owner_ = std::move(other.owner_);
            slot_ = other.slot_;
            generation_ = other.generation_;
        }
        return *this;
    }

    /** @copydoc RuntimeSingleStepReceipt::ResultValue */
    Result<RuntimeSingleStepResult> RuntimeSingleStepReceipt::ResultValue() const {
        if (!owner_ || owner_->owner != std::this_thread::get_id())
            return Result<RuntimeSingleStepResult>::Failure(MakeError(RuntimeErrors::SimulationTimingInvalid));
        const auto &record = owner_->steps[slot_];
        if (!record.occupied || record.generation != generation_)
            return Result<RuntimeSingleStepResult>::Failure(MakeError(RuntimeErrors::SimulationTimingStale));
        return Result<RuntimeSingleStepResult>::Success(record.result);
    }

    /** @copydoc RuntimeSingleStepReceipt::Release */
    void RuntimeSingleStepReceipt::Release() noexcept {
        if (!owner_)
            return;
        auto &record = owner_->steps[slot_];
        if (record.generation == generation_)
            record.released.store(true, std::memory_order_release);
        owner_.reset();
    }

    SimulationTimingDetail::Storage *RuntimeSimulationControl::OwnedState::Mutable() noexcept {
        return pin.get();
    }

    const SimulationTimingDetail::Storage *RuntimeSimulationControl::OwnedState::Borrow() const noexcept {
        return pin.get();
    }

    std::shared_ptr<const SimulationTimingDetail::Storage> RuntimeSimulationControl::OwnedState::ReadPin() const noexcept {
        return pin;
    }

    std::shared_ptr<SimulationTimingDetail::Storage> &RuntimeSimulationControl::OwnedState::PublisherPin() noexcept {
        return pin;
    }

    RuntimeSimulationControl::~RuntimeSimulationControl() {
        Close();
    }
}  // namespace Horo::Runtime
