#include "CanonicalPhysicsRuntimeInternal.h"

#include <utility>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Requires an explicit enabled schema/profile response; missing bindings remain closed. */
        std::optional<PhysicsQueryResponse> ResidentResponse(const CanonicalQueryAccess &access, const PhysicsQueryDescriptor &descriptor,
                                                             const CanonicalSceneBodyRecord &body) noexcept {
            if (!body.collision || (descriptor.filter.excludedBody && body.handle == *descriptor.filter.excludedBody))
                return std::nullopt;
            const auto *profile = access.simulation.Profile(body.collision->profile);
            const auto response = CanonicalProfileQueryResponse(access, body.collision->profile, descriptor.filter.channel, body.sensor);
            if (!profile || !response || (body.sensor && descriptor.filter.triggers == PhysicsQueryTriggerPolicy::Exclude) ||
                (descriptor.filter.requiredProfile && body.collision->profile != *descriptor.filter.requiredProfile) ||
                (descriptor.filter.requiredLayer && profile->layer != *descriptor.filter.requiredLayer))
                return std::nullopt;
            const auto effective = *response;
            if (descriptor.filter.blockingOnly && effective != PhysicsQueryResponse::Block)
                return std::nullopt;
            return effective;
        }
    }  // namespace

    /** @copydoc AdmitsCanonicalSceneQueryBody */
    bool AdmitsCanonicalSceneQueryBody(const CanonicalQueryAccess &access, const PhysicsQueryDescriptor &descriptor,
                                       const JPH::BodyID body) noexcept {
        const auto *record = ResolveCanonicalSceneBody(access, descriptor.world, body);
        return record && ResidentResponse(access, descriptor, *record).has_value();
    }

    /** @copydoc ProjectCanonicalSceneQueryHit */
    Result<std::optional<PhysicsQueryHit>> ProjectCanonicalSceneQueryHit(const CanonicalQueryAccess &access,
                                                                         const PhysicsQueryDescriptor &descriptor,
                                                                         const CanonicalQueryHitEvidence &evidence) {
        const auto *record = ResolveCanonicalSceneBody(access, descriptor.world, evidence.body);
        if (!record)
            return Result<std::optional<PhysicsQueryHit>>::Success(std::nullopt);
        const auto response = ResidentResponse(access, descriptor, *record);
        if (!response)
            return Result<std::optional<PhysicsQueryHit>>::Success(std::nullopt);
        const auto *profile = access.simulation.Profile(record->collision->profile);
        PhysicsQueryHit hit{.body = record->handle,
                            .shape = record->policy.shape,
                            .layer = profile->layer,
                            .profile = record->collision->profile,
                            .channel = descriptor.filter.channel,
                            .filterSchemaGeneration = CanonicalQueryFilterGeneration(access),
                            .response = *response,
                            .position = evidence.position,
                            .normal = evidence.normal,
                            .distanceMeters = evidence.distance,
                            .penetrationDepthMeters = evidence.penetrationDepth};
        // A native compound path is not an authored ID or an asset material slot. Ambiguity remains explicit absence.
        if (record->collision->colliders.size() == 1) {
            const auto &collider = record->collision->colliders.front();
            hit.subshape = collider.subshape;
            if (collider.materials.size() == 1)
                hit.material = collider.materials.front();
        }
        if (const auto valid = ValidatePhysicsQueryHit(hit, descriptor); valid.HasError())
            return Result<std::optional<PhysicsQueryHit>>::Failure(valid.ErrorValue());
        return Result<std::optional<PhysicsQueryHit>>::Success(std::move(hit));
    }
}  // namespace Horo::Physics::Detail
