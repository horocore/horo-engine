#pragma once

#include "CanonicalPhysicsStateTopology.h"
#include "PhysicsStateSolverStorage.h"

namespace Horo::Physics::Detail {
    /** @brief Captures current last-applied policy intent from actual Horo body
     * records, never from cold cached bindings. */
    [[nodiscard]] Result<void> CaptureCanonicalStateIntent(const CanonicalWorld &world, const CanonicalPhysicsStateBindings &bindings,
                                                           StateDetail::SolverStorage &storage, const PhysicsStateSolverView &state);
    /** @brief Checks retained configuration with restored last-applied intent
     * through the existing body contract. */
    [[nodiscard]] Result<void> ValidateCanonicalStateIntent(const CanonicalWorld &world, const CanonicalPhysicsStateBindings &bindings,
                                                            const PhysicsStateSolverView &state);
    /** @brief Applies only to a private candidate after complete validation; live
     * Horo metadata remains untouched. */
    [[nodiscard]] Result<void> ApplyCanonicalStateIntent(CanonicalWorld &candidate, const CanonicalPhysicsStateBindings &bindings,
                                                         const PhysicsStateSolverView &state);
}  // namespace Horo::Physics::Detail
