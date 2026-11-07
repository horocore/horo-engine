#pragma once

#include "CanonicalPhysicsStateBackend.h"
#include "CanonicalPhysicsStateBindingInternal.h"
#include "CanonicalPhysicsStateCapture.h"
#include "CanonicalPhysicsStateRestore.h"
#include "CanonicalPhysicsStateTopologyMap.h"
#include "PhysicsStateConstructionManifestOwner.h"

namespace Horo::Physics::Detail {
    /** @brief Real retained native resources. Outer Physics runtime lease keeps
     * globals/code valid until these references retire. */
    class CanonicalPhysicsStateResources final {
    public:
        std::vector<JPH::RefConst<JPH::Shape>> shapes;
        std::size_t retainedBytes{};
        PhysicsWorldSimulationBinding simulation;
    };

    struct CanonicalPhysicsStateWorkspace::Impl final {
        explicit Impl(const PhysicsWorldSettings &source) : settings(source) {}

        PhysicsWorldSettings settings;
        CanonicalWorldHandle owner;
        std::unique_ptr<CanonicalWorld> candidate; /**< Cold prepared once; after publication owns the previous
                                                      native world. */
        CanonicalPhysicsStateBindingStorage bindings;
        CanonicalPhysicsStateTopologyMap ownerMap, candidateMap;
        std::vector<PhysicsStateShapeBinding> shapes;
        std::vector<PhysicsStateBodyBinding> bodies, bodiesByHandle;
        std::shared_ptr<CanonicalPhysicsStateResources> resources;
        Sha256Digest filters, surfaces;
        std::uint64_t filterEpoch{}, surfaceEpoch{};
        std::unique_ptr<StateDetail::SolverStorage> recapture;
        std::unique_ptr<StateDetail::ConstructionManifestOwner> construction;
        std::vector<std::byte> temporary, recorder, expectedWire, actualWire;
        std::vector<std::uint32_t> active;
        std::size_t candidateReservation{};

        [[nodiscard]] Result<void> PrepareInventories(const CanonicalWorld &world, const PhysicsStateCaptureConfiguration &configuration);
        [[nodiscard]] Result<void> PrepareBuffers(const CanonicalPhysicsStateBounds &bounds, std::size_t maximumBytes);
        [[nodiscard]] Result<void> PreparePayloads(const CanonicalWorld &, const PhysicsStateCaptureConfiguration &,
                                                   const CanonicalPhysicsStateBounds &);
        [[nodiscard]] Result<void> PrepareCandidate(const CanonicalWorld &world, std::size_t maximumBytes);

        [[nodiscard]] CanonicalPhysicsStateCaptureScratch CaptureScratch() noexcept {
            return {temporary, recorder, active};
        }

        [[nodiscard]] CanonicalPhysicsStateRestoreScratch RestoreScratch() noexcept {
            return {CaptureScratch(), expectedWire, actualWire, *recapture};
        }
    };

    /** @brief Pure actual-field fingerprint, independent of transient pointers or
     * runtime world/body IDs. */
    [[nodiscard]] Result<Sha256Digest> CanonicalPhysicsStateTopologyFingerprint(const CanonicalWorld &world,
                                                                                const CanonicalPhysicsStateBindings &bindings,
                                                                                const CanonicalPhysicsStateTopologyMap &map);
    /** @brief Hashes complete immutable normalized filter values rather than
     * relying on the older 64-bit summary alone. */
    [[nodiscard]] Sha256Digest CanonicalPhysicsStateFilterFingerprint(const CanonicalWorld &world) noexcept;
    /** @brief Durable semantic surface policy; local owner mapping epoch is checked by its separate live fence. */
    [[nodiscard]] Sha256Digest CanonicalPhysicsStateSurfaceFingerprint(const PhysicsSurfaceWorldBinding &) noexcept;
    /** @brief Copies complete stable shape bindings only after actual owner
     * inventory admission. */
    [[nodiscard]] Result<std::vector<PhysicsStateShapeBinding>> CanonicalPhysicsStateShapeBindings(
        const CanonicalWorld &world, std::span<const PhysicsStateShapeBinding> bindings);
}  // namespace Horo::Physics::Detail
