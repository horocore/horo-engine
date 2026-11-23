#pragma once

/** @file DeterministicCrowdProvider.h
 * @brief Fixed-order planar crowd solver for an explicitly qualified runtime tuple.
 */

#include "Horo/Navigation/NavigationCrowdBatch.h"

namespace Horo::Navigation {
    /** @brief Identifies the versioned built-in solver contract. @return Stable provider fingerprint. */
    [[nodiscard]] Sha256Digest DeterministicCrowdProviderFingerprint() noexcept;

    /** @brief Creates the built-in solver for one exact host-verified qualification.
     * @param qualification Verified same-build, same-platform numeric, profile and golden evidence.
     * @return Owned solver, or typed evidence/capacity failure.
     * @details Candidates are examined in fixed order and checked against all retained swept constraints.
     *          A qualification record supplied by the host must already be verified for the current build.
     */
    [[nodiscard]] Result<std::unique_ptr<INavigationQualifiedCrowdBackend>> CreateDeterministicCrowdBackend(
        const NavigationCrowdQualification &qualification);
}  // namespace Horo::Navigation
