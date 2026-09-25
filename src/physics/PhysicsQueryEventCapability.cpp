#include "PhysicsWorldInternal.h"

#include <algorithm>
#include <limits>

namespace Horo::Physics {
    namespace {
        /** @brief Validates exact client evidence before any borrowed world access. */
        [[nodiscard]] Result<PhysicsWorld *> ResolveAccess(const PhysicsQueryEventCapabilityState &state,
                                                           const PhysicsQueryEventIdentity requested) {
            if (state.ownerThread != std::this_thread::get_id())
                return Result<PhysicsWorld *>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
            if (!requested.world.IsValid() || requested.world != state.identity.world)
                return Result<PhysicsWorld *>::Failure(MakeError(PhysicsErrors::HandleWorldMismatch));
            if (requested.capabilityGeneration == 0 || requested.capabilityGeneration != state.identity.capabilityGeneration)
                return Result<PhysicsWorld *>::Failure(MakeError(PhysicsErrors::CapabilityStale));
            if (state.revoked)
                return Result<PhysicsWorld *>::Failure(MakeError(PhysicsErrors::CapabilityRevoked));
            if (state.stale || state.world == nullptr)
                return Result<PhysicsWorld *>::Failure(MakeError(PhysicsErrors::CapabilityStale));
            return Result<PhysicsWorld *>::Success(state.world);
        }
    }  // namespace

    /** @copydoc PhysicsQueryEventCapability::Identity */
    PhysicsQueryEventIdentity PhysicsQueryEventCapability::Identity() const noexcept {
        return state_ ? state_->identity : PhysicsQueryEventIdentity{};
    }

    /** @copydoc PhysicsWorld::IssueQueryEventCapability */
    Result<PhysicsQueryEventCapability> PhysicsWorld::IssueQueryEventCapability() {
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<PhysicsQueryEventCapability>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state == PhysicsWorldState::ActiveNull || impl_->runtime->state != PhysicsRuntimeState::Ready)
            return Result<PhysicsQueryEventCapability>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->stepping)
            return Result<PhysicsQueryEventCapability>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (impl_->queryEvents.nextCapabilityGeneration == 0)
            return Result<PhysicsQueryEventCapability>::Failure(MakeError(PhysicsErrors::GenerationExhausted));
        std::erase_if(impl_->queryEvents.capabilities, [](const auto &weak) {
            const auto access = weak.lock();
            return !access || access->revoked;
        });
        if (impl_->queryEvents.capabilities.size() >= MaximumPhysicsQueryEventCapabilitiesPerWorld)
            return Result<PhysicsQueryEventCapability>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        try {
            auto state = std::make_shared<PhysicsQueryEventCapabilityState>();
            state->world = this;
            state->identity = {impl_->identity, impl_->queryEvents.nextCapabilityGeneration};
            state->ownerThread = impl_->runtime->ownerThread;
            impl_->queryEvents.capabilities.push_back(state);
            impl_->queryEvents.nextCapabilityGeneration =
                impl_->queryEvents.nextCapabilityGeneration == std::numeric_limits<std::uint64_t>::max()
                    ? 0
                    : impl_->queryEvents.nextCapabilityGeneration + 1;
            return Result<PhysicsQueryEventCapability>::Success(PhysicsQueryEventCapability{std::move(state)});
        } catch (const std::bad_alloc &) {
            return Result<PhysicsQueryEventCapability>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }
    }

    /** @copydoc PhysicsWorld::RevokeQueryEventCapability */
    Result<void> PhysicsWorld::RevokeQueryEventCapability(const PhysicsQueryEventCapability &capability) const {
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (!capability.state_)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        auto &state = *capability.state_;
        if (state.identity.world != impl_->identity)
            return Result<void>::Failure(MakeError(PhysicsErrors::HandleWorldMismatch));
        if (state.stale || state.world != this)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        state.revoked = true;
        state.world = nullptr;
        return Result<void>::Success();
    }

    /** @copydoc PhysicsQueryEventCapability::Submit */
    Result<PhysicsQueryCompletion> PhysicsQueryEventCapability::Submit(const PhysicsQueryCommand &command,
                                                                       const std::span<PhysicsQueryHit> hits) const {
        if (!state_)
            return Result<PhysicsQueryCompletion>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        const auto access = ResolveAccess(*state_, command.identity);
        if (access.HasError())
            return Result<PhysicsQueryCompletion>::Failure(access.ErrorValue());
        const PhysicsWorld &world = *access.Value();
        const auto &impl = *world.impl_;
        if (impl.state != PhysicsWorldState::ActiveSolver || impl.runtime->state != PhysicsRuntimeState::Ready)
            return Result<PhysicsQueryCompletion>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl.stepping)
            return Result<PhysicsQueryCompletion>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (command.descriptor.world != command.identity.world)
            return Result<PhysicsQueryCompletion>::Failure(MakeError(PhysicsErrors::HandleWorldMismatch));
        const PhysicsPublishedTick published = world.PublishedTick();
        if (published.completedTick == 0)
            return Result<PhysicsQueryCompletion>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (command.expectedPublicationRevision != published.publicationRevision)
            return Result<PhysicsQueryCompletion>::Failure(MakeError(PhysicsErrors::QuerySnapshotStale));
        const auto result = world.Query(command.descriptor, hits);
        if (result.HasError())
            return Result<PhysicsQueryCompletion>::Failure(result.ErrorValue());
        return Result<PhysicsQueryCompletion>::Success({result.Value(), published.completedTick, published.publicationRevision});
    }

    /** @copydoc PhysicsQueryEventCapability::ReadEvents */
    Result<PhysicsEventReadCompletion> PhysicsQueryEventCapability::ReadEvents(const PhysicsEventReadCommand &command,
                                                                               const std::span<PhysicsEventRecord> records) const {
        if (!state_)
            return Result<PhysicsEventReadCompletion>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        const auto access = ResolveAccess(*state_, command.identity);
        if (access.HasError())
            return Result<PhysicsEventReadCompletion>::Failure(access.ErrorValue());
        const PhysicsWorld &world = *access.Value();
        const auto &impl = *world.impl_;
        if (impl.state != PhysicsWorldState::ActiveSolver || impl.runtime->state != PhysicsRuntimeState::Ready)
            return Result<PhysicsEventReadCompletion>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl.stepping)
            return Result<PhysicsEventReadCompletion>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (command.maximumRecords == 0 || command.maximumRecords > impl.settings.Values().budgets.maximumEvents ||
            records.size() > impl.settings.Values().budgets.maximumEvents)
            return Result<PhysicsEventReadCompletion>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        const PhysicsPublishedTick published = world.PublishedTick();
        if (published.completedTick == 0)
            return Result<PhysicsEventReadCompletion>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (command.completedTick != published.eventTick || command.publicationRevision != published.publicationRevision ||
            impl.queryEvents.events.PublishedTick() != published.eventTick)
            return Result<PhysicsEventReadCompletion>::Failure(MakeError(PhysicsErrors::QuerySnapshotStale));
        return Result<PhysicsEventReadCompletion>::Success(impl.queryEvents.events.CopyPublishedEvents(records, command.maximumRecords));
    }
}  // namespace Horo::Physics
