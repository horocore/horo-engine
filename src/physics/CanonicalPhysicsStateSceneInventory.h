#pragma once
#include "CanonicalPhysicsRuntime.h"
#include "PhysicsStateRestartInput.h"

namespace Horo::Physics::Detail {
    struct CanonicalStateSceneCounts final {
        std::size_t bodies{};
        std::size_t shapes{};
        std::size_t constraints{};
        std::size_t queryFixtures{};
    };

    struct CanonicalStateSceneBody final {
        BodyHandle handle;
        ShapeHandle shape;
        std::uint64_t sceneEntity{};
    };

    struct CanonicalStateSceneStorage final {
        std::span<CanonicalStateSceneBody> bodies;
        std::span<ShapeHandle> shapes;
        std::span<ConstraintHandle> constraints;
    };

    /** @brief Exact joined owner inventory for canonical Scene key coverage, never persisted native identity.
     * Both calls are synchronous on the already validated Physics owner; caller provides bounded cold storage.
     * Query fixture count remains explicit so a Scene adapter cannot omit unowned resources.
     */
    [[nodiscard]] Result<StateDetail::RestartInputLimits> ReadCanonicalStateReceivingLimits(CanonicalWorldHandle,
                                                                                            const PhysicsWorldSettings &,
                                                                                            std::size_t maximumWireBytes,
                                                                                            std::size_t maximumRetainedBytes);
    [[nodiscard]] Result<CanonicalStateSceneCounts> InspectCanonicalStateSceneInventory(CanonicalWorldHandle);
    [[nodiscard]] Result<void> CopyCanonicalStateSceneInventory(CanonicalWorldHandle, CanonicalStateSceneStorage);
}  // namespace Horo::Physics::Detail
