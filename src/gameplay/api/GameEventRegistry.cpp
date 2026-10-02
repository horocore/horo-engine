#include "Horo/Gameplay/GameEventRegistry.h"

#include "Horo/Gameplay/GameplayErrors.h"

#include <algorithm>
#include <utility>

namespace Horo::Gameplay {
    GameplayEventLease::GameplayEventLease(ConstructionKey, std::shared_ptr<void> owner, const GameplayEventRegistration &registration,
                                           CancellationToken cancellation, const std::atomic_bool *admission)
        : owner_(std::move(owner)), registration_(registration), cancellation_(std::move(cancellation)), admission_(admission) {}

    /** @copydoc GameplayEventLease::Invoke */
    GameplayEventOutcome GameplayEventLease::Invoke(const GameplayEventRequest &request) const noexcept {
        if (cancellation_.IsCancellationRequested() || !admission_->load(std::memory_order_acquire))
            return GameplayEventOutcome::CapabilityUnavailable;
        try {
            return registration_.callback(registration_.context, request);
        } catch (...) {
            return GameplayEventOutcome::HandlerFailed;
        }
    }

    /** @copydoc GameEventRegistry::Register */
    Result<void> GameEventRegistry::Register(const GameplayEventRegistration &registration) {
        if (frozen_ || registrations_.size() >= 2'048 || registration.binding == 0 || registration.bindingVersion == 0 ||
            registration.schema == 0 || registration.schemaVersion == 0 || !registration.context.IsValid() ||
            registration.callback == nullptr || std::ranges::any_of(registrations_, [&](const auto &existing) {
            return existing.binding == registration.binding;
        }))
            return Result<void>::Failure(
                MakeError(GameplayErrors::InvalidGameModuleDescriptor, "Invalid or duplicate gameplay event registration."));
        registrations_.push_back(registration);
        return Result<void>::Success();
    }

    /** @copydoc GameEventRegistry::Freeze */
    void GameEventRegistry::Freeze() noexcept {
        frozen_ = true;
    }

    /** @copydoc GameEventRegistry::Acquire */
    Result<std::shared_ptr<GameplayEventLease>> GameEventRegistry::Acquire(const std::uint64_t binding, const std::uint64_t bindingVersion,
                                                                           const std::uint64_t schema, const std::uint64_t schemaVersion,
                                                                           CancellationToken cancellation) const {
        const auto found = std::ranges::find_if(registrations_, [&](const auto &entry) {
            return entry.binding == binding && entry.bindingVersion == bindingVersion && entry.schema == schema &&
                   entry.schemaVersion == schemaVersion;
        });
        auto owner = generationLease_.lock();
        if (!frozen_ || !owner || generationLeaseAdmission_ == nullptr || !generationLeaseAdmission_->load(std::memory_order_acquire) ||
            cancellation.IsCancellationRequested() || found == registrations_.end())
            return Result<std::shared_ptr<GameplayEventLease>>::Failure(
                MakeError(GameplayErrors::GameplayReloadRestartRequired, "The exact gameplay event callback generation is unavailable."));
        return Result<std::shared_ptr<GameplayEventLease>>::Success(
            std::make_shared<GameplayEventLease>(GameplayEventLease::ConstructionKey{}, std::move(owner), *found, std::move(cancellation),
                                                 generationLeaseAdmission_));
    }
}  // namespace Horo::Gameplay
