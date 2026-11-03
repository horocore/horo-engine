#include "CanonicalPhysicsStateRestore.h"

#include "CanonicalPhysicsStateAllocator.h"
#include "CanonicalPhysicsStateEncode.h"
#include "CanonicalPhysicsStateIntent.h"
#include "CanonicalPhysicsStateRestoreScratch.h"
#include "CanonicalPhysicsStateTopology.h"

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Restore preparation has the same joined native boundary as capture.
         */
        bool Joined(const CanonicalWorld *world) noexcept {
            return world && world->native.system && world->pendingBodyBatch.expired() && !world->contactRoute.load();
        }

        /** @brief Rebuilds the ACTUAL active order before native restore so activation
         * sleep resets are overwritten by captured sleep state. */
        Result<void> PrepareActivity(CanonicalWorld &candidate, const CanonicalPhysicsStateBindings &bindings,
                                     const PhysicsStateSolverView &state) {
            for (auto id : state.activeBodyOrder)
                if (!FindStableStateBody(bindings, id))
                    return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
            auto &interface = candidate.native.system->GetBodyInterface();
            const auto count = candidate.native.system->GetNumActiveBodies(JPH::EBodyType::RigidBody);
            for (std::uint32_t remaining = count; remaining != 0; --remaining) {
                const auto id = candidate.native.system->GetActiveBodiesUnsafe(JPH::EBodyType::RigidBody)[remaining - 1];
                interface.DeactivateBody(id);
            }
            for (auto id : state.activeBodyOrder)
                interface.ActivateBody(JPH::BodyID{FindStableStateBody(bindings, id)->nativeId});
            return Result<void>::Success();
        }

        /** @brief Complete typed validation precedes any candidate solver allocation or
         * native mutation. */
        Result<std::size_t> ValidateInput(CanonicalWorld &source, const PhysicsWorldSettings &settings,
                                          const CanonicalPhysicsStateBindings &bindings, const PhysicsStateSolverView &state,
                                          const PhysicsContinuousCollisionPolicy &policy, CanonicalPhysicsStateRestoreScratch &scratch) {
            if (auto native = ValidateCanonicalPhysicsStateScratch({&source}, settings, scratch.recapture, scratch.native);
                native.HasError())
                return Result<std::size_t>::Failure(native.ErrorValue());
            if (!CanonicalStateRestoreScratchDisjoint(state, scratch))
                return Result<std::size_t>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Physics restore scratch aliases immutable state."));
            if (auto typed = ValidatePhysicsStateSolver(state, scratch.recapture.limits, scratch.recapture.activeBodyScratch);
                typed.HasError())
                return Result<std::size_t>::Failure(typed.ErrorValue());
            if (auto topology = ValidateCanonicalPhysicsStateTopology(source, bindings, state, settings); topology.HasError())
                return Result<std::size_t>::Failure(topology.ErrorValue());
            if (auto ccd = ValidateCanonicalContinuousCollision({&source}, policy); ccd.HasError())
                return Result<std::size_t>::Failure(ccd.ErrorValue());
            if (auto intent = ValidateCanonicalStateIntent(source, bindings, state); intent.HasError())
                return Result<std::size_t>::Failure(intent.ErrorValue());
            return StateDetail::EncodeSolverState(state, scratch.recapture.limits, scratch.recapture.activeBodyScratch,
                                                  scratch.expectedWire);
        }

        /** @brief No public wire data is ever handed directly to native RestoreState.
         */
        Result<void> Restore(CanonicalWorld &candidate, const CanonicalPhysicsStateBindings &bindings, const PhysicsStateSolverView &state,
                             const PhysicsContinuousCollisionPolicy &policy, CanonicalPhysicsStateCaptureScratch scratch) {
            if (auto ccd = ApplyCanonicalContinuousCollision({&candidate}, policy); ccd.HasError())
                return ccd;
            if (auto intent = ApplyCanonicalStateIntent(candidate, bindings, state); intent.HasError())
                return intent;
            if (auto active = PrepareActivity(candidate, bindings, state); active.HasError())
                return active;
            CanonicalPhysicsStateStream native{scratch.recorder};
            if (auto encoded = EncodeCanonicalPhysicsState(native, bindings, state); encoded.HasError())
                return encoded;
            CanonicalPhysicsStateArena arena{scratch.temporary};
            bool restored{};
            {
                CanonicalPhysicsStateAllocationScope route{arena};
                if (!route.Admitted())
                    return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
                restored = candidate.native.system->RestoreState(native);
            }
            if (arena.ForeignReallocations() != 0)
                return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported,
                                                       "Pinned Physics restore unexpectedly required a foreign "
                                                       "native reallocation."));
            if (!restored || native.IsFailed() || !native.IsEOF())
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Native Physics candidate rejected complete typed restore."));
            return Result<void>::Success();
        }

        /** @brief Exact canonical recapture detects dropped hidden fields, incorrect
         * native IDs and wrong active-body ordering. */
        Result<void> Verify(CanonicalWorld &candidate, const PhysicsWorldSettings &settings, const CanonicalPhysicsStateBindings &bindings,
                            CanonicalPhysicsStateRestoreScratch &scratch, std::size_t expectedBytes) {
            auto captured = CaptureCanonicalPhysicsState({&candidate}, settings, bindings, scratch.recapture, scratch.native);
            if (captured.HasError())
                return Result<void>::Failure(captured.ErrorValue());
            auto encoded = StateDetail::EncodeSolverState(captured.Value(), scratch.recapture.limits, scratch.recapture.activeBodyScratch,
                                                          scratch.actualWire);
            if (encoded.HasError())
                return Result<void>::Failure(encoded.ErrorValue());
            if (encoded.Value() != expectedBytes ||
                !std::ranges::equal(scratch.expectedWire.first(expectedBytes), scratch.actualWire.first(encoded.Value())))
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Native Physics candidate canonical recapture diverged."));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PrepareCanonicalPhysicsStateRestore */
    Result<CanonicalWorldHandle> PrepareCanonicalPhysicsStateRestore(const CanonicalWorldHandle handle, const CanonicalWorldHandle retained,
                                                                     const PhysicsWorldSettings &settings,
                                                                     const CanonicalPhysicsStateBindings &bindings,
                                                                     const PhysicsStateSolverView &state,
                                                                     const PhysicsContinuousCollisionPolicy &continuousCollision,
                                                                     CanonicalPhysicsStateRestoreScratch scratch) {
        auto *source = static_cast<CanonicalWorld *>(handle.value);
        auto *candidate = static_cast<CanonicalWorld *>(retained.value);
        if (!Joined(source) || !Joined(candidate) || source == candidate || &source->owner != &candidate->owner)
            return Result<CanonicalWorldHandle>::Failure(MakeError(PhysicsErrors::InvalidState));
        auto input = ValidateInput(*source, settings, bindings, state, continuousCollision, scratch);
        if (input.HasError())
            return Result<CanonicalWorldHandle>::Failure(input.ErrorValue());
        if (auto restored = Restore(*candidate, bindings, state, continuousCollision, scratch.native); restored.HasError())
            return Result<CanonicalWorldHandle>::Failure(restored.ErrorValue());
        if (auto verified = Verify(*candidate, settings, bindings, scratch, input.Value()); verified.HasError())
            return Result<CanonicalWorldHandle>::Failure(verified.ErrorValue());
        return Result<CanonicalWorldHandle>::Success(retained);
    }
}  // namespace Horo::Physics::Detail
