#include "CharacterWorldPlacementInternal.h"

namespace Horo::Character {
    Result<std::unique_ptr<CharacterWorld>> CharacterWorld::Prepare(const CharacterWorldPreparationDescriptor &descriptor,
                                                                    const CharacterWorldSettings &settings) {
        const auto completed = Detail::CompleteWorldDescriptor(descriptor);
        if (completed.HasError())
            return Result<std::unique_ptr<CharacterWorld>>::Failure(completed.ErrorValue());
        const CharacterWorldDescriptor owner = completed.Value();

        try {
            Detail::CharacterControllerRegistry<Detail::CharacterControllerRecord>
                registry{owner.sceneGeneration, owner.identity, {.maximumSlots = settings.Values().capacities.maximumControllers}};
            auto impl = std::make_unique<Impl>(owner, settings, std::move(registry));
            return Result<std::unique_ptr<CharacterWorld>>::Success(std::unique_ptr<CharacterWorld>{
                new CharacterWorld(std::move(impl))});  // NOSONAR: make_unique cannot access this private constructor.
        } catch (const std::bad_alloc &) {
            return Result<std::unique_ptr<CharacterWorld>>::Failure(
                MakeError(CharacterErrors::CapacityExceeded, "Unable to allocate Character world ownership state."));
        }
    }

    /** @copydoc CharacterWorld::CharacterWorld */
    CharacterWorld::CharacterWorld(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    /** @copydoc CharacterWorld::~CharacterWorld */
    CharacterWorld::~CharacterWorld() {
        Shutdown();
    }

    /** @copydoc CharacterWorld::Activate */
    Result<void> CharacterWorld::Activate() {
        if (const auto owner = Detail::RequireOwnerThread(impl_->ownerThread); owner.HasError())
            return owner;
        if (impl_->state.load() != CharacterWorldState::Prepared)
            return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
        impl_->state.store(CharacterWorldState::Active);
        impl_->acceptingCommands.store(true);
        return Result<void>::Success();
    }

    /** @copydoc CharacterWorld::CreateController */
    Result<CharacterControllerHandle> CharacterWorld::CreateController(const CharacterControllerDescriptor &descriptor) {
        if (const auto owner = Detail::RequireOwnerThread(impl_->ownerThread); owner.HasError())
            return Result<CharacterControllerHandle>::Failure(
                MakeError(CharacterErrors::InvalidState, "Controller creation requires prepared owner-thread mutation."));
        if (impl_->state.load() != CharacterWorldState::Prepared)
            return Result<CharacterControllerHandle>::Failure(MakeError(CharacterErrors::InvalidState));
        if (const auto valid = ValidateCharacterControllerDescriptor(descriptor); valid.HasError())
            return Result<CharacterControllerHandle>::Failure(valid.ErrorValue());
        if (const std::array ownerMatches{descriptor.sceneGeneration == impl_->descriptor.sceneGeneration,
                                          descriptor.characterWorld == impl_->descriptor.identity,
                                          descriptor.physicsWorld == impl_->descriptor.physicsWorld};
            !std::ranges::all_of(ownerMatches, std::identity{})) {
            return Result<CharacterControllerHandle>::Failure(MakeError(CharacterErrors::HandleWorldMismatch));
        }
        if (descriptor.maximumContacts > impl_->settings.Values().work.maximumContactsPerMovement)
            return Result<CharacterControllerHandle>::Failure(
                MakeError(CharacterErrors::CapacityExceeded, "Controller contact capacity exceeds the Character world work budget."));
        const auto registryLock = impl_->synchronization.LockRegistry();
        auto acquired = impl_->controllers.Acquire(Detail::CharacterControllerRecord{descriptor});
        if (acquired.HasValue()) {
            const CharacterControllerHandle handle = acquired.Value();
            impl_->controllerGenerations[handle.slot.index] = handle.slot.generation;
            impl_->closedSequences[handle.slot.index] = 0;
            auto record = impl_->controllers.ResolveMutable(handle);
            if (record.HasError())
                return Result<CharacterControllerHandle>::Failure(record.ErrorValue());
            const auto publicationLock = impl_->synchronization.LockPublication();
            record.Value()->publication.controller = handle;
            record.Value()->publication.position = descriptor.collisionRootPosition;
            record.Value()->publication.up = descriptor.up;
        }
        return acquired;
    }

    /** @copydoc CharacterWorld::SpawnController */
    Result<CharacterPlacementResult> CharacterWorld::SpawnController(const CharacterControllerHandle &handle,
                                                                     const CharacterPhysicsQueryContext &query) {
        if (const auto ready = Detail::RequirePlacementMutation(*impl_); ready.HasError())
            return Result<CharacterPlacementResult>::Failure(ready.ErrorValue());

        Detail::PlacementOperationGuard guard{*impl_};
        CharacterControllerDescriptor descriptor;
        {
            const auto registryLock = impl_->synchronization.LockRegistry();
            const auto record = impl_->controllers.Resolve(handle);
            if (record.HasError())
                return Result<CharacterPlacementResult>::Failure(record.ErrorValue());
            if (record.Value()->spawned)
                return Result<CharacterPlacementResult>::Failure(
                    MakeError(CharacterErrors::InvalidState, "A Character controller can only be spawned once."));
            descriptor = record.Value()->descriptor;
        }
        const auto sourceTick = impl_->closedTick.load();
        const auto recovery = Detail::RecoverSpawnPosition(*impl_, handle, descriptor, query, sourceTick, descriptor.collisionRootPosition);
        if (recovery.HasError())
            return Result<CharacterPlacementResult>::Failure(recovery.ErrorValue());
        return Detail::PublishPlacement(*impl_, handle, CharacterPlacementOperation::Spawn, sourceTick, recovery.Value().position,
                                        Math::Quaternion::Identity(), recovery.Value().iterations);
    }

    /** @copydoc CharacterWorld::DestroyController */
    Result<void> CharacterWorld::DestroyController(const CharacterControllerHandle &handle) {
        if (const auto owner = Detail::RequireOwnerThread(impl_->ownerThread); owner.HasError())
            return Result<void>::Failure(
                MakeError(CharacterErrors::InvalidState, "Controller destruction requires prepared owner-thread mutation."));
        if (impl_->state.load() != CharacterWorldState::Prepared)
            return Result<void>::Failure(MakeError(CharacterErrors::InvalidState));
        const auto registryLock = impl_->synchronization.LockRegistry();
        return impl_->controllers.Remove(handle);
    }

    /** @copydoc CharacterWorld::TeleportController */
    Result<CharacterPlacementResult> CharacterWorld::TeleportController(const CharacterTeleportRequest &request,
                                                                        const CharacterPhysicsQueryContext &query) {
        if (const auto ready = Detail::RequirePlacementMutation(*impl_); ready.HasError())
            return Result<CharacterPlacementResult>::Failure(ready.ErrorValue());
        if (const auto valid = ValidateCharacterTeleportRequest(request, impl_->descriptor.sceneGeneration, impl_->descriptor.identity);
            valid.HasError())
            return Result<CharacterPlacementResult>::Failure(valid.ErrorValue());
        Detail::PlacementOperationGuard guard{*impl_};
        const auto descriptor = Detail::ReserveTeleport(*impl_, request);
        if (descriptor.HasError())
            return Result<CharacterPlacementResult>::Failure(descriptor.ErrorValue());
        if (const auto valid = Detail::ValidateTeleportClearance(*impl_, request.controller, descriptor.Value(), query, request.tick,
                                                                 request.targetPosition);
            valid.HasError()) {
            Detail::ReleaseTeleportReservation(*impl_, request.controller, request.tick);
            return Result<CharacterPlacementResult>::Failure(valid.ErrorValue());
        }
        const auto displacement = Detail::CurrentControllerPosition(*impl_, request.controller);
        if (displacement.HasError()) {
            Detail::ReleaseTeleportReservation(*impl_, request.controller, request.tick);
            return Result<CharacterPlacementResult>::Failure(displacement.ErrorValue());
        }
        if (const auto valid = Detail::ValidatePlacementDisplacement(displacement.Value(), request.targetPosition,
                                                                     impl_->settings.Values().work.maximumDisplacementMetersPerTick);
            valid.HasError()) {
            Detail::ReleaseTeleportReservation(*impl_, request.controller, request.tick);
            return Result<CharacterPlacementResult>::Failure(valid.ErrorValue());
        }
        const auto published = Detail::PublishPlacement(*impl_, request.controller, CharacterPlacementOperation::Teleport, request.tick,
                                                        request.targetPosition, request.targetHeading, 0);
        if (published.HasError())
            Detail::ReleaseTeleportReservation(*impl_, request.controller, request.tick);
        return published;
    }

    /** @copydoc CharacterWorld::ControllerDescriptor */
    Result<CharacterControllerDescriptor> CharacterWorld::ControllerDescriptor(const CharacterControllerHandle &handle) const {
        const auto registryLock = impl_->synchronization.LockRegistry();
        if (impl_->state.load() == CharacterWorldState::Destroyed)
            return Result<CharacterControllerDescriptor>::Failure(MakeError(CharacterErrors::InvalidState));
        const auto record = impl_->controllers.Resolve(handle);
        if (record.HasError())
            return Result<CharacterControllerDescriptor>::Failure(record.ErrorValue());
        return Result<CharacterControllerDescriptor>::Success(record.Value()->descriptor);
    }

    /** @copydoc CharacterWorld::ControllerTransform */
    Result<CharacterTransformPublication> CharacterWorld::ControllerTransform(const CharacterControllerHandle &handle) const {
        const auto registryLock = impl_->synchronization.LockRegistry();
        if (impl_->state.load() == CharacterWorldState::Destroyed)
            return Result<CharacterTransformPublication>::Failure(MakeError(CharacterErrors::InvalidState));
        const auto record = impl_->controllers.Resolve(handle);
        if (record.HasError())
            return Result<CharacterTransformPublication>::Failure(record.ErrorValue());
        if (!record.Value()->spawned)
            return Result<CharacterTransformPublication>::Failure(
                MakeError(CharacterErrors::InvalidState, "The controller has no published spawn transform."));
        const auto publicationLock = impl_->synchronization.LockPublication();
        return Result<CharacterTransformPublication>::Success(record.Value()->publication);
    }

    void CharacterWorld::Shutdown() noexcept {
        if (impl_->ownerThread != std::this_thread::get_id())
            return;
        if (impl_->state.load() == CharacterWorldState::Destroyed)
            return;
        impl_->acceptingCommands.store(false);
        {
            const auto queueLock = impl_->synchronization.LockCommands();
            impl_->fastPath.ResetAll();
            impl_->pendingCommands.store(0);
        }
        if (impl_->placementActive || impl_->ticking.load()) {
            impl_->shutdownRequested = true;
            impl_->state.store(CharacterWorldState::Destroyed);
            return;
        }
        const auto registryLock = impl_->synchronization.LockRegistry();
        impl_->controllers.Drain();
        impl_->state.store(CharacterWorldState::Destroyed);
    }

    /** @copydoc CharacterWorld::State */
    CharacterWorldState CharacterWorld::State() const noexcept {
        return impl_->state.load();
    }

    /** @copydoc CharacterWorld::Descriptor */
    const CharacterWorldDescriptor &CharacterWorld::Descriptor() const noexcept {
        return impl_->descriptor;
    }

    /** @copydoc CharacterWorld::Settings */
    const CharacterWorldSettings &CharacterWorld::Settings() const noexcept {
        return impl_->settings;
    }

    /** @copydoc CharacterWorld::ActiveControllerCount */
    std::size_t CharacterWorld::ActiveControllerCount() const noexcept {
        const auto registryLock = impl_->synchronization.LockRegistry();
        return impl_->controllers.Statistics().active;
    }

    /** @copydoc CharacterWorld::ControllerCapacity */
    std::size_t CharacterWorld::ControllerCapacity() const noexcept {
        const auto registryLock = impl_->synchronization.LockRegistry();
        return impl_->controllers.Statistics().capacity;
    }
}  // namespace Horo::Character
