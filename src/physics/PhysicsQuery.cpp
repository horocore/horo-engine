#include "Horo/Physics/PhysicsQuery.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <cmath>
#include <tuple>

namespace Horo::Physics {
    namespace {
        /** @brief Tests one finite unit direction without normalizing caller data. */
        [[nodiscard]] bool IsUnitDirection(const Math::Vec3 direction) noexcept {
            if (!Math::IsFinite(direction))
                return false;
            const double squaredNorm = static_cast<double>(direction.x) * direction.x + static_cast<double>(direction.y) * direction.y +
                                       static_cast<double>(direction.z) * direction.z;
            return std::abs(squaredNorm - 1.0) <= PhysicsQueryUnitVectorSquaredNormTolerance;
        }

        /** @brief Validates ray geometry in the active origin frame. */
        [[nodiscard]] Result<void> ValidateGeometry(const PhysicsRayQuery &query, const PhysicsWorldId) {
            if (!Math::IsFinite(query.origin) || !IsUnitDirection(query.direction) || !std::isfinite(query.maximumDistanceMeters) ||
                query.maximumDistanceMeters <= 0)
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid,
                                                       "Ray queries require a finite origin, unit direction and positive distance."));
            return Result<void>::Success();
        }

        /** @brief Validates resident swept shape identity and finite motion. */
        [[nodiscard]] Result<void> ValidateGeometry(const PhysicsSweepQuery &query, const PhysicsWorldId world) {
            if (const auto owner = ValidatePhysicsHandleOwner(query.shape, world); owner.HasError())
                return owner;
            if (const auto pose = ValidatePhysicsPose(query.pose); pose.HasError())
                return pose;
            if (!IsUnitDirection(query.direction) || !std::isfinite(query.maximumDistanceMeters) || query.maximumDistanceMeters <= 0)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Sweep queries require a unit direction and positive finite distance."));
            return Result<void>::Success();
        }

        /** @brief Validates resident overlap shape identity and pose. */
        [[nodiscard]] Result<void> ValidateGeometry(const PhysicsOverlapQuery &query, const PhysicsWorldId world) {
            if (const auto owner = ValidatePhysicsHandleOwner(query.shape, world); owner.HasError())
                return owner;
            return ValidatePhysicsPose(query.pose);
        }

        /** @brief Validates one point in the active origin frame. */
        [[nodiscard]] Result<void> ValidateGeometry(const PhysicsPointQuery &query, const PhysicsWorldId) {
            if (!Math::IsFinite(query.point))
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Point queries require finite coordinates."));
            return Result<void>::Success();
        }

        /** @brief Validates supported enum values without assuming their underlying representation. */
        [[nodiscard]] bool IsSupported(const PhysicsQueryTriggerPolicy value) noexcept {
            return value == PhysicsQueryTriggerPolicy::Exclude || value == PhysicsQueryTriggerPolicy::Include;
        }

        [[nodiscard]] bool IsSupported(const PhysicsQueryCollection value) noexcept {
            using enum PhysicsQueryCollection;
            return value == Closest || value == Any || value == All || value == ThroughFirstBlock;
        }

        [[nodiscard]] bool IsSupported(const PhysicsQueryOrdering value) noexcept {
            return value == PhysicsQueryOrdering::ClosestFirst;
        }

        [[nodiscard]] bool IsSupported(const PhysicsQueryResponse value) noexcept {
            return value == PhysicsQueryResponse::Overlap || value == PhysicsQueryResponse::Block;
        }

        /** @brief Produces an ordering sentinel for an absent asset-local subshape. */
        [[nodiscard]] std::uint64_t SubshapeValue(const PhysicsQueryHit &hit) noexcept {
            return hit.subshape.has_value() ? hit.subshape->Value() : 0;
        }

        /** @brief Orders blocking evidence ahead of overlap evidence at an exact-distance tie. */
        [[nodiscard]] std::uint8_t ResponseRank(const PhysicsQueryResponse response) noexcept {
            return response == PhysicsQueryResponse::Block ? 0U : 1U;
        }

        /** @brief Returns the finite travel bound for ray/sweep queries, when applicable. */
        [[nodiscard]] std::optional<float> MaximumTravelDistance(const PhysicsQueryGeometry &geometry) noexcept {
            if (const auto *ray = std::get_if<PhysicsRayQuery>(&geometry))
                return ray->maximumDistanceMeters;
            if (const auto *sweep = std::get_if<PhysicsSweepQuery>(&geometry))
                return sweep->maximumDistanceMeters;
            return std::nullopt;
        }

        /** @brief Maps an absent normal before present normals in the final deterministic tie-break. */
        [[nodiscard]] auto NormalKey(const PhysicsQueryHit &hit) noexcept {
            return std::tuple{hit.normal.has_value(), hit.normal.has_value() ? hit.normal->x : 0.0F,
                              hit.normal.has_value() ? hit.normal->y : 0.0F, hit.normal.has_value() ? hit.normal->z : 0.0F};
        }

        /** @brief Validates world/scene/channel admission identity. */
        [[nodiscard]] Result<void> ValidateQueryIdentity(const PhysicsQueryDescriptor &descriptor, const PhysicsWorldId expectedWorld,
                                                         const std::uint64_t expectedSceneGeneration) {
            if (!expectedWorld.IsValid() || !descriptor.world.IsValid())
                return Result<void>::Failure(MakeError(PhysicsErrors::WorldInvalid));
            if (descriptor.world != expectedWorld)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::HandleWorldMismatch, "Query targets another physics world generation."));
            if (expectedSceneGeneration == 0 || descriptor.sceneGeneration != expectedSceneGeneration)
                return Result<void>::Failure(MakeError(PhysicsErrors::QuerySnapshotStale));
            if (!descriptor.filter.channel.IsValid())
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Queries require a non-zero query channel ID."));
            return Result<void>::Success();
        }

        /** @brief Validates closed query policies and result bounds. */
        [[nodiscard]] Result<void> ValidateQueryPolicies(const PhysicsQueryDescriptor &descriptor) {
            if (!IsSupported(descriptor.filter.triggers) || !IsSupported(descriptor.collection) || !IsSupported(descriptor.ordering))
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::OperationUnsupported, "Query policy contains an unknown typed value."));
            if (descriptor.maximumHitCount == 0 || descriptor.maximumHitCount > MaximumPhysicsQueryHits)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::CapacityExceeded, "Query result bound is outside the supported profile."));
            if ((descriptor.collection == PhysicsQueryCollection::Closest || descriptor.collection == PhysicsQueryCollection::Any) &&
                descriptor.maximumHitCount != 1)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Closest and any queries require an exact one-hit result bound."));
            return Result<void>::Success();
        }

        /** @brief Validates allocation-free optional selectors. */
        [[nodiscard]] Result<void> ValidateQuerySelectors(const PhysicsQueryFilter &filter, const PhysicsWorldId world) {
            if (filter.requiredLayer.has_value() && !filter.requiredLayer->IsValid())
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Required collision layer ID is invalid."));
            if (filter.requiredProfile.has_value() && !filter.requiredProfile->IsValid())
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Required collision profile ID is invalid."));
            if (filter.excludedBody.has_value())
                return ValidatePhysicsHandleOwner(*filter.excludedBody, world);
            return Result<void>::Success();
        }

        /** @brief Validates hit identities selected by the admitted filter. */
        [[nodiscard]] Result<void> ValidateHitIdentity(const PhysicsQueryHit &hit, const PhysicsQueryDescriptor &descriptor) {
            if (const auto body = ValidatePhysicsHandleOwner(hit.body, descriptor.world); body.HasError())
                return body;
            if (const auto shape = ValidatePhysicsHandleOwner(hit.shape, descriptor.world); shape.HasError())
                return shape;
            if (!hit.layer.IsValid() || !hit.profile.IsValid() || hit.channel != descriptor.filter.channel)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Hit filter identity evidence is incomplete or mismatched."));
            if (descriptor.filter.requiredLayer.has_value() && hit.layer != *descriptor.filter.requiredLayer)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Hit does not match the required collision layer."));
            if (descriptor.filter.requiredProfile.has_value() && hit.profile != *descriptor.filter.requiredProfile)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Hit does not match the required collision profile."));
            return Result<void>::Success();
        }

        /** @brief Validates finite contact evidence and the admitted travel bound. */
        [[nodiscard]] Result<void> ValidateHitGeometry(const PhysicsQueryHit &hit, const PhysicsQueryDescriptor &descriptor) {
            if (!IsSupported(hit.response) || !Math::IsFinite(hit.position))
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Hit response and position must be supported and finite."));
            if (!std::isfinite(hit.distanceMeters) || hit.distanceMeters < 0)
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Hit distance must be non-negative and finite."));
            if (const auto maximumDistance = MaximumTravelDistance(descriptor.geometry);
                maximumDistance.has_value() && hit.distanceMeters > *maximumDistance)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Hit distance exceeds the admitted query travel bound."));
            if (hit.normal.has_value() && !IsUnitDirection(*hit.normal))
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "A reported hit normal must be finite and unit length."));
            return Result<void>::Success();
        }

        /** @brief Validates optional stable subshape/material evidence. */
        [[nodiscard]] Result<void> ValidateHitDetails(const PhysicsQueryHit &hit) {
            if (hit.subshape.has_value() && !hit.subshape->IsValid())
                return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "A reported subshape identity must be non-zero."));
            if (hit.material.has_value() &&
                (!hit.material->asset.IsValid() || hit.material->assetGeneration == 0 || !hit.material->slot.IsValid()))
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::DescriptorInvalid, "Material evidence requires exact asset, generation and slot identity."));
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc ValidatePhysicsQueryFixtureDescriptor */
    Result<void> ValidatePhysicsQueryFixtureDescriptor(const PhysicsQueryFixtureDescriptor &fixture, const PhysicsWorldId expectedWorld) {
        if (!expectedWorld.IsValid())
            return Result<void>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        if (const auto shape = ValidatePhysicsShapeDescriptor(fixture.shape); shape.HasError())
            return shape;
        if (const auto pose = ValidatePhysicsPose(fixture.pose); pose.HasError())
            return pose;
        if (!fixture.layer.IsValid() || !fixture.profile.IsValid() || !fixture.channel.IsValid())
            return Result<void>::Failure(
                MakeError(PhysicsErrors::DescriptorInvalid, "Query fixtures require stable layer, profile and channel identities."));
        if (static_cast<std::uint8_t>(fixture.response) > static_cast<std::uint8_t>(PhysicsQueryFixtureResponse::Block))
            return Result<void>::Failure(MakeError(PhysicsErrors::OperationUnsupported, "Unknown query fixture response."));
        if (fixture.subshape.has_value() && !fixture.subshape->IsValid())
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Fixture subshape identity must be non-zero."));
        if (fixture.material.has_value() &&
            (!fixture.material->asset.IsValid() || fixture.material->assetGeneration == 0 || !fixture.material->slot.IsValid()))
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Fixture material evidence is incomplete."));
        return Result<void>::Success();
    }

    /** @copydoc ValidatePhysicsQueryDescriptor */
    Result<void> ValidatePhysicsQueryDescriptor(const PhysicsQueryDescriptor &descriptor, const PhysicsWorldId expectedWorld,
                                                const std::uint64_t expectedSceneGeneration) {
        if (const auto identity = ValidateQueryIdentity(descriptor, expectedWorld, expectedSceneGeneration); identity.HasError())
            return identity;
        if (const auto policies = ValidateQueryPolicies(descriptor); policies.HasError())
            return policies;
        if (const auto selectors = ValidateQuerySelectors(descriptor.filter, expectedWorld); selectors.HasError())
            return selectors;
        return std::visit([expectedWorld](const auto &geometry) {
            return ValidateGeometry(geometry, expectedWorld);
        }, descriptor.geometry);
    }

    /** @copydoc ValidatePhysicsQueryHit */
    Result<void> ValidatePhysicsQueryHit(const PhysicsQueryHit &hit, const PhysicsQueryDescriptor &descriptor) {
        if (const auto identity = ValidateHitIdentity(hit, descriptor); identity.HasError())
            return identity;
        if (hit.filterSchemaGeneration == 0)
            return Result<void>::Failure(MakeError(PhysicsErrors::QuerySnapshotStale));
        if (const auto geometry = ValidateHitGeometry(hit, descriptor); geometry.HasError())
            return geometry;
        return ValidateHitDetails(hit);
    }

    /** @copydoc ValidatePhysicsQueryResult */
    Result<void> ValidatePhysicsQueryResult(const PhysicsQueryResult &result, const PhysicsQueryDescriptor &descriptor) {
        if (result.hitCount > descriptor.maximumHitCount)
            return Result<void>::Failure(
                MakeError(PhysicsErrors::CapacityExceeded, "Query provider returned more hits than the admitted result bound."));
        if (result.filterSchemaGeneration == 0)
            return Result<void>::Failure(MakeError(PhysicsErrors::QuerySnapshotStale));
        return Result<void>::Success();
    }

    /** @copydoc PhysicsQueryHitLess */
    bool PhysicsQueryHitLess(const PhysicsQueryHit &left, const PhysicsQueryHit &right) noexcept {
        const auto leftResponse = ResponseRank(left.response);
        const auto rightResponse = ResponseRank(right.response);
        const auto leftSubshape = SubshapeValue(left);
        const auto rightSubshape = SubshapeValue(right);
        const auto leftNormal = NormalKey(left);
        const auto rightNormal = NormalKey(right);
        const auto leftKey =
            std::tie(left.distanceMeters, leftResponse, left.body.slot.index, left.body.slot.generation, left.shape.slot.index,
                     left.shape.slot.generation, leftSubshape, left.position.x, left.position.y, left.position.z, leftNormal);
        const auto rightKey =
            std::tie(right.distanceMeters, rightResponse, right.body.slot.index, right.body.slot.generation, right.shape.slot.index,
                     right.shape.slot.generation, rightSubshape, right.position.x, right.position.y, right.position.z, rightNormal);
        if (const auto ordering = leftKey <=> rightKey; ordering != 0)
            return ordering < 0;
        if (left.material.has_value() != right.material.has_value())
            return !left.material.has_value();
        if (!left.material.has_value())
            return false;
        const auto leftSlot = left.material->slot.Value();
        const auto rightSlot = right.material->slot.Value();
        return std::tie(left.material->asset.Bytes(), left.material->assetGeneration, leftSlot) <=>
                   std::tie(right.material->asset.Bytes(), right.material->assetGeneration, rightSlot) <
               0;
    }
}  // namespace Horo::Physics
