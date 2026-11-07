#include "CanonicalPhysicsStateFpControl.h"
#include "PhysicsWorldInternal.h"
#include "PhysicsWorldStateRestartAccess.h"

namespace Horo::Physics::Detail {
    /** @copydoc PhysicsWorldStateRestartAccess::DecodeRestartInput */
    Result<std::unique_ptr<StateDetail::RestartInput>> PhysicsWorldStateRestartAccess::DecodeRestartInput(
        const PhysicsWorld &world, std::span<const std::byte> bytes, std::size_t maximumWireBytes, std::size_t maximumRetainedBytes) {
        using Outcome = Result<std::unique_ptr<StateDetail::RestartInput>>;
        if (auto joined = InventoryCounts(world); joined.HasError())
            return Outcome::Failure(joined.ErrorValue());
        const auto &owner = *world.impl_;
        if (!owner.runtime->stateBuild)
            return Outcome::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        auto current = ReadCanonicalPhysicsStateFpControl();
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (current.Value() != owner.runtime->stateBuild->floatingPoint)
            return Outcome::Failure(MakeError(PhysicsErrors::ProfileUnsupported));
        auto limits = ReadCanonicalStateReceivingLimits(owner.native, owner.settings, maximumWireBytes, maximumRetainedBytes);
        if (limits.HasError())
            return Outcome::Failure(limits.ErrorValue());
        return StateDetail::RestartInput::Prepare(bytes, limits.Value(), current.Value());
    }
}  // namespace Horo::Physics::Detail
