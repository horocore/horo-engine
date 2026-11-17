#include "PhysicsStateRestoreControl.h"
#include "PhysicsStateSnapshotFields.h"
#include "PhysicsWorldSnapshotState.h"

namespace Horo::Physics::Detail {
    /** @copydoc PhysicsWorldSnapshotState::FixedRetainedPayloadBytes */
    std::size_t PhysicsWorldSnapshotState::FixedRetainedPayloadBytes() const noexcept {
        return sizeof(*this) + sizeof(PhysicsStateResourceRecord) + sizeof(PhysicsStateRestoreCandidate::Impl) +
               workspace->RetainedPayloadBytes() + CanonicalPhysicsStateResourceBytes(resources->native) +
               stagedEvents->RetainedPayloadBytes() + slots.capacity() * sizeof(slots.front()) +
               eventPairScratch.capacity() * sizeof(PhysicsEventProjectionPairState) +
               eventScratch.capacity() * sizeof(PhysicsEventRecord) + eventOrdinalScratch.capacity();
    }

    /** @copydoc PhysicsWorldSnapshotState::PreparePublicationStorage */
    Result<void> PhysicsWorldSnapshotState::PreparePublicationStorage(const PhysicsStateCaptureConfiguration &configuration) {
        const auto &budgets = world->settings.Values().budgets;
        stagedEvents =
            std::make_unique<PhysicsEventProjection>(budgets.maximumEvents, budgets.maximumInFlightPairs, budgets.eventOverflow, true);
        eventPairScratch.resize(budgets.maximumInFlightPairs);
        eventScratch.resize(budgets.maximumEvents);
        eventOrdinalScratch.resize(budgets.maximumEvents);
        slots.reserve(configuration.snapshotSlots);
        retainedPayloadBytes = FixedRetainedPayloadBytes();
        if (retainedPayloadBytes > configuration.maximumRetainedBytes)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        StateDetail::SnapshotHeader maximum;
        maximum.solverBytes = workspace->MaximumSolverWireBytes();
        maximum.constructionBytes = workspace->ConstructionBytes().size();
        maximum.pairCount = budgets.maximumInFlightPairs;
        maximum.eventCount = budgets.maximumEvents;
        std::size_t wireBytes{};
        if (!StateDetail::SnapshotRequiredWireSize(maximum, configuration.maximumRetainedBytes, wireBytes))
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        for (std::uint32_t index = 0; index < configuration.snapshotSlots; ++index) {
            auto slot = PrepareSlot(workspace->RecaptureStorage().limits, budgets.maximumInFlightPairs, budgets.maximumEvents, wireBytes,
                                    configuration.maximumRetainedBytes - retainedPayloadBytes, resources);
            if (slot.HasError())
                return Result<void>::Failure(slot.ErrorValue());
            retainedPayloadBytes += sizeof(PhysicsStateSnapshotSlot) + slot.Value()->snapshot->RetainedPayloadBytes();
            slots.push_back(std::move(slot).Value());
        }
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
