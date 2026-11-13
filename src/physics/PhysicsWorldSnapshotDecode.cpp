#include "PhysicsStateEnvelope.h"
#include "PhysicsWorldSnapshotState.h"

#include <cstring>

namespace Horo::Physics::Detail {
    /** @copydoc PhysicsWorldSnapshotState::Decode */
    Result<PhysicsStateSnapshotLease> PhysicsWorldSnapshotState::Decode(std::span<const std::byte> bytes) {
        using Outcome = Result<PhysicsStateSnapshotLease>;
        if (auto current = CheckCurrentWorld(); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        CollectAbandonedRestore();
        if (pendingRestore)
            return Outcome::Failure(MakeError(PhysicsErrors::InvalidState));
        auto slot = SealSlot();
        if (slot.HasError())
            return Outcome::Failure(slot.ErrorValue());
        PhysicsSnapshotWriteGuard writing{*slot.Value()};
        auto &value = *slot.Value()->snapshot->impl_;
        if (bytes.size() > value.wire.size())
            return Outcome::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        auto decoded = StateDetail::DecodeSnapshotEnvelope(bytes, *value.solver, value.pairs, value.events, eventOrdinalScratch);
        if (decoded.HasError())
            return Outcome::Failure(decoded.ErrorValue());
        value.compatibility = decoded.Value().compatibility;
        value.world = decoded.Value().world;
        value.construction = decoded.Value().construction;
        if (auto admitted = ValidateSnapshot(*slot.Value()->snapshot); admitted.HasError())
            return Outcome::Failure(admitted.ErrorValue());
        const auto constructionOffset = static_cast<std::size_t>(decoded.Value().construction.data() - bytes.data());
        std::memmove(value.wire.data(), bytes.data(), bytes.size());
        value.construction = std::span<const std::byte>{value.wire}.subspan(constructionOffset, decoded.Value().construction.size());
        value.usedWireBytes = bytes.size();
        auto lease = Publish(slot.Value());
        writing.Release();
        return Outcome::Success(std::move(lease));
    }
}  // namespace Horo::Physics::Detail
