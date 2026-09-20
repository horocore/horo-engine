#include "CharacterWorldCommandInternal.h"

namespace Horo::Character {
    /** @copydoc CharacterWorld::QueueMovementCommand */
    Result<CharacterCommandAdmission> CharacterWorld::QueueMovementCommand(const CharacterMovementRequest &request) {
        const auto rejected = [this](const CharacterCommandAdmissionStatus status) {
            impl_->rejectedCommands.fetch_add(1);
            return Result<CharacterCommandAdmission>::Success({status, impl_->pendingCommands.load()});
        };
        if (const auto valid = Detail::ValidateAdmissionRequest(*impl_, request); valid.HasError()) {
            impl_->rejectedCommands.fetch_add(1);
            return Result<CharacterCommandAdmission>::Failure(valid.ErrorValue());
        }

        if (auto queueLock = impl_->synchronization.TryLockCommands(); queueLock.owns_lock()) {
            const auto registryLock = impl_->synchronization.LockRegistry();
            if (const auto valid = Detail::ValidateLockedAdmission(*impl_, request); valid.HasError()) {
                impl_->rejectedCommands.fetch_add(1);
                return Result<CharacterCommandAdmission>::Failure(valid.ErrorValue());
            }
            if (impl_->fastPath.Commands().size() >= impl_->settings.Values().capacities.maximumQueuedCommands) {
                impl_->commandOverflowCount.fetch_add(1);
                return rejected(CharacterCommandAdmissionStatus::RejectedFull);
            }

            impl_->fastPath.Commands().push_back(request);
            const auto depth = static_cast<std::uint32_t>(impl_->fastPath.Commands().size());
            impl_->pendingCommands.store(depth);
            impl_->maximumCommandDepth.store(std::max(depth, impl_->maximumCommandDepth.load()));
            impl_->admittedCommands.fetch_add(1);
            return Result<CharacterCommandAdmission>::Success({CharacterCommandAdmissionStatus::Deferred, depth});
        }
        return rejected(CharacterCommandAdmissionStatus::RejectedBusy);
    }

    /** @copydoc CharacterWorld::AdvanceFixedTick */
    Result<void> CharacterWorld::AdvanceFixedTick(const CharacterFixedTickInput &input) {
        if (const auto owner = Detail::RequireOwnerThread(impl_->ownerThread); owner.HasError())
            return owner;
        if (impl_->state.load() != CharacterWorldState::Active || impl_->ticking.load())
            return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
        if (input.tick == 0 || input.sceneGeneration != impl_->descriptor.sceneGeneration || input.fixedDelta <= Duration{} ||
            input.tick != impl_->closedTick.load() + 1)
            return Result<void>::Failure(MakeError(CharacterErrors::CommandOrderInvalid));

        Detail::TickGuard ticking{*impl_};
        impl_->fastPath.ResetTransient();
        if (const auto frozen = Detail::FreezeCommandFrame(*impl_, input); frozen.HasError())
            return frozen;

        Detail::ObservePhase(input, CharacterTickPhase::FreezeCommands);
        const auto applied = Detail::ApplyCommandFrame(*impl_, input);
        if (applied.HasError())
            return Result<void>::Failure(applied.ErrorValue());
        if (impl_->state.load() != CharacterWorldState::Active)
            return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
        Detail::ObservePhase(input, CharacterTickPhase::ResolveMovement);

        impl_->fastPath.Canonicalize();
        Detail::PublishTick(*impl_, input, applied.Value());
        impl_->completedTicks.fetch_add(1);
        Detail::ObservePhase(input, CharacterTickPhase::PublishCompletedTick);
        return Result<void>::Success();
    }

    /** @copydoc CharacterWorld::PublishedTick */
    CharacterPublishedTick CharacterWorld::PublishedTick() const noexcept {
        const auto publicationLock = impl_->synchronization.LockPublication();
        return impl_->published;
    }

    /** @copydoc CharacterWorld::TickStatistics */
    CharacterTickStatistics CharacterWorld::TickStatistics() const noexcept {
        const auto fastPath = impl_->fastPath.Snapshot();
        return {.completedTicks = impl_->completedTicks.load(),
                .admittedCommands = impl_->admittedCommands.load(),
                .rejectedCommands = impl_->rejectedCommands.load(),
                .pendingCommands = impl_->pendingCommands.load(),
                .maximumCommandDepth = impl_->maximumCommandDepth.load(),
                .commandOverflowCount = impl_->commandOverflowCount.load(),
                .contactOverflowCount = fastPath.contactOverflowCount,
                .hitOverflowCount = fastPath.hitOverflowCount,
                .eventOverflowCount = fastPath.eventOverflowCount,
                .impulseOverflowCount = fastPath.impulseOverflowCount,
                .scratchOverflowCount = fastPath.scratchOverflowCount,
                .invalidInputCount = fastPath.invalidInputCount,
                .retainedContacts = fastPath.retainedContacts,
                .queuedHits = fastPath.queuedHits,
                .queuedEvents = fastPath.queuedEvents,
                .scratchBytesUsed = fastPath.scratchBytesUsed};
    }

    /** @copydoc CharacterWorld::Shutdown */
}  // namespace Horo::Character
