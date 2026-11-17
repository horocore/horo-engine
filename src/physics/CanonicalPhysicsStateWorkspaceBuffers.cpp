#include "CanonicalPhysicsStateBackendInternal.h"
#include "PhysicsStateSolverCodecInternal.h"

namespace Horo::Physics::Detail {
    /** @copydoc CanonicalPhysicsStateWorkspace::Impl::PrepareBuffers */
    Result<void> CanonicalPhysicsStateWorkspace::Impl::PrepareBuffers(const CanonicalPhysicsStateBounds &bounds, std::size_t maximumBytes) {
        const auto &capacity = settings.Values().world.capacity;
        PhysicsStateLimits limits{capacity.maximumBodies,
                                  capacity.maximumConstraints,
                                  static_cast<std::uint32_t>(bounds.pairEntries),
                                  static_cast<std::uint32_t>(bounds.manifoldEntries),
                                  static_cast<std::uint32_t>(bounds.manifoldEntries),
                                  maximumBytes};
        auto prepared = StateDetail::PrepareSolverStorage(limits);
        if (prepared.HasError())
            return Result<void>::Failure(prepared.ErrorValue());
        StateDetail::SolverHeader maximum;
        maximum.counts = {limits.maximumBodies, limits.maximumBodies,    limits.maximumConstraints,
                          limits.maximumPairs,  limits.maximumManifolds, limits.maximumCcdManifolds};
        std::size_t wireBytes{};
        if (!StateDetail::WireSize(maximum, maximumBytes, wireBytes))
            return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        recapture = std::move(prepared).Value();
        temporary.resize(bounds.temporaryBytes);
        recorder.resize(bounds.recorderBytes);
        active.resize(capacity.maximumBodies);
        expectedWire.resize(wireBytes);
        actualWire.resize(wireBytes);
        return Result<void>::Success();
    }
}  // namespace Horo::Physics::Detail
