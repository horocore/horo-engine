#pragma once

#include "CanonicalPhysicsRuntimeInternal.h"

namespace Horo::Physics::Detail {
    /** @brief Exact source construction profile is checked before allocating an
     * unpublished native topology. */
    [[nodiscard]] Result<void> ValidateCanonicalConstraintCreationProfile(const CanonicalWorld &world);
    /** @brief Replays only recorded constructor frames through the SAME typed
     * builder; caller restores complete body solver state afterwards. */
    [[nodiscard]] Result<JPH::Ref<JPH::Constraint>> ReplayCanonicalConstraintCreation(const CanonicalSceneConstraintRecord &source,
                                                                                      JPH::Body &first, JPH::Body &second);
    /** @brief Same checked typed replay kernel for a fully validated cold restart manifest, never a fabricated native record. */
    [[nodiscard]] Result<JPH::Ref<JPH::Constraint>> ReplayCanonicalConstraintCreation(const CanonicalConstraintCreation &,
                                                                                      const PhysicsConstraintDescriptor &,
                                                                                      bool hasSecondBody, JPH::Body &, JPH::Body &);
}  // namespace Horo::Physics::Detail
