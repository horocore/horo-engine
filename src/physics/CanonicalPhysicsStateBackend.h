#pragma once

#include "CanonicalPhysicsRuntime.h"
#include "PhysicsStateSolverStorage.h"

namespace Horo::Physics::Detail {
    class CanonicalPhysicsStateResources;

    /** @brief Exact native shape/schema/surface lease payload metadata; no
     * caller-created replacement authority. */
    [[nodiscard]] std::size_t CanonicalPhysicsStateResourceBytes(
        const std::shared_ptr<const CanonicalPhysicsStateResources> &resources) noexcept;

    /** @brief Vendor-free private workspace seam; native-OFF implements typed
     * unsupported operations. */
    class CanonicalPhysicsStateWorkspace final {
        struct Impl;

    public:
        ~CanonicalPhysicsStateWorkspace();
        CanonicalPhysicsStateWorkspace(const CanonicalPhysicsStateWorkspace &) = delete;
        CanonicalPhysicsStateWorkspace &operator=(const CanonicalPhysicsStateWorkspace &) = delete;
        [[nodiscard]] static Result<std::unique_ptr<CanonicalPhysicsStateWorkspace>> Prepare(
            CanonicalWorldHandle world, const PhysicsWorldSettings &settings,
            const PhysicsStateCaptureConfiguration &configuration) noexcept;
        [[nodiscard]] Result<PhysicsStateSolverView> Capture(CanonicalWorldHandle world, StateDetail::SolverStorage &storage);
        [[nodiscard]] Result<void> ValidateState(const PhysicsStateSolverView &state,
                                                 const PhysicsContinuousCollisionPolicy &continuousCollision);
        [[nodiscard]] Result<CanonicalWorldHandle> Restore(CanonicalWorldHandle world, const PhysicsStateSolverView &state,
                                                           const PhysicsContinuousCollisionPolicy &continuousCollision);
        /** @brief Exact receiving owner epochs, kept separate from durable semantic compatibility. */
        [[nodiscard]] Result<void> ValidateOwnerEpochs(CanonicalWorldHandle) const;
        [[nodiscard]] Result<Sha256Digest> TopologyFingerprint(CanonicalWorldHandle world) const;
        [[nodiscard]] Sha256Digest FilterFingerprint() const noexcept;
        [[nodiscard]] Sha256Digest SurfaceFingerprint() const noexcept;
        [[nodiscard]] const StateDetail::SolverStorage &RecaptureStorage() const noexcept;
        [[nodiscard]] std::span<const PhysicsStateShapeBinding> Shapes() const noexcept;
        [[nodiscard]] std::span<const PhysicsStateBodyBinding> Bodies() const noexcept;
        [[nodiscard]] PhysicsStateObjectId BodyId(BodyHandle body) const noexcept;
        [[nodiscard]] PhysicsStateObjectId ShapeId(ShapeHandle shape) const noexcept;
        [[nodiscard]] std::span<std::byte> SolverWireScratch() noexcept;
        /** @brief Rebinds only exact admitted collider/profile/material evidence; serialized IDs confer no new authority. */
        [[nodiscard]] Result<PhysicsEventEndpoint> ResolveEventEndpoint(const PhysicsStateEventEndpoint &endpoint,
                                                                        const std::optional<PhysicsEventMaterial> &material,
                                                                        const std::optional<PhysicsStateSurfaceEvidence> &surface) const;
        /** @brief Reissues real live metadata only through this exact current owner; state bytes grant no catalogue/slot authority. */
        [[nodiscard]] Result<std::optional<PhysicsSurfaceEvidence>> ResolveStateSurface(
            const std::optional<PhysicsStateSurfaceEvidence> &) const;
        [[nodiscard]] std::shared_ptr<const CanonicalPhysicsStateResources> Resources() const noexcept;
        [[nodiscard]] std::size_t RetainedPayloadBytes() const noexcept;
        [[nodiscard]] std::size_t MaximumSolverWireBytes() const noexcept;
        /** @brief Immutable SAME canonical-codec construction subsection, captured cold from actual retained inputs. */
        [[nodiscard]] std::span<const std::byte> ConstructionBytes() const noexcept;
        /** @brief Checks the exact retained native pair and monotonic audit revision
         * before the no-fail owner swap. */
        [[nodiscard]] Result<void> ValidateRestoredOwnerSwap(CanonicalWorldHandle previous, CanonicalWorldHandle restored) const;
        /** @brief Rebinds the same validated identity inventory after the existing
         * owner publishes its private restored solver. */
        void AdoptRestoredOwner(CanonicalWorldHandle previous, CanonicalWorldHandle restored) noexcept;

    private:
        explicit CanonicalPhysicsStateWorkspace(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };
}  // namespace Horo::Physics::Detail
