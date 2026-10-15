#include "Horo/Gameplay/GameModuleRetirement.h"

#include "Horo/Gameplay/GameplayErrors.h"

#include <exception>
#include <utility>

namespace Horo::Gameplay {
    /** @copydoc GameModuleRetirementQueue::Reservation::Reservation */
    GameModuleRetirementQueue::Reservation::Reservation(GameModuleRetirementQueue &queue, const std::size_t slot) noexcept
        : queue_(&queue), slot_(slot) {}

    GameModuleRetirementQueue::Reservation::Reservation(Reservation &&other) noexcept
        : queue_(std::exchange(other.queue_, nullptr)), slot_(other.slot_) {}

    GameModuleRetirementQueue::Reservation::~Reservation() {
        if (queue_) {
            if (std::this_thread::get_id() != queue_->thread_)
                std::terminate();
            queue_->entries_[slot_].reserved = false;
        }
    }

    /** @copydoc GameModuleRetirementQueue::Reservation::Retain */
    void GameModuleRetirementQueue::Reservation::Retain(std::unique_ptr<LoadedGameModule> owner) noexcept {
        if (!queue_ || std::this_thread::get_id() != queue_->thread_)
            std::terminate();
        auto &entry = queue_->entries_[slot_];
        entry.owner = std::move(owner);
        if (entry.owner)
            entry.owner->RequestRetirement();
        entry.reserved = false;
        queue_ = nullptr;
    }

    /** @copydoc GameModuleRetirementQueue::GameModuleRetirementQueue */
    GameModuleRetirementQueue::GameModuleRetirementQueue(const std::size_t capacity)
        : entries_(capacity > 0 && capacity <= 64 ? capacity : 0) {}

    GameModuleRetirementQueue::~GameModuleRetirementQueue() {
        if (Close().HasError())
            std::terminate();
    }

    /** @copydoc GameModuleRetirementQueue::Reserve */
    Result<GameModuleRetirementQueue::Reservation> GameModuleRetirementQueue::Reserve() {
        if (std::this_thread::get_id() == thread_ && !draining_)
            for (std::size_t index = 0; index < entries_.size(); ++index) {
                auto &entry = entries_[index];
                if (!entry.reserved && !entry.owner) {
                    entry.reserved = true;
                    return Result<Reservation>::Success(Reservation{*this, index});
                }
            }
        return Result<Reservation>::Failure(
            MakeError(GameplayErrors::GameplayReloadRestartRequired, "Gameplay retirement admission is unavailable."));
    }

    /** @copydoc GameModuleRetirementQueue::Drain */
    Result<std::size_t> GameModuleRetirementQueue::Drain() {
        if (std::this_thread::get_id() != thread_)
            return Result<std::size_t>::Failure(MakeError(GameplayErrors::GameplayReloadRestartRequired,
                                                          "Gameplay retirement state belongs to its application owner lane."));
        if (draining_)
            return Result<std::size_t>::Failure(
                MakeError(GameplayErrors::GameplayReloadRestartRequired, "Gameplay retirement cannot reenter a native teardown callback."));
        draining_ = true;

        struct DrainScope final {
            bool &active;

            ~DrainScope() {
                active = false;
            }
        } scope{draining_};

        std::size_t pending{};
        for (auto &entry : entries_) {
            if (entry.reserved)
                ++pending;
            if (!entry.owner)
                continue;
            if (auto retired = entry.owner->TryRetire(); retired.HasError())
                return Result<std::size_t>::Failure(retired.ErrorValue());
            entry.owner.reset();
        }
        return Result<std::size_t>::Success(pending);
    }

    /** @copydoc GameModuleRetirementQueue::Close */
    Result<void> GameModuleRetirementQueue::Close() {
        auto drained = Drain();
        if (drained.HasError())
            return Result<void>::Failure(drained.ErrorValue());
        if (drained.Value() != 0)
            return Result<void>::Failure(MakeError(GameplayErrors::GameplayReloadRestartRequired,
                                                   "Reserved gameplay candidates still require their application owner."));
        return Result<void>::Success();
    }
}  // namespace Horo::Gameplay
