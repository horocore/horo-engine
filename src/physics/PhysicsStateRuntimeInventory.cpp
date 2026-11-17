#include "PhysicsStateRuntimeInventory.h"

#include <algorithm>
#include <exception>
#include <memory>
#include <new>

namespace Horo::Physics::Detail {
    /** @brief Only actual owner collection may release native shape/code ownership;
     * foreign snapshot release cannot reach this destructor.
     */
    PhysicsStateResourceRecord::~PhysicsStateResourceRecord() {
        if (ownerThread != std::this_thread::get_id())
            std::terminate();
        native.reset();
    }

    /** @copydoc PhysicsStateRuntimeInventory::Prepare */
    Result<std::unique_ptr<PhysicsStateRuntimeInventory>> PhysicsStateRuntimeInventory::Prepare(const PhysicsStateRuntimeBudget &budget,
                                                                                                std::size_t additionalFixedPayload) {
        using Outcome = Result<std::unique_ptr<PhysicsStateRuntimeInventory>>;
        if (budget.maximumCaptureOwners == 0 || budget.maximumCaptureOwners > MaximumPhysicsBufferEntries ||
            budget.maximumRetainedBytes < sizeof(PhysicsStateRuntimeInventory))
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        if (const auto remaining = budget.maximumRetainedBytes - sizeof(PhysicsStateRuntimeInventory);
            budget.maximumCaptureOwners > remaining / sizeof(std::shared_ptr<PhysicsStateResourceRecord>))
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        try {
            auto inventory = std::make_unique<PhysicsStateRuntimeInventory>();
            inventory->budget_ = budget;
            inventory->records_.reserve(budget.maximumCaptureOwners);
            inventory->reserved_ =
                sizeof(*inventory) + inventory->records_.capacity() * sizeof(std::shared_ptr<PhysicsStateResourceRecord>);
            if (inventory->reserved_ > budget.maximumRetainedBytes ||
                additionalFixedPayload > budget.maximumRetainedBytes - inventory->reserved_)
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            inventory->reserved_ += additionalFixedPayload;
            if (inventory->reserved_ > budget.maximumRetainedBytes)
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            return Outcome::Success(std::move(inventory));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }
    }

    /** @copydoc PhysicsStateRuntimeInventory::Register */
    Result<void> PhysicsStateRuntimeInventory::Register(const std::shared_ptr<PhysicsStateResourceRecord> &record) {
        if (!record || record->ownerThread != std::this_thread::get_id() || record->reservedPayloadBytes < sizeof(*record))
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        if (std::ranges::any_of(records_, [&record](const auto &existing) {
            return existing.get() == record.get();
        }))
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        if (records_.size() == budget_.maximumCaptureOwners || record->reservedPayloadBytes > budget_.maximumRetainedBytes - reserved_)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        records_.push_back(record);
        reserved_ += record->reservedPayloadBytes;
        return Result<void>::Success();
    }

    /** @copydoc PhysicsStateRuntimeInventory::Collect */
    std::uint32_t PhysicsStateRuntimeInventory::Collect(std::uint32_t maximumRecords) noexcept {
        const auto budget = std::min<std::size_t>(maximumRecords, records_.size());
        std::uint32_t retired{};
        for (std::size_t examined = 0; examined < budget && !records_.empty(); ++examined) {
            cursor_ %= records_.size();
            if (records_[cursor_].use_count() == 1) {
                auto record = std::move(records_[cursor_]);
                records_[cursor_] = std::move(records_.back());
                records_.pop_back();
                reserved_ -= record->reservedPayloadBytes;
                record.reset();
                ++retired;
            } else
                ++cursor_;
        }
        return retired;
    }
}  // namespace Horo::Physics::Detail
