#include "PhysicsSceneStateTarget.h"

#include "Horo/Physics/CharacterWorld.h"
#include "internal/PhysicsSceneHostAccess.h"

namespace Horo::Physics::Detail {
    /** @copydoc PhysicsSceneHostAccess::StateTarget */
    Result<PhysicsSceneStateTarget> PhysicsSceneHostAccess::StateTarget(PhysicsSceneActivationCandidate &candidate, bool published) {
        using Outcome = Result<PhysicsSceneStateTarget>;
        if (auto current = candidate.ValidatePublication(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (candidate.published_ != published || !candidate.structuralState_)
            return Outcome::Failure(MakeError(PhysicsErrors::InvalidState));
        return Outcome::Success(PhysicsSceneStateTarget{candidate, candidate.structuralState_, published});
    }

    /** @copydoc PhysicsSceneStateTarget::PhysicsSceneStateTarget */
    PhysicsSceneStateTarget::PhysicsSceneStateTarget(PhysicsSceneActivationCandidate &candidate,
                                                     std::shared_ptr<PhysicsStructuralState> state, bool published) noexcept
        : candidate_(&candidate), state_(state), ownerThread_(std::this_thread::get_id()), scene_(state->scene), revision_(state->revision),
          published_(published) {}

    /** @copydoc PhysicsSceneStateTarget::Validate */
    Result<std::shared_ptr<PhysicsStructuralState>> PhysicsSceneStateTarget::Validate() const {
        using Outcome = Result<std::shared_ptr<PhysicsStructuralState>>;
        if (ownerThread_ != std::this_thread::get_id())
            return Outcome::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        auto state = state_.lock();
        if (!state || state->closed)
            return Outcome::Failure(MakeError(PhysicsErrors::CapabilityRevoked));
        if (state->revision != revision_ || state->scene != scene_)
            return Outcome::Failure(MakeError(PhysicsErrors::CapabilityStale));
        // Candidate destructor closes this same existing state before freeing either owned World.
        if (!candidate_ || state->world != candidate_->physics_.get() || candidate_->published_ != published_)
            return Outcome::Failure(MakeError(PhysicsErrors::CapabilityStale));
        if (auto current = candidate_->ValidatePublication(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        return Outcome::Success(std::move(state));
    }

    /** @copydoc PhysicsSceneStateTarget::EmptyCharacterOwner */
    Result<void> PhysicsSceneStateTarget::EmptyCharacterOwner() const {
        if (!candidate_->characterBindings_.empty() || candidate_->character_->ActiveControllerCount() != 0)
            return Result<void>::Failure(
                MakeError(PhysicsErrors::OperationUnsupported, "Canonical Physics Save does not yet own Character continuation."));
        if (candidate_->character_->PublishedTick().completedTick != candidate_->physics_->PublishedTick().completedTick)
            return Result<void>::Failure(MakeError(PhysicsErrors::QuerySnapshotStale));
        return Result<void>::Success();
    }

    /** @copydoc PhysicsSceneStateTarget::CoveredOwner */
    Result<std::shared_ptr<PhysicsStructuralState>> PhysicsSceneStateTarget::CoveredOwner(bool published) const {
        using Outcome = Result<std::shared_ptr<PhysicsStructuralState>>;
        auto state = Validate();
        if (state.HasError())
            return state;
        if (published_ != published)
            return Outcome::Failure(MakeError(PhysicsErrors::InvalidState));
        if (state.Value()->world->State() == PhysicsWorldState::ActiveNull)
            return Outcome::Failure(MakeError(PhysicsErrors::OperationUnsupported));
        if (auto empty = EmptyCharacterOwner(); empty.HasError())
            return Outcome::Failure(empty.ErrorValue());
        return state;
    }

    /** @copydoc PhysicsSceneStateTarget::CaptureReady */
    Result<bool> PhysicsSceneStateTarget::CaptureReady() const {
        auto state = CoveredOwner(true);
        if (state.HasError())
            return Result<bool>::Failure(state.ErrorValue());
        return PhysicsWorldStateRestartAccess::CaptureReady(*state.Value()->world);
    }

    /** @copydoc PhysicsSceneStateTarget::DecodeInput */
    Result<std::unique_ptr<StateDetail::RestartInput>> PhysicsSceneStateTarget::DecodeInput(std::span<const std::byte> bytes,
                                                                                            std::size_t maximumWireBytes,
                                                                                            std::size_t maximumRetainedBytes) const {
        auto state = CoveredOwner(false);
        if (state.HasError())
            return Result<std::unique_ptr<StateDetail::RestartInput>>::Failure(state.ErrorValue());
        if (auto pending = PreflightPendingContinuation(); pending.HasError())
            return Result<std::unique_ptr<StateDetail::RestartInput>>::Failure(pending.ErrorValue());
        return PhysicsWorldStateRestartAccess::DecodeRestartInput(*state.Value()->world, bytes, maximumWireBytes, maximumRetainedBytes);
    }
}  // namespace Horo::Physics::Detail
