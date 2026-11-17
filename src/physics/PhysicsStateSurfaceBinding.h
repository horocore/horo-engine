#pragma once
#include "Horo/Physics/PhysicsErrors.h"
#include "Horo/Physics/PhysicsStateConstructionTypes.h"

#include <optional>

namespace Horo::Physics::StateDetail {
    /** @brief Copies actual immutable semantic fields; no live/resource permission is minted. */
    inline PhysicsStateSurfaceEvidence CaptureStateSurface(const PhysicsSurfaceEvidence &live) noexcept {
        return {live.surface, live.catalogRevision, live.catalogFingerprint, live.collider};
    }

    inline std::optional<PhysicsStateSurfaceEvidence> CaptureStateSurface(const std::optional<PhysicsSurfaceEvidence> &live) noexcept {
        return live ? std::optional{CaptureStateSurface(*live)} : std::nullopt;
    }

    /** @brief Issues actual current evidence, then compares every durable field and exact owned catalogue membership.
     * Body/shape/collider membership is checked separately through the canonical endpoint/constructor binding.
     */
    inline Result<std::optional<PhysicsSurfaceEvidence>> RebindStateSurface(const PhysicsSurfaceWorldBinding &binding,
                                                                            const std::optional<PhysicsStateSurfaceEvidence> &saved) {
        using Outcome = Result<std::optional<PhysicsSurfaceEvidence>>;
        if (!saved)
            return Outcome::Success({});
        if (!binding.catalog || !binding.mappingGeneration || !binding.catalog->Contains(saved->surface))
            return Outcome::Failure(MakeError(PhysicsErrors::ProfileUnsupported));
        auto current = MakePhysicsSurfaceEvidence(binding, saved->surface);
        current.collider = saved->collider;
        if (CaptureStateSurface(current) != *saved || !MatchesPhysicsSurfaceEvidence(binding, current))
            return Outcome::Failure(MakeError(PhysicsErrors::ProfileUnsupported));
        return Outcome::Success(current);
    }
}  // namespace Horo::Physics::StateDetail
