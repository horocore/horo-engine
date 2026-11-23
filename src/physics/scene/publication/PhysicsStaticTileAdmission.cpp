#include "Horo/Physics/PhysicsStaticTileAdmission.h"

#include <algorithm>
#include <limits>
#include <new>
#include <utility>

namespace Horo::Physics {
    /** @copydoc PhysicsStaticTileAdmission::Create */
    Result<PhysicsStaticTileAdmission> PhysicsStaticTileAdmission::Create(PhysicsWorldId authority, PhysicsShapeCookTargetDigest target,
                                                                          std::uint32_t maximumProducers, std::uint32_t maximumTiles) {
        using Output = Result<PhysicsStaticTileAdmission>;
        constexpr std::uint32_t MaximumCapacity = 65'536;
        if (!authority.IsValid())
            return Output::Failure(MakeError(PhysicsErrors::WorldInvalid));
        if (maximumProducers == 0 || maximumProducers > MaximumCapacity || maximumTiles == 0 || maximumTiles > MaximumCapacity)
            return Output::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        try {
            std::vector<ProducerSlot> producers(maximumProducers);
            std::vector<Tile> tiles;
            tiles.reserve(maximumTiles);
            return Output::Success({authority, target, std::move(producers), std::move(tiles), maximumTiles});
        } catch (const std::bad_alloc &) {
            return Output::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }
    }

    /** @brief Captures preallocated storage for the original authority. */
    PhysicsStaticTileAdmission::PhysicsStaticTileAdmission(PhysicsWorldId authority, PhysicsShapeCookTargetDigest target,
                                                           std::vector<ProducerSlot> producers, std::vector<Tile> tiles,
                                                           std::uint32_t maximumTiles) noexcept
        : authority_(authority), target_(target), producers_(std::move(producers)), tiles_(std::move(tiles)), maximumTiles_(maximumTiles) {}

    /** @copydoc PhysicsStaticTileAdmission::PhysicsStaticTileAdmission */
    PhysicsStaticTileAdmission::PhysicsStaticTileAdmission(PhysicsStaticTileAdmission &&other) noexcept
        : authority_(std::exchange(other.authority_, {})), target_(other.target_), producers_(std::move(other.producers_)),
          tiles_(std::move(other.tiles_)), maximumTiles_(other.maximumTiles_), nextRequest_(other.nextRequest_) {}

    /** @copydoc PhysicsStaticTileAdmission::OpenProducer */
    Result<PhysicsStaticTileProducer> PhysicsStaticTileAdmission::OpenProducer() {
        using Output = Result<PhysicsStaticTileProducer>;
        if (!authority_.IsValid())
            return Output::Failure(MakeError(PhysicsErrors::WorldInvalid));
        const auto free = std::ranges::find_if(producers_, [](const auto &slot) {
            return !slot.open && slot.generation != 0;
        });
        if (free == producers_.end())
            return Output::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        free->open = true;
        return Output::Success({authority_, {static_cast<std::uint32_t>(free - producers_.begin()), free->generation}});
    }

    /** @brief Rejects foreign and recycled producer slots before touching ownership. */
    Result<void> PhysicsStaticTileAdmission::ValidateProducer(PhysicsStaticTileProducer producer) const {
        if (!producer.IsValid())
            return Result<void>::Failure(MakeError(PhysicsErrors::HandleMalformed));
        if (producer.world != authority_)
            return Result<void>::Failure(MakeError(PhysicsErrors::HandleWorldMismatch));
        if (producer.slot.index >= producers_.size())
            return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
        const auto &slot = producers_[producer.slot.index];
        return slot.open && slot.generation == producer.slot.generation ? Result<void>::Success()
                                                                        : Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
    }

    /** @copydoc PhysicsStaticTileAdmission::CloseProducer */
    Result<void> PhysicsStaticTileAdmission::CloseProducer(PhysicsStaticTileProducer producer) {
        if (auto valid = ValidateProducer(producer); valid.HasError())
            return valid;
        if (std::ranges::any_of(tiles_, [producer](const auto &tile) {
            return tile.owner == producer && tile.active;
        }))
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState, "Retire the producer's active tiles before closing it."));
        std::erase_if(tiles_, [producer](const auto &tile) {
            return tile.owner == producer;
        });
        auto &slot = producers_[producer.slot.index];
        slot.open = false;
        slot.generation = slot.generation == std::numeric_limits<std::uint32_t>::max() ? 0 : slot.generation + 1;
        return Result<void>::Success();
    }

    /** @brief Validates authored shape identity without resolving a payload or touching Physics execution. */
    Result<void> PhysicsStaticTileAdmission::ValidateUpdate(const PhysicsStaticTileUpdate &update) const {
        using enum PhysicsStaticTileOperation;
        if (!update.key.asset.IsValid() || !update.key.tile.IsValid())
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        if (update.operation == Retire && !update.content && update.expectedVersion != 0)
            return Result<void>::Success();
        if ((update.operation != Submit && update.operation != Replace) || !update.content)
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        const auto &content = *update.content;
        if (content.version <= update.expectedVersion || (update.operation == Replace && update.expectedVersion == 0))
            return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
        if (content.shape.asset != update.key.asset || content.shape.subresource != update.key.tile || !content.bounds.IsValid())
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        return ValidatePhysicsCookedShapeDescriptor(content.shape, target_);
    }

    /** @copydoc PhysicsStaticTileAdmission::Admit */
    Result<PhysicsStaticTileReceipt> PhysicsStaticTileAdmission::Admit(PhysicsStaticTileProducer producer,
                                                                       const PhysicsStaticTileUpdate &update) {
        using Output = Result<PhysicsStaticTileReceipt>;
        using enum PhysicsStaticTileOperation;
        if (auto valid = ValidateProducer(producer); valid.HasError())
            return Output::Failure(valid.ErrorValue());
        if (auto valid = ValidateUpdate(update); valid.HasError())
            return Output::Failure(valid.ErrorValue());
        auto tile = std::ranges::find(tiles_, update.key, &Tile::key);
        if (tile != tiles_.end()) {
            if (tile->owner != producer)
                return Output::Failure(MakeError(PhysicsErrors::StaticTileOwnerConflict));
            if (tile->pending || (tile->active && update.operation == Submit))
                return Output::Failure(MakeError(PhysicsErrors::StaticTileDuplicate));
            if (tile->version != update.expectedVersion || (!tile->active && update.operation != Submit))
                return Output::Failure(MakeError(PhysicsErrors::HandleStale));
        } else {
            if (update.operation != Submit || update.expectedVersion != 0)
                return Output::Failure(MakeError(PhysicsErrors::HandleStale));
            if (tiles_.size() == maximumTiles_)
                return Output::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }
        if (nextRequest_ == 0)
            return Output::Failure(MakeError(PhysicsErrors::GenerationExhausted));
        const auto version = update.content ? update.content->version : update.expectedVersion;
        const PhysicsStaticTileReceipt receipt{producer, update.key, nextRequest_++, version, PhysicsStaticTileStage::Accepted};
        const Pending pending{receipt, update};
        if (tile == tiles_.end())
            tiles_.push_back({.owner = producer, .key = update.key, .pending = pending});
        else
            tile->pending = pending;
        return Output::Success(receipt);
    }

    /** @copydoc PhysicsStaticTileAdmission::ReadPending */
    Result<PhysicsStaticTileUpdate> PhysicsStaticTileAdmission::ReadPending(const PhysicsStaticTileReceipt &receipt) const {
        const auto tile = std::ranges::find(tiles_, receipt.key, &Tile::key);
        if (receipt.producer.world != authority_ || tile == tiles_.end() || !tile->pending || tile->pending->receipt != receipt)
            return Result<PhysicsStaticTileUpdate>::Failure(MakeError(PhysicsErrors::HandleStale));
        return Result<PhysicsStaticTileUpdate>::Success(tile->pending->update);
    }

    /** @copydoc PhysicsStaticTileAdmission::Complete */
    Result<PhysicsStaticTileReceipt> PhysicsStaticTileAdmission::Complete(const PhysicsStaticTileReceipt &receipt,
                                                                          PhysicsStaticTileOutcome outcome) {
        using Output = Result<PhysicsStaticTileReceipt>;
        const auto tile = std::ranges::find(tiles_, receipt.key, &Tile::key);
        if (receipt.producer.world != authority_ || tile == tiles_.end() || !tile->pending || tile->pending->receipt != receipt)
            return Output::Failure(MakeError(PhysicsErrors::HandleStale));
        auto terminal = receipt;
        switch (outcome) {
            case PhysicsStaticTileOutcome::Applied:
                tile->active = tile->pending->update.operation != PhysicsStaticTileOperation::Retire;
                tile->version = receipt.version;
                terminal.stage = tile->active ? PhysicsStaticTileStage::Activated : PhysicsStaticTileStage::Retired;
                break;
            case PhysicsStaticTileOutcome::Failed:
                terminal.stage = PhysicsStaticTileStage::Failed;
                break;
            case PhysicsStaticTileOutcome::Cancelled:
                terminal.stage = PhysicsStaticTileStage::Cancelled;
                break;
            default:
                return Output::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        }
        tile->pending.reset();
        if (!tile->active && tile->version == 0)
            tiles_.erase(tile);
        return Output::Success(terminal);
    }
}  // namespace Horo::Physics
