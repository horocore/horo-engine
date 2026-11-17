#include "CanonicalPhysicsStateBackend.h"
#include "CanonicalPhysicsStateBuild.h"
#include "CanonicalPhysicsStateFpControl.h"
#include "CanonicalPhysicsStateRestart.h"
#include "CanonicalPhysicsStateSceneInventory.h"
#include "Horo/Physics/PhysicsErrors.h"

#include <exception>

namespace Horo::Physics::Detail {
    struct CanonicalPhysicsStateWorkspace::Impl final {};

    /** @copydoc PrepareCanonicalPhysicsStateBuild */
    Result<CanonicalPhysicsStateBuildIdentity> PrepareCanonicalPhysicsStateBuild(const PhysicsStateBuildBinding &) {
        return Result<CanonicalPhysicsStateBuildIdentity>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::Prepare */
    Result<std::unique_ptr<CanonicalPhysicsStateWorkspace>> CanonicalPhysicsStateWorkspace::Prepare(
        CanonicalWorldHandle, const PhysicsWorldSettings &, const PhysicsStateCaptureConfiguration &) noexcept {
        return Result<std::unique_ptr<CanonicalPhysicsStateWorkspace>>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::Capture */
    Result<PhysicsStateSolverView> CanonicalPhysicsStateWorkspace::Capture(CanonicalWorldHandle, StateDetail::SolverStorage &) {
        return Result<PhysicsStateSolverView>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
    }

    /** @copydoc ReadCanonicalPhysicsStateFpControl */
    Result<PhysicsStateFpControl> ReadCanonicalPhysicsStateFpControl() {
        return Result<PhysicsStateFpControl>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::ValidateState */
    Result<void> CanonicalPhysicsStateWorkspace::ValidateState(const PhysicsStateSolverView &, const PhysicsContinuousCollisionPolicy &) {
        return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::ResolveEventEndpoint */
    Result<PhysicsEventEndpoint> CanonicalPhysicsStateWorkspace::ResolveEventEndpoint(
        const PhysicsStateEventEndpoint &, const std::optional<PhysicsEventMaterial> &,
        const std::optional<PhysicsStateSurfaceEvidence> &) const {
        return Result<PhysicsEventEndpoint>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::BodyId */
    PhysicsStateObjectId CanonicalPhysicsStateWorkspace::BodyId(BodyHandle) const noexcept {
        return 0;
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::ShapeId */
    PhysicsStateObjectId CanonicalPhysicsStateWorkspace::ShapeId(ShapeHandle) const noexcept {
        return 0;
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::SolverWireScratch */
    std::span<std::byte> CanonicalPhysicsStateWorkspace::SolverWireScratch() noexcept {
        return {};
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::Restore */
    Result<CanonicalWorldHandle> CanonicalPhysicsStateWorkspace::Restore(CanonicalWorldHandle, const PhysicsStateSolverView &,
                                                                         const PhysicsContinuousCollisionPolicy &) {
        return Result<CanonicalWorldHandle>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::TopologyFingerprint */
    Result<Sha256Digest> CanonicalPhysicsStateWorkspace::TopologyFingerprint(CanonicalWorldHandle) const {
        return Result<Sha256Digest>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::ValidateRestoredOwnerSwap */
    Result<void> CanonicalPhysicsStateWorkspace::ValidateRestoredOwnerSwap(CanonicalWorldHandle, CanonicalWorldHandle) const {
        return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::CanonicalPhysicsStateWorkspace */
    CanonicalPhysicsStateWorkspace::CanonicalPhysicsStateWorkspace(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    /** @copydoc CanonicalPhysicsStateWorkspace::~CanonicalPhysicsStateWorkspace */
    CanonicalPhysicsStateWorkspace::~CanonicalPhysicsStateWorkspace() = default;

    /** @copydoc CanonicalPhysicsStateWorkspace::FilterFingerprint */
    Sha256Digest CanonicalPhysicsStateWorkspace::FilterFingerprint() const noexcept {
        return {};
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::SurfaceFingerprint */
    Sha256Digest CanonicalPhysicsStateWorkspace::SurfaceFingerprint() const noexcept {
        return {};
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::Shapes */
    std::span<const PhysicsStateShapeBinding> CanonicalPhysicsStateWorkspace::Shapes() const noexcept {
        return {};
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::Bodies */
    std::span<const PhysicsStateBodyBinding> CanonicalPhysicsStateWorkspace::Bodies() const noexcept {
        return {};
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::Resources */
    std::shared_ptr<const CanonicalPhysicsStateResources> CanonicalPhysicsStateWorkspace::Resources() const noexcept {
        return {};
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::RetainedPayloadBytes */
    std::size_t CanonicalPhysicsStateWorkspace::RetainedPayloadBytes() const noexcept {
        return 0;
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::ConstructionBytes */
    std::span<const std::byte> CanonicalPhysicsStateWorkspace::ConstructionBytes() const noexcept {
        return {};
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::MaximumSolverWireBytes */
    std::size_t CanonicalPhysicsStateWorkspace::MaximumSolverWireBytes() const noexcept {
        return 0;
    }

    /** @copydoc CanonicalPhysicsStateResourceBytes */
    std::size_t CanonicalPhysicsStateResourceBytes(const std::shared_ptr<const CanonicalPhysicsStateResources> &) noexcept {
        return 0;
    }

    /** @brief Unreachable invariant: omitted backend never creates a workspace and
     * grants no getter/publication authority. */
    const StateDetail::SolverStorage &CanonicalPhysicsStateWorkspace::RecaptureStorage() const noexcept {
        std::terminate();
    }

    /** @brief Unreachable invariant: omitted Prepare/Validate can never reach a
     * publication fence. */
    void CanonicalPhysicsStateWorkspace::AdoptRestoredOwner(CanonicalWorldHandle, CanonicalWorldHandle) noexcept {
        std::terminate();
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::ValidateOwnerEpochs */
    Result<void> CanonicalPhysicsStateWorkspace::ValidateOwnerEpochs(CanonicalWorldHandle) const {
        return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
    }

    /** @copydoc CanonicalPhysicsStateWorkspace::ResolveStateSurface */
    Result<std::optional<PhysicsSurfaceEvidence>> CanonicalPhysicsStateWorkspace::ResolveStateSurface(
        const std::optional<PhysicsStateSurfaceEvidence> &) const {
        return Result<std::optional<PhysicsSurfaceEvidence>>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
    }

    /** @copydoc ReconstructCanonicalPhysicsStateTopology */
    Result<CanonicalWorldHandle> ReconstructCanonicalPhysicsStateTopology(
        CanonicalWorldHandle, PhysicsWorldId, const PhysicsWorldSettings &, const PhysicsStateConstructionManifest &,
        const PhysicsStateConstructionManifestLimits &, std::span<const CanonicalRestartBodyRoute>,
        const PhysicsContinuousCollisionPolicy &, const CancellationToken &, CanonicalRestartBindings &) {
        return Result<CanonicalWorldHandle>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
    }

    /** @copydoc InspectCanonicalStateSceneInventory */
    Result<CanonicalStateSceneCounts> InspectCanonicalStateSceneInventory(CanonicalWorldHandle) {
        return Result<CanonicalStateSceneCounts>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
    }

    /** @copydoc CopyCanonicalStateSceneInventory */
    Result<void> CopyCanonicalStateSceneInventory(CanonicalWorldHandle, CanonicalStateSceneStorage) {
        return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
    }

    /** @copydoc ReadCanonicalStateReceivingLimits */
    Result<StateDetail::RestartInputLimits> ReadCanonicalStateReceivingLimits(CanonicalWorldHandle, const PhysicsWorldSettings &,
                                                                              std::size_t, std::size_t) {
        return Result<StateDetail::RestartInputLimits>::Failure(MakeError(PhysicsErrors::OperationUnsupported));
    }
}  // namespace Horo::Physics::Detail
