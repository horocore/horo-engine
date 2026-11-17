#include "PhysicsStateEnvelope.h"
#include "PhysicsWorldSnapshotState.h"
#include "PhysicsWorldStateEvents.h"

namespace Horo::Physics::Detail {
    namespace {
        bool Complete(const PhysicsPublishedTick &value) noexcept {
            return value.completedTick != 0 && value.transformTick == value.completedTick && value.queryTick == value.completedTick &&
                   value.eventTick == value.completedTick;
        }
    }  // namespace

    /** @copydoc PhysicsWorldSnapshotState::CaptureSlot */
    Result<void> PhysicsWorldSnapshotState::CaptureSlot(PhysicsStateSnapshot::Impl &value) {
        const auto publication = world->publication.Snapshot();
        const auto events = world->queryEvents.events.ReadState();
        if (!Complete(publication) || events.tick != publication.completedTick)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        auto solver = workspace->Capture(world->native, *value.solver);
        if (solver.HasError())
            return Result<void>::Failure(solver.ErrorValue());
        auto policy = ReadCanonicalContinuousCollision(world->native);
        if (policy.HasError())
            return Result<void>::Failure(policy.ErrorValue());
        auto copied = CapturePhysicsStateEvents(*workspace, events, value.pairs, value.events);
        if (copied.HasError())
            return Result<void>::Failure(copied.ErrorValue());
        value.compatibility = compatibility;
        value.world = {publication.completedTick,
                       policy.Value().policy,
                       publication.appliedCommands,
                       publication.droppedEventCount,
                       std::span<const PhysicsStateEventPair>{value.pairs}.first(copied.Value().pairs),
                       std::span<const PhysicsStatePublishedEvent>{value.events}.first(copied.Value().events)};
        auto solverWire = workspace->SolverWireScratch();
        auto encoded = StateDetail::EncodeSolverState(solver.Value(), value.solver->limits, value.solver->activeBodyScratch, solverWire);
        if (encoded.HasError())
            return Result<void>::Failure(encoded.ErrorValue());
        auto envelope = StateDetail::EncodeSnapshotEnvelope(value.compatibility, solverWire.first(encoded.Value()), value.world, value.wire,
                                                            eventOrdinalScratch, workspace->ConstructionBytes());
        if (envelope.HasError())
            return Result<void>::Failure(envelope.ErrorValue());
        value.usedWireBytes = envelope.Value();
        value.construction = std::span<const std::byte>{value.wire}.subspan(StateDetail::SnapshotHeader::Bytes + encoded.Value(),
                                                                            workspace->ConstructionBytes().size());
        return Result<void>::Success();
    }

    /** @copydoc PhysicsWorldSnapshotState::Capture */
    Result<PhysicsStateSnapshotLease> PhysicsWorldSnapshotState::Capture() {
        using Outcome = Result<PhysicsStateSnapshotLease>;
        if (auto current = CheckCurrentWorld(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        CollectAbandonedRestore();
        auto slot = SealSlot();
        if (slot.HasError())
            return Outcome::Failure(slot.ErrorValue());
        PhysicsSnapshotWriteGuard writing{*slot.Value()};
        if (auto captured = CaptureSlot(*slot.Value()->snapshot->impl_); captured.HasError())
            return Outcome::Failure(captured.ErrorValue());
        auto lease = Publish(slot.Value());
        writing.Release();
        return Outcome::Success(std::move(lease));
    }
}  // namespace Horo::Physics::Detail
