#include "PhysicsStateConstructionStorage.h"

#include <utility>

namespace Horo::Physics::StateDetail {
    ConstructionRecipeLease::ConstructionRecipeLease(std::shared_ptr<ConstructionRecipePool> owner, std::uint32_t slot,
                                                     std::uint64_t serial) noexcept
        : owner_(std::move(owner)), slot_(slot), serial_(serial) {}

    ConstructionRecipeLease::ConstructionRecipeLease(const ConstructionRecipeLease &other) noexcept
        : owner_(other.owner_), slot_(other.slot_), serial_(other.serial_) {
        if (owner_)
            owner_->AddReader(slot_, serial_);
    }

    ConstructionRecipeLease &ConstructionRecipeLease::operator=(const ConstructionRecipeLease &other) noexcept {
        if (this != &other) {
            ConstructionRecipeLease copy{other};
            *this = std::move(copy);
        }
        return *this;
    }

    ConstructionRecipeLease::ConstructionRecipeLease(ConstructionRecipeLease &&other) noexcept
        : owner_(std::move(other.owner_)), slot_(other.slot_), serial_(other.serial_) {}

    ConstructionRecipeLease &ConstructionRecipeLease::operator=(ConstructionRecipeLease &&other) noexcept {
        if (this != &other) {
            Reset();
            owner_ = std::move(other.owner_);
            slot_ = other.slot_;
            serial_ = other.serial_;
        }
        return *this;
    }

    ConstructionRecipeLease::~ConstructionRecipeLease() {
        Reset();
    }

    void ConstructionRecipeLease::Reset() noexcept {
        if (owner_) {
            owner_->DropReader(slot_, serial_);
            owner_.reset();
        }
    }
}  // namespace Horo::Physics::StateDetail
