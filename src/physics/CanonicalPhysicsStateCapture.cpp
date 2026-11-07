#include "CanonicalPhysicsStateCapture.h"

#include "CanonicalPhysicsRuntimeInternal.h"
#include "CanonicalPhysicsStateAllocator.h"
#include "CanonicalPhysicsStateDecode.h"
#include "CanonicalPhysicsStateIntent.h"

#include <limits>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief No capture can overlap native work or an unpublished structural body
         * group. */
        bool Joined(const CanonicalWorld *world) noexcept {
            return world && world->native.system && world->pendingBodyBatch.expired() && !world->contactRoute.load();
        }

        /** @brief Copies exact native solver active order without allocating another
         * Jolt Array. */
        std::span<const std::uint32_t> CaptureActiveOrder(const CanonicalWorld &world, std::span<std::uint32_t> output) noexcept {
            const auto count = world.native.system->GetNumActiveBodies(JPH::EBodyType::RigidBody);
            const auto *active = world.native.system->GetActiveBodiesUnsafe(JPH::EBodyType::RigidBody);
            for (std::uint32_t index = 0; index < count; ++index)
                output[index] = active[index].GetIndexAndSequenceNumber();
            return output.first(count);
        }
    }  // namespace

    /** @copydoc CaptureCanonicalPhysicsState */
    Result<PhysicsStateSolverView> CaptureCanonicalPhysicsState(const CanonicalWorldHandle handle, const PhysicsWorldSettings &settings,
                                                                const CanonicalPhysicsStateBindings &bindings,
                                                                StateDetail::SolverStorage &staging,
                                                                const CanonicalPhysicsStateCaptureScratch scratch) {
        const auto *world = static_cast<const CanonicalWorld *>(handle.value);
        if (!Joined(world))
            return Result<PhysicsStateSolverView>::Failure(MakeError(PhysicsErrors::InvalidState));
        auto bounds = ValidateCanonicalPhysicsStateScratch(handle, settings, staging, scratch);
        if (bounds.HasError())
            return Result<PhysicsStateSolverView>::Failure(bounds.ErrorValue());
        staging.used = {};
        CanonicalPhysicsStateArena arena{scratch.temporary};
        CanonicalPhysicsStateStream recorder{scratch.recorder};
        const auto active = CaptureActiveOrder(*world, scratch.activeBodies);
        {
            CanonicalPhysicsStateAllocationScope route{arena};
            if (!route.Admitted())
                return Result<PhysicsStateSolverView>::Failure(MakeError(PhysicsErrors::InvalidState));
            world->native.system->SaveState(recorder, JPH::EStateRecorderState::All);
        }
        if (scratch.metrics)
            *scratch.metrics = {arena.Allocations(), arena.UsedBytes(), arena.ForeignReallocations()};
        if (recorder.IsFailed())
            return Result<PhysicsStateSolverView>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        if (arena.ForeignReallocations() != 0)
            return Result<PhysicsStateSolverView>::Failure(MakeError(PhysicsErrors::OperationUnsupported,
                                                                     "Pinned Physics state capture unexpectedly required a "
                                                                     "foreign native reallocation."));
        auto captured = DecodeCanonicalPhysicsState(recorder, bindings, staging, active);
        if (captured.HasError())
            return captured;
        if (auto intent = CaptureCanonicalStateIntent(*world, bindings, staging, captured.Value()); intent.HasError())
            return Result<PhysicsStateSolverView>::Failure(intent.ErrorValue());
        staging.used = captured.Value();
        return captured;
    }
}  // namespace Horo::Physics::Detail
