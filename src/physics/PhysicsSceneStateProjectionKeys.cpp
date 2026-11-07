#include "PhysicsSceneStateProjectionInternal.h"
#include "PhysicsStateHashInternal.h"

#include <algorithm>
#include <limits>

namespace Horo::Physics::Detail {
    /** @copydoc ResolveSceneStateKey */
    Result<SceneStateKey> ResolveSceneStateKey(const Runtime::PersistentEntityIdentityMap &map, Runtime::EntityRef entity,
                                               SceneStateKeyRole role, std::uint64_t slot) {
        using Outcome = Result<SceneStateKey>;
        if (!slot)
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        auto identity = map.Find(entity);
        if (identity.HasError())
            return Outcome::Failure(identity.ErrorValue());
        const auto records = map.Records();
        const auto record = std::ranges::lower_bound(records, identity.Value(), {}, &Runtime::PersistentEntityRecord::identity);
        if (record == records.end() || record->identity != identity.Value())
            return Outcome::Failure(MakeError(PhysicsErrors::DescriptorInvalid));
        auto resolved = map.Resolve(record->identity, record->generation);
        if (resolved.HasError())
            return Outcome::Failure(resolved.ErrorValue());
        if (resolved.Value() != entity)
            return Outcome::Failure(MakeError(PhysicsErrors::CapabilityStale));
        return Outcome::Success({record->identity, record->generation, role, slot});
    }

    /** @copydoc SceneStateOwnerCountsFit */
    bool SceneStateOwnerCountsFit(const CanonicalStateSceneCounts &counts, const PhysicsStructuralState &state,
                                  SceneStateProjectionLimits limits) noexcept {
        if (counts.queryFixtures || counts.bodies != state.bodies.size() || counts.constraints != state.constraints.size() ||
            state.shapes.size() > counts.shapes)
            return false;
        if (counts.bodies > limits.maximumBindings || counts.shapes > limits.maximumBindings - counts.bodies)
            return false;
        return counts.constraints <= limits.maximumBindings - counts.bodies - counts.shapes;
    }

    /** @copydoc PhysicsSceneStateProjection::ContentFingerprint */
    Sha256Digest PhysicsSceneStateProjection::ContentFingerprint(const Sha256Digest &authenticatedContent) const noexcept {
        StateDetail::StateHash hash{"Horo.Physics.State.SceneKeys.v1"};
        hash.Digest(authenticatedContent);
        hash.Word(rows_.size());
        for (const auto &row : rows_) {
            hash.Id(row.key.entity);
            hash.Word(row.key.generation.value);
            hash.Word(static_cast<std::uint8_t>(row.key.role));
            hash.Word(row.key.slot);
        }
        return hash.Finish();
    }
}  // namespace Horo::Physics::Detail
