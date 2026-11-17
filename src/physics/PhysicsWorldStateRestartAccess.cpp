#include "PhysicsWorldStateRestartAccess.h"

#include "CanonicalPhysicsStateRestart.h"
#include "PhysicsWorldSnapshotState.h"

namespace Horo::Physics::Detail {
    namespace {
        bool SameBuild(const CanonicalPhysicsStateBuildIdentity &actual, const PhysicsStateCompatibility &saved) noexcept {
            return actual.build == saved.build && actual.platform == saved.platform && actual.solver == saved.solver &&
                   actual.floatingPoint == saved.floatingPoint;
        }

    }  // namespace

    Result<void> PhysicsWorldStateRestartAccess::JoinedPrivateOwner(const PhysicsWorld::Impl &owner) {
        if (owner.runtime->ownerThread != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (owner.state != PhysicsWorldState::ActiveSolver || owner.runtime->state != PhysicsRuntimeState::Ready)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (owner.stepping || owner.commandCount || owner.queryBatch.pending || owner.snapshots)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (!owner.runtime->stateBuild || !owner.runtime->stateRecipes)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        return Result<void>::Success();
    }

    Result<void> PhysicsWorldStateRestartAccess::ImmutableCompatibility(const PhysicsWorld::Impl &owner,
                                                                        const StateDetail::RestartInput &input,
                                                                        const PhysicsStateCaptureConfiguration &configuration) {
        const auto &saved = input.Envelope().compatibility;
        if (!configuration.contentLease || configuration.content != saved.content || owner.settings.Identity() != saved.settings)
            return Result<void>::Failure(MakeError(PhysicsErrors::ProfileUnsupported));
        if (!SameBuild(*owner.runtime->stateBuild, saved))
            return Result<void>::Failure(MakeError(PhysicsErrors::ProfileUnsupported));
        return ValidatePhysicsStateCompatibility(saved);
    }

    /** @brief Only unpublished native selection changes before success. No live Scene binding table is touched.
     * A failed configuration/restore closes newly registered snapshot inventory before candidate destruction.
     */
    class PhysicsWorldStateRestartAccess::PrivateNativeSelection final {
    public:
        PrivateNativeSelection(PhysicsWorld::Impl &owner, CanonicalWorldHandle candidate) noexcept
            : owner_(owner), previous_(owner.native) {
            owner_.native = candidate;
        }

        PrivateNativeSelection(const PrivateNativeSelection &) = delete;
        PrivateNativeSelection &operator=(const PrivateNativeSelection &) = delete;

        ~PrivateNativeSelection() {
            if (!committed_) {
                RevokePhysicsWorldSnapshots(owner_.snapshots);
                DestroyCanonicalWorld(owner_.native);
                owner_.native = previous_;
            }
        }

        void Commit() noexcept {
            DestroyCanonicalWorld(previous_);
            committed_ = true;
        }

    private:
        PhysicsWorld::Impl &owner_;
        CanonicalWorldHandle previous_;
        bool committed_{};
    };

    namespace {
        Result<void> RestoreSelectedWorld(PhysicsWorld &world, const StateDetail::RestartInput &input,
                                          const CanonicalRestartBindings &bindings, PhysicsStateCaptureConfiguration configuration,
                                          const CancellationToken &cancellation) {
            configuration.bodies = bindings.bodies;
            configuration.shapes = bindings.shapes;
            configuration.constraints = bindings.constraints;
            if (auto configured = world.ConfigureStateSnapshots(configuration); configured.HasError())
                return Result<void>::Failure(configured.ErrorValue());
            auto decoded = world.DecodeStateSnapshot(input.Bytes());
            if (decoded.HasError())
                return Result<void>::Failure(decoded.ErrorValue());
            auto restored = world.PrepareStateRestore(std::move(decoded).Value(), cancellation);
            if (restored.HasError())
                return Result<void>::Failure(restored.ErrorValue());
            auto candidate = std::move(restored).Value();
            if (auto armed = candidate.ValidatePublication(); armed.HasError())
                return armed;
            // This is only the inner private World transaction. The pending Scene is still unexposed;
            // its final aggregate arm remains mandatory before the Scene's actual no-fail publication.
            candidate.Publish();
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc PhysicsWorldStateRestartAccess::ValidateCaptureProvider */
    Result<void> PhysicsWorldStateRestartAccess::ValidateCaptureProvider(const PhysicsWorld::Impl &owner) {
        if (owner.runtime->ownerThread != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (owner.runtime->mode == PhysicsRuntimeMode::Null)
            return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
        if (owner.state != PhysicsWorldState::ActiveSolver || owner.runtime->state != PhysicsRuntimeState::Ready)
            return Result<void>::Failure(MakeError(PhysicsErrors::InvalidState));
        if (!owner.native.value || !owner.runtime->stateBuild || !owner.runtime->stateRecipes || !owner.runtime->stateInventory)
            return Result<void>::Failure(MakeError(PhysicsErrors::CapabilityUnavailable));
        return Result<void>::Success();
    }

    /** @copydoc PhysicsWorldStateRestartAccess::CaptureIdle */
    bool PhysicsWorldStateRestartAccess::CaptureIdle(const PhysicsWorld::Impl &owner) noexcept {
        return !owner.stepping && owner.commandCount == 0 && !owner.queryBatch.pending && !HasPendingCanonicalSceneBodies(owner.native);
    }

    /** @copydoc PhysicsWorldStateRestartAccess::CompleteCaptureTick */
    bool PhysicsWorldStateRestartAccess::CompleteCaptureTick(const PhysicsWorld::Impl &owner) noexcept {
        const auto publication = owner.publication.Snapshot();
        return publication.completedTick != 0 && publication.transformTick == publication.completedTick &&
               publication.queryTick == publication.completedTick && publication.eventTick == publication.completedTick &&
               owner.queryEvents.events.PublishedTick() == publication.completedTick;
    }

    /** @copydoc PhysicsWorldStateRestartAccess::CaptureReady */
    Result<bool> PhysicsWorldStateRestartAccess::CaptureReady(const PhysicsWorld &world) {
        using Outcome = Result<bool>;
        if (!world.impl_)
            return Outcome::Failure(MakeError(PhysicsErrors::WorldInvalid));
        const auto &owner = *world.impl_;
        if (auto current = ValidateCaptureProvider(owner); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (!CaptureIdle(owner))
            return Outcome::Success(false);
        // Reuse the existing joined exact-serial owner collection; never collect beneath active native work.
        if (owner.snapshots) {
            owner.snapshots->CollectAbandonedRestore();
            if (owner.snapshots->pendingRestore)
                return Outcome::Success(false);
        }
        return Outcome::Success(CompleteCaptureTick(owner));
    }

    /** @copydoc PhysicsWorldStateRestartAccess::InventoryCounts */
    Result<CanonicalStateSceneCounts> PhysicsWorldStateRestartAccess::InventoryCounts(const PhysicsWorld &world) {
        using Outcome = Result<CanonicalStateSceneCounts>;
        if (!world.impl_)
            return Outcome::Failure(MakeError(PhysicsErrors::WorldInvalid));
        const auto &owner = *world.impl_;
        if (owner.runtime->ownerThread != std::this_thread::get_id())
            return Outcome::Failure(MakeError(PhysicsErrors::ThreadAffinityViolation));
        if (owner.stepping || owner.commandCount || owner.queryBatch.pending)
            return Outcome::Failure(MakeError(PhysicsErrors::InvalidState));
        return InspectCanonicalStateSceneInventory(owner.native);
    }

    /** @copydoc PhysicsWorldStateRestartAccess::CopyInventory */
    Result<void> PhysicsWorldStateRestartAccess::CopyInventory(const PhysicsWorld &world, CanonicalStateSceneStorage storage) {
        if (auto checked = InventoryCounts(world); checked.HasError())
            return Result<void>::Failure(checked.ErrorValue());
        return CopyCanonicalStateSceneInventory(world.impl_->native, storage);
    }

    /** @copydoc PhysicsWorldStateRestartAccess::Reconstruct */
    Result<CanonicalRestartBindings> PhysicsWorldStateRestartAccess::Reconstruct(PhysicsWorld &world,
                                                                                 const StateDetail::RestartInput &input,
                                                                                 std::span<const CanonicalRestartBodyRoute> routes,
                                                                                 PhysicsStateCaptureConfiguration configuration,
                                                                                 const CancellationToken &cancellation) {
        using Outcome = Result<CanonicalRestartBindings>;
        if (!world.impl_)
            return Outcome::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &owner = *world.impl_;
        if (auto joined = JoinedPrivateOwner(owner); joined.HasError())
            return Outcome::Failure(joined.ErrorValue());
        if (auto compatible = ImmutableCompatibility(owner, input, configuration); compatible.HasError())
            return Outcome::Failure(compatible.ErrorValue());
        CanonicalRestartBindings bindings;
        auto native = ReconstructCanonicalPhysicsStateTopology(owner.native, owner.identity, owner.settings, input.Construction(),
                                                               input.ConstructionLimits(), routes,
                                                               input.Envelope().world.continuousCollision, cancellation, bindings);
        if (native.HasError())
            return Outcome::Failure(native.ErrorValue());
        PrivateNativeSelection selection{owner, native.Value()};
        if (auto restored = RestoreSelectedWorld(world, input, bindings, std::move(configuration), cancellation); restored.HasError())
            return Outcome::Failure(restored.ErrorValue());
        selection.Commit();
        return Outcome::Success(std::move(bindings));
    }
}  // namespace Horo::Physics::Detail
