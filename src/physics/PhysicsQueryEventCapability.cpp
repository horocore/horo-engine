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

        struct QueryBatchAdmission final {
            PhysicsPublishedTick published;
            std::uint32_t admittedThisTick{};
        };

        template <typename WorldImpl>
        [[nodiscard]] Result<QueryBatchAdmission> ValidateBatchAdmission(PhysicsWorld &world, WorldImpl &impl,
                                                                         const std::size_t commandCount) {
            if (impl.state != PhysicsWorldState::ActiveSolver || impl.runtime->state != PhysicsRuntimeState::Ready)
                return Result<QueryBatchAdmission>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
            if (impl.stepping)
                return Result<QueryBatchAdmission>::Failure(MakeError(PhysicsErrors::InvalidState));
            const auto &budgets = impl.settings.Values().budgets;
            if (commandCount == 0 || commandCount > budgets.maximumQueriesPerTick || commandCount > budgets.maximumQueries)
                return Result<QueryBatchAdmission>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            if (impl.pendingQueryBatch) {
                if (!impl.pendingQueryBatch->IsTerminal())
                    return Result<QueryBatchAdmission>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
                impl.pendingQueryBatch.reset();
            }
            const auto published = world.PublishedTick();
            if (published.completedTick == 0)
                return Result<QueryBatchAdmission>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
            const auto admittedThisTick = impl.queryBatchTick == published.completedTick ? impl.queryBatchAdmissions : 0;
            if (commandCount > budgets.maximumQueriesPerTick - admittedThisTick)
                return Result<QueryBatchAdmission>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            return Result<QueryBatchAdmission>::Success({published, admittedThisTick});
        }

        template <typename WorldImpl>
        [[nodiscard]] Result<void> ValidateBatchCommands(const PhysicsQueryEventCapabilityState &state, WorldImpl &impl,
                                                         const std::span<const PhysicsQueryCommand> commands,
                                                         const PhysicsPublishedTick published) {
            std::uint64_t hitCapacity{};
            for (const auto &command : commands) {
                if (command.identity.world != state.identity.world)
                    return Result<void>::Failure(MakeError(PhysicsErrors::HandleWorldMismatch));
                if (command.identity.capabilityGeneration != state.identity.capabilityGeneration)
                    return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityStale));
                if (command.expectedPublicationRevision != published.publicationRevision)
                    return Result<void>::Failure(MakeError(PhysicsErrors::QuerySnapshotStale));
                if (const auto valid = ValidatePhysicsQueryDescriptor(command.descriptor, impl.identity, impl.querySceneGeneration);
                    valid.HasError())
                    return Result<void>::Failure(valid.ErrorValue());
                hitCapacity += command.descriptor.maximumHitCount;
                if (hitCapacity > MaximumPhysicsQueryBatchHits)
                    return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            }
            return Result<void>::Success();
        }

        template <typename WorldImpl>
        [[nodiscard]] Result<std::shared_ptr<PhysicsQueryBatchState>> QueueQueryBatch(
            PhysicsWorld &world, WorldImpl &impl, const std::shared_ptr<PhysicsQueryEventCapabilityState> &access,
            const std::span<const PhysicsQueryCommand> commands) {
            const auto admission = ValidateBatchAdmission(world, impl, commands.size());
            if (admission.HasError())
                return Result<std::shared_ptr<PhysicsQueryBatchState>>::Failure(admission.ErrorValue());
            const auto validation = ValidateBatchCommands(*access, impl, commands, admission.Value().published);
            if (validation.HasError())
                return Result<std::shared_ptr<PhysicsQueryBatchState>>::Failure(validation.ErrorValue());
            try {
                auto batch = std::make_shared<PhysicsQueryBatchState>();
                batch->commands.assign(commands.begin(), commands.end());
                batch->access = access;
                impl.pendingQueryBatch = batch;
                impl.queryBatchTick = admission.Value().published.completedTick;
                impl.queryBatchAdmissions = admission.Value().admittedThisTick + static_cast<std::uint32_t>(commands.size());
                return Result<std::shared_ptr<PhysicsQueryBatchState>>::Success(std::move(batch));
            } catch (const std::bad_alloc &) {
                return Result<std::shared_ptr<PhysicsQueryBatchState>>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            }
        }

        [[nodiscard]] bool PrepareBatchEntries(const PhysicsWorld &world, const std::shared_ptr<PhysicsQueryBatchState> &batch,
                                               const PhysicsPublishedTick published, PhysicsQueryBatchCompletion &completed) {
            for (const auto &command : batch->commands) {
                if (batch->IsTerminal())
                    return false;
                PhysicsQueryBatchEntry entry;
                entry.hits.resize(command.descriptor.maximumHitCount);
                const auto result = world.Query(command.descriptor, entry.hits);
                if (result.HasError()) {
                    (void)batch->Fail(result.ErrorValue());
                    return false;
                }
                entry.hits.resize(result.Value().hitCount);
                entry.completion = {result.Value(), published.completedTick, published.publicationRevision};
                completed.entries.push_back(std::move(entry));
            }
            return true;
        }
    }  // namespace

    /** @copydoc PhysicsQueryEventCapability::Identity */
    PhysicsQueryEventIdentity PhysicsQueryEventCapability::Identity() const noexcept {
        return state_ ? state_->identity : PhysicsQueryEventIdentity{};
    }

    /** @copydoc PhysicsQueryBatchHandle::Cancel */
    bool PhysicsQueryBatchHandle::Cancel() const noexcept {
        return state_ && state_->FailCode(PhysicsErrors::QueryCancelled);
    }

    /** @copydoc PhysicsQueryBatchHandle::Poll */
    Result<std::shared_ptr<const PhysicsQueryBatchCompletion>> PhysicsQueryBatchHandle::Poll() const {
        if (!state_)
            return Result<std::shared_ptr<const PhysicsQueryBatchCompletion>>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        return state_->Poll();
    }

    /** @copydoc PhysicsWorld::IssueQueryEventCapability */
    Result<PhysicsQueryEventCapability> PhysicsWorld::IssueQueryEventCapability() {
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<PhysicsQueryEventCapability>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->state == PhysicsWorldState::ActiveNull || impl_->runtime->state != PhysicsRuntimeState::Ready)
            return Result<PhysicsQueryEventCapability>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->stepping)
            return Result<PhysicsQueryEventCapability>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (impl_->nextQueryEventCapabilityGeneration == 0)
            return Result<PhysicsQueryEventCapability>::Failure(MakeError(PhysicsErrors::GenerationExhausted));
        impl_->queryEventCapabilities.erase(std::remove_if(impl_->queryEventCapabilities.begin(), impl_->queryEventCapabilities.end(),
                                                           [](const auto &weak) {
            const auto access = weak.lock();
            return !access || access->revoked;
        }),
                                            impl_->queryEventCapabilities.end());
        if (impl_->queryEventCapabilities.size() >= MaximumPhysicsQueryEventCapabilitiesPerWorld)
            return Result<PhysicsQueryEventCapability>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        try {
            auto state = std::make_shared<PhysicsQueryEventCapabilityState>();
            state->world = this;
            state->identity = {impl_->identity, impl_->nextQueryEventCapabilityGeneration};
            state->ownerThread = impl_->runtime->ownerThread;
            impl_->queryEventCapabilities.push_back(state);
            impl_->nextQueryEventCapabilityGeneration =
                impl_->nextQueryEventCapabilityGeneration == std::numeric_limits<std::uint64_t>::max()
                    ? 0
                    : impl_->nextQueryEventCapabilityGeneration + 1;
            return Result<PhysicsQueryEventCapability>::Success(PhysicsQueryEventCapability{std::move(state)});
        } catch (const std::bad_alloc &) {
            return Result<PhysicsQueryEventCapability>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }
    }

    /** @copydoc PhysicsWorld::RevokeQueryEventCapability */
    Result<void> PhysicsWorld::RevokeQueryEventCapability(const PhysicsQueryEventCapability &capability) {
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
        if (impl_->pendingQueryBatch && impl_->pendingQueryBatch->access == capability.state_)
            (void)impl_->pendingQueryBatch->FailCode(PhysicsErrors::CapabilityRevoked);
        return Result<void>::Success();
    }

    /** @copydoc PhysicsQueryEventCapability::SubmitBatch */
    Result<PhysicsQueryBatchHandle> PhysicsQueryEventCapability::SubmitBatch(const std::span<const PhysicsQueryCommand> commands) const {
        if (!state_)
            return Result<PhysicsQueryBatchHandle>::Failure(MakeError(PhysicsErrors::CapabilityStale));
        const auto access = ResolveAccess(*state_, commands.empty() ? state_->identity : commands.front().identity);
        if (access.HasError())
            return Result<PhysicsQueryBatchHandle>::Failure(access.ErrorValue());
        PhysicsWorld &world = *access.Value();
        auto batch = QueueQueryBatch(world, *world.impl_, state_, commands);
        if (batch.HasError())
            return Result<PhysicsQueryBatchHandle>::Failure(batch.ErrorValue());
        return Result<PhysicsQueryBatchHandle>::Success(PhysicsQueryBatchHandle{std::move(batch.Value())});
    }

    /** @copydoc PhysicsWorld::ProcessQueryBatch */
    Result<void> PhysicsWorld::ProcessQueryBatch() {
        if (impl_->runtime->ownerThread != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (impl_->stepping)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        auto batch = std::move(impl_->pendingQueryBatch);
        if (!batch)
            return Result<void>::Success();
        if (batch->IsTerminal())
            return Result<void>::Success();
        if (impl_->state != PhysicsWorldState::ActiveSolver || impl_->runtime->state != PhysicsRuntimeState::Ready) {
            (void)batch->FailCode(PhysicsErrors::CapabilityUnavailable);
            return Result<void>::Success();
        }
        const auto &access = batch->access;
        if (!access || access->stale || access->world != this) {
            (void)batch->FailCode(PhysicsErrors::CapabilityStale);
            return Result<void>::Success();
        }
        if (access->revoked) {
            (void)batch->FailCode(PhysicsErrors::CapabilityRevoked);
            return Result<void>::Success();
        }
        const auto published = PublishedTick();
        if (published.completedTick == 0 || batch->commands.front().expectedPublicationRevision != published.publicationRevision) {
            (void)batch->FailCode(PhysicsErrors::QuerySnapshotStale);
            return Result<void>::Success();
        }
        try {
            auto completed = std::make_shared<PhysicsQueryBatchCompletion>();
            completed->entries.reserve(batch->commands.size());
            if (!PrepareBatchEntries(*this, batch, published, *completed))
                return Result<void>::Success();
            (void)batch->Complete(std::move(completed));
        } catch (const std::bad_alloc &) {
            (void)batch->FailCode(PhysicsErrors::CapacityExceeded);
        }
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
        PhysicsWorld &world = *access.Value();
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
            impl.events.PublishedTick() != published.eventTick)
            return Result<PhysicsEventReadCompletion>::Failure(MakeError(PhysicsErrors::QuerySnapshotStale));
        return Result<PhysicsEventReadCompletion>::Success(impl.events.CopyPublishedEvents(records, command.maximumRecords));
    }
}  // namespace Horo::Physics
