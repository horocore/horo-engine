#include "PhysicsWorldInternal.h"

namespace Horo::Physics {
    /** @copydoc PhysicsRuntime::BeginRetirement */
    Result<PhysicsRuntimeRetirement> PhysicsRuntime::BeginRetirement() {
        if (impl_->ownerThread != std::this_thread::get_id())
            return Result<PhysicsRuntimeRetirement>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        impl_->state = PhysicsRuntimeState::Stopped;
        impl_->ReleaseNativeWhenIdle();
        return Result<PhysicsRuntimeRetirement>::Success(PhysicsRuntimeRetirement{impl_});
    }

    /** @copydoc PhysicsRuntimeRetirement::~PhysicsRuntimeRetirement */
    PhysicsRuntimeRetirement::~PhysicsRuntimeRetirement() = default;

    /** @copydoc PhysicsRuntimeRetirement::operator= */
    PhysicsRuntimeRetirement &PhysicsRuntimeRetirement::operator=(PhysicsRuntimeRetirement &&other) noexcept = default;

    /** @copydoc PhysicsRuntimeRetirement::Collect */
    Result<PhysicsRuntimeRetirementProgress> PhysicsRuntimeRetirement::Collect(std::uint32_t maximumRecords) {
        using Outcome = Result<PhysicsRuntimeRetirementProgress>;
        if (!owner_)
            return Outcome::Failure(MakeError(PhysicsErrors::InvalidState));
        auto runtime = std::static_pointer_cast<PhysicsRuntime::Impl>(owner_);
        if (runtime->ownerThread != std::this_thread::get_id())
            return Outcome::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        PhysicsRuntimeRetirementProgress progress;
        progress.pendingWorlds = static_cast<std::uint32_t>(std::min<std::size_t>(runtime->identities.size(), UINT32_MAX));
        progress.nativeCandidates = runtime->nativeStateCandidates;
        if (runtime->stateInventory) {
            progress.retiredRecords = runtime->stateInventory->Collect(maximumRecords);
            progress.stateLeaseRecords = runtime->stateInventory->Count();
        }
        runtime->ReleaseNativeWhenIdle();
        if (progress.pendingWorlds)
            progress.phase = PhysicsRuntimeRetirementPhase::WaitingWorlds;
        else if (progress.nativeCandidates)
            progress.phase = PhysicsRuntimeRetirementPhase::WaitingNativeCandidates;
        else if (progress.stateLeaseRecords)
            progress.phase = PhysicsRuntimeRetirementPhase::WaitingStateLeases;
        else if (!runtime->native.value)
            progress.phase = PhysicsRuntimeRetirementPhase::Drained;
        else
            return Outcome::Failure(MakeError(PhysicsErrors::InvalidState));
        return Outcome::Success(progress);
    }
}  // namespace Horo::Physics
