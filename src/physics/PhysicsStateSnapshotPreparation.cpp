#include "PhysicsStateValidationInternal.h"
#include "PhysicsWorldSnapshotState.h"

#include <new>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Full fixed slot and every vector prefix must fit before any
         * allocation starts. */
        bool SlotFits(const PhysicsStateLimits &limits, std::uint32_t pairs, std::uint32_t events, std::size_t wire, std::size_t budget,
                      std::size_t fixed) noexcept {
            if (!StateDetail::ValidStateLimits(limits) || pairs > MaximumPhysicsResourceRecords || events > MaximumPhysicsBufferEntries)
                return false;
            const std::array<std::pair<std::size_t, std::size_t>, 9> costs{
                {{1, fixed},
                 {limits.maximumBodies, sizeof(PhysicsStateBody) + sizeof(PhysicsStateObjectId) + 1},
                 {limits.maximumConstraints, sizeof(PhysicsStateConstraint)},
                 {limits.maximumPairs, sizeof(PhysicsStateBodyPair)},
                 {limits.maximumManifolds, sizeof(PhysicsStateManifold)},
                 {limits.maximumCcdManifolds, sizeof(PhysicsStateManifoldKey)},
                 {pairs, sizeof(PhysicsStateEventPair)},
                 {events, sizeof(PhysicsStatePublishedEvent)},
                 {wire, 1}}};
            for (const auto &[count, size] : costs) {
                if (count > budget / size)
                    return false;
                budget -= count * size;
            }
            return true;
        }
    }  // namespace

    /** @copydoc PhysicsWorldSnapshotState::PrepareSlot */
    Result<std::shared_ptr<PhysicsStateSnapshotSlot>> PhysicsWorldSnapshotState::PrepareSlot(
        const PhysicsStateLimits &limits, std::uint32_t maximumPairs, std::uint32_t maximumEvents, std::size_t wireBytes,
        std::size_t maximumPayloadBytes, const std::shared_ptr<PhysicsStateResourceRecord> &resources) {
        using Outcome = Result<std::shared_ptr<PhysicsStateSnapshotSlot>>;
        constexpr auto fixed = sizeof(PhysicsStateSnapshotSlot) + sizeof(PhysicsStateSnapshot) + sizeof(PhysicsStateSnapshot::Impl) +
                               sizeof(StateDetail::SolverStorage);
        if (!resources || !SlotFits(limits, maximumPairs, maximumEvents, wireBytes, maximumPayloadBytes, fixed))
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        try {
            auto solver = StateDetail::PrepareSolverStorage(limits);
            if (solver.HasError())
                return Outcome::Failure(solver.ErrorValue());
            auto value = std::make_unique<PhysicsStateSnapshot::Impl>();
            value->solver = std::move(solver).Value();
            value->pairs.resize(maximumPairs);
            value->events.resize(maximumEvents);
            value->wire.resize(wireBytes);
            auto slot = std::make_shared<PhysicsStateSnapshotSlot>();
            slot->resources = resources;
            slot->snapshot = std::unique_ptr<PhysicsStateSnapshot>{new PhysicsStateSnapshot{std::move(value)}};
            if (sizeof(*slot) + slot->snapshot->RetainedPayloadBytes() > maximumPayloadBytes)
                return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            return Outcome::Success(std::move(slot));
        } catch (const std::bad_alloc &) {
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }
    }
}  // namespace Horo::Physics::Detail
