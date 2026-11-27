#pragma once

/** @file CanonicalSolver.h
 * @brief Target-private native build check used before explicit solver activation.
 */

namespace Horo::Physics::Detail {
    /**
     * @brief Checks whether the composed solver binary matches the private adapter's ABI settings.
     * @return False when Physics native composition is omitted or the linked binary is incompatible.
     * @post No allocation, factory registration, callbacks, world creation or global state mutation.
     * Success is build compatibility only, not runtime readiness or platform/determinism qualification.
     */
    [[nodiscard]] bool IsCanonicalSolverBuildCompatible() noexcept;
}  // namespace Horo::Physics::Detail
