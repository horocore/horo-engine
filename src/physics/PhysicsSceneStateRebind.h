#pragma once
#include "PhysicsSceneStateProjection.h"

namespace Horo::Physics::Detail {
    /** @brief Cold private table replacement. Local Scene EntityRefs/slots remain receiving-owner values;
     * only actual handles are rebound through exact canonical full-key IDs. No wire handle is admitted.
     */
    struct PhysicsSceneStateRebind final {
        std::vector<StructuralBodyBinding> bodies;
        std::vector<StructuralShapeBinding> shapes;
        std::vector<StructuralConstraintBinding> constraints;
        std::vector<PhysicsSceneBodyBinding> authoredBodies, legacyBodies;
        std::vector<PhysicsSceneShapeBinding> authoredShapes, legacyShapes;
        std::vector<PhysicsSceneConstraintBinding> authoredConstraints, legacyConstraints;
        [[nodiscard]] Result<void> Rebind(const PhysicsSceneStateProjection &, const CanonicalRestartBindings &);
        [[nodiscard]] std::size_t RetainedPayloadBytes() const noexcept;
    };
}  // namespace Horo::Physics::Detail
