#pragma once
#include "CanonicalPhysicsStateSceneInventory.h"
#include "Horo/Physics/PhysicsWorld.h"
#include "PhysicsStateRestartInput.h"
#include "PhysicsStateRestartTypes.h"

namespace Horo::Physics::Detail {
    class PhysicsSceneStateTarget;
    class PhysicsSceneStateProjection;
    class PreparedPhysicsSceneCapture;
    class PreparedPhysicsSceneRestore;
    struct PhysicsWorldSnapshotState;

    /** @brief Non-installed cold reconstruction access issued only by the exact pending Scene Physics owner.
     * No published World borrow or worker entry is provided. The Scene retains complete pending ownership until
     * its actual final aggregate arm/publication; failure keeps its old private World unchanged.
     */
    class PhysicsWorldStateRestartAccess final {
        friend class PhysicsSceneStateTarget;
        friend class PhysicsSceneStateProjection;
        friend class PreparedPhysicsSceneCapture;
        friend class PreparedPhysicsSceneRestore;
        class PrivateNativeSelection;
        [[nodiscard]] static Result<void> JoinedPrivateOwner(const PhysicsWorld::Impl &);
        [[nodiscard]] static Result<void> ImmutableCompatibility(const PhysicsWorld::Impl &, const StateDetail::RestartInput &,
                                                                 const PhysicsStateCaptureConfiguration &);
        [[nodiscard]] static Result<std::unique_ptr<StateDetail::RestartInput>> DecodeRestartInput(const PhysicsWorld &,
                                                                                                   std::span<const std::byte>,
                                                                                                   std::size_t maximumWireBytes,
                                                                                                   std::size_t maximumRetainedBytes);
        [[nodiscard]] static Result<std::size_t> ConfigurationPayloadBytes(const PhysicsWorld &);
        [[nodiscard]] static Result<std::weak_ptr<PhysicsWorldSnapshotState>> ConfigurationOwner(const PhysicsWorld &);
        [[nodiscard]] static Result<PhysicsStateSnapshotLease> CaptureConfigured(const PhysicsWorld &,
                                                                                 const std::weak_ptr<PhysicsWorldSnapshotState> &);
        [[nodiscard]] static Result<void> ValidateConfigured(const PhysicsWorld &, const std::weak_ptr<PhysicsWorldSnapshotState> &,
                                                             std::uint64_t publicationRevision);
        [[nodiscard]] static Result<void> ValidateCaptureProvider(const PhysicsWorld::Impl &);
        [[nodiscard]] static bool CaptureIdle(const PhysicsWorld::Impl &) noexcept;
        [[nodiscard]] static bool CompleteCaptureTick(const PhysicsWorld::Impl &) noexcept;
        [[nodiscard]] static Result<bool> CaptureReady(const PhysicsWorld &);
        [[nodiscard]] static Result<CanonicalStateSceneCounts> InventoryCounts(const PhysicsWorld &);
        [[nodiscard]] static Result<void> CopyInventory(const PhysicsWorld &, CanonicalStateSceneStorage);
        [[nodiscard]] static Result<CanonicalRestartBindings> Reconstruct(PhysicsWorld &, const StateDetail::RestartInput &,
                                                                          std::span<const CanonicalRestartBodyRoute>,
                                                                          PhysicsStateCaptureConfiguration, const CancellationToken &);
    };
}  // namespace Horo::Physics::Detail
