#pragma once
#include "CanonicalPhysicsStateTopologyMap.h"
#include "PhysicsStateConstructionManifestOwner.h"

namespace Horo::Physics::Detail {
    /** @brief Caller-private row cursors. All capacity derives from actual retained topology before export. */
    struct CanonicalConstructionRows final {
        StateDetail::ConstructionManifestStorage storage;
        std::size_t bodies{};
        std::size_t colliders{};
        std::size_t materials{};
        std::size_t joints{};
        std::size_t fixtures{};
        std::size_t children{};
    };

    [[nodiscard]] Result<void> ExportCanonicalConstructionBodies(const CanonicalWorld &, const CanonicalPhysicsStateBindings &,
                                                                 const CanonicalPhysicsStateTopologyMap &, CanonicalConstructionRows &);
    [[nodiscard]] Result<void> ExportCanonicalConstructionJoints(const CanonicalWorld &, const CanonicalPhysicsStateBindings &,
                                                                 const CanonicalPhysicsStateTopologyMap &, CanonicalConstructionRows &);
    [[nodiscard]] Result<void> ExportCanonicalConstructionFixtures(const CanonicalWorld &, const CanonicalPhysicsStateBindings &,
                                                                   const CanonicalPhysicsStateTopologyMap &, CanonicalConstructionRows &);
    [[nodiscard]] Result<PhysicsStateShapeRecipes> ExportCanonicalConstructionRoots(const CanonicalWorld &,
                                                                                    const CanonicalPhysicsStateTopologyMap &,
                                                                                    StateDetail::ConstructionManifestOwner &);
    [[nodiscard]] Result<PhysicsStateConstructionManifestLimits> ReadCanonicalConstructionLimits(const CanonicalWorld &,
                                                                                                 std::size_t maximumRetainedBytes,
                                                                                                 std::size_t ownerCount);
    [[nodiscard]] Result<std::unique_ptr<StateDetail::ConstructionManifestOwner>> PrepareCanonicalConstructionManifest(
        const CanonicalWorld &, const CanonicalPhysicsStateBindings &, const CanonicalPhysicsStateTopologyMap &,
        std::size_t maximumRetainedBytes, std::span<const PhysicsStatePersistentOwner> owners);
}  // namespace Horo::Physics::Detail
