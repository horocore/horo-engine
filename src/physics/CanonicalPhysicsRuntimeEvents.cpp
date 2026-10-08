#include "CanonicalPhysicsRuntimeInternal.h"

#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/EstimateCollisionResponse.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <algorithm>
#include <cmath>
#include <ranges>
#include <tuple>

namespace Horo::Physics::Detail {
    namespace {
        /** @brief Orders native body identities without dropping their reuse sequence. */
        [[nodiscard]] std::uint64_t CollisionPairKey(const JPH::BodyID first, const JPH::BodyID second) noexcept {
            const auto low = std::min(first.GetIndexAndSequenceNumber(), second.GetIndexAndSequenceNumber());
            const auto high = std::max(first.GetIndexAndSequenceNumber(), second.GetIndexAndSequenceNumber());
            return (static_cast<std::uint64_t>(low) << 32U) | high;
        }

        [[nodiscard]] const CanonicalQueryFixtureRecord *FindFixture(const CanonicalWorld &world, const BodyHandle body) {
            const auto found = std::ranges::find_if(world.query.fixtures, [body](const auto &fixture) {
                return fixture.fixture.body == body;
            });
            return found == world.query.fixtures.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] const CanonicalQueryFixtureRecord *FindFixture(const CanonicalWorld &world, const JPH::BodyID body) {
            const std::size_t nativeIndex = body.GetIndex();
            if (nativeIndex >= world.query.nativeFixtureIndices.size())
                return nullptr;
            const std::size_t fixtureIndex = world.query.nativeFixtureIndices[nativeIndex];
            if (fixtureIndex == CanonicalWorldQueryState::InvalidFixtureIndex || fixtureIndex >= world.query.fixtures.size() ||
                world.query.fixtures[fixtureIndex].nativeBody != body)
                return nullptr;
            return &world.query.fixtures[fixtureIndex];
        }

        /** @brief Converts one native contact position into backend-neutral scene coordinates. */
        [[nodiscard]] Math::Vec3 ToScene(const JPH::RVec3 &value) noexcept {
            return {value.GetX(), value.GetY(), value.GetZ()};
        }

        /** @brief Copies one query fixture identity and filter generation into event evidence. */
        [[nodiscard]] PhysicsEventEndpoint ToEventEndpoint(const CanonicalWorld &world, const CanonicalQueryFixtureRecord &fixture,
                                                           const PhysicsCompoundChild *child) noexcept {
            return {.body = fixture.fixture.body,
                    .shape = fixture.fixture.shape,
                    .subshape = child == nullptr ? fixture.descriptor.subshape : std::optional<PhysicsShapeSubresourceId>{child->subshape},
                    .layer = child == nullptr ? fixture.descriptor.layer : child->layer,
                    .profile = child == nullptr ? fixture.descriptor.profile : child->profile,
                    .filterSchemaGeneration = world.query.querySchemaGeneration};
        }

        /** @brief Copies optional query material evidence without retaining descriptor storage. */
        [[nodiscard]] std::optional<PhysicsEventMaterial> ToEventMaterial(const std::optional<PhysicsQueryMaterial> &material) noexcept {
            if (!material.has_value())
                return std::nullopt;
            return PhysicsEventMaterial{.asset = material->asset, .assetGeneration = material->assetGeneration, .slot = material->slot};
        }

        struct ContactEndpointEvidence final {
            PhysicsEventEndpoint endpoint;
            std::optional<PhysicsEventMaterial> material;
        };

        /** @brief Copies exact fixture or indexed resident identity; unknown IDs remain closed. */
        [[nodiscard]] std::optional<ContactEndpointEvidence> ResolveContactEndpoint(const CanonicalWorld &world, const JPH::Body &native,
                                                                                    const JPH::SubShapeID subshape) noexcept {
            if (const auto *fixture = FindFixture(world, native.GetID())) {
                const auto *child = ResolveCanonicalFixtureChild(*fixture, subshape);
                if (std::holds_alternative<PhysicsCompoundShapeDescriptor>(fixture->descriptor.shape) && child == nullptr)
                    return std::nullopt;
                return ContactEndpointEvidence{ToEventEndpoint(world, *fixture, child),
                                               ToEventMaterial(child == nullptr ? fixture->descriptor.material : child->material)};
            }
            const auto nativeIndex = native.GetID().GetIndex();
            if (nativeIndex >= world.scene.nativeBodyIndices.size())
                return std::nullopt;
            const auto index = world.scene.nativeBodyIndices[nativeIndex];
            if (index >= world.scene.bodies.size())
                return std::nullopt;
            const auto &record = world.scene.bodies[index];
            const auto row = native.GetObjectLayer();
            if (record.nativeBody != native.GetID() || !record.handle.IsValid() || !record.collision || row == 0 ||
                row > world.simulation.layerCount)
                return std::nullopt;
            if (const auto *profile = world.simulation.Profile(record.collision->profile); !profile || !profile->lifecycleEventsEnabled)
                return std::nullopt;
            PhysicsEventEndpoint endpoint{.body = record.handle,
                                          .shape = record.policy.shape,
                                          .layer = world.simulation.Layer(row),
                                          .profile = record.collision->profile,
                                          .filterSchemaGeneration = world.simulation.SchemaGeneration()};
            // A compound's native path is not an authored ID. Only exact single-contributor evidence is unambiguous here.
            if (record.collision->colliders.size() == 1)
                endpoint.subshape = record.collision->colliders.front().subshape;
            return ContactEndpointEvidence{endpoint, std::nullopt};
        }

        /** @brief Orders point identity independently of an optional estimated impulse. */
        [[nodiscard]] bool PointGeometryLess(const PhysicsContactPoint &left, const PhysicsContactPoint &right) noexcept {
            return std::tie(left.positionOnFirst, left.positionOnSecond, left.normal, left.penetrationDepthMeters) <
                   std::tie(right.positionOnFirst, right.positionOnSecond, right.normal, right.penetrationDepthMeters);
        }

        /** @brief Retains the smallest distinct point evidence in the fixed public manifold. */
        void RetainPoint(PhysicsContactSummary &summary, const PhysicsContactPoint &point) noexcept {
            auto end = summary.points.begin() + summary.pointCount;
            auto position = std::lower_bound(summary.points.begin(), end, point, PointGeometryLess);
            if (position != end && !PointGeometryLess(point, *position)) {
                position->normalImpulseEstimateNewtonSeconds =
                    std::max(position->normalImpulseEstimateNewtonSeconds, point.normalImpulseEstimateNewtonSeconds);
                return;
            }
            if (summary.pointCount == MaximumPhysicsContactPoints) {
                ++summary.omittedPointCount;
                if (position == end)
                    return;
                --end;
            } else {
                ++summary.pointCount;
            }
            std::move_backward(position, end, end + 1);
            *position = point;
        }

        /** @brief Copies bounded native manifold geometry and clearly labels pre-solve estimates. */
        [[nodiscard]] PhysicsContactSummary CopyContactSummary(const JPH::Body &first, const JPH::Body &second,
                                                               const JPH::ContactManifold &manifold,
                                                               const JPH::ContactSettings &settings) noexcept {
            PhysicsContactSummary summary;
            const JPH::uint count = std::min(manifold.mRelativeContactPointsOn1.size(), manifold.mRelativeContactPointsOn2.size());
            if (count == 0)
                return summary;
            const Math::Vec3 normal{manifold.mWorldSpaceNormal.GetX(), manifold.mWorldSpaceNormal.GetY(),
                                    manifold.mWorldSpaceNormal.GetZ()};
            if (!Math::IsFinite(normal) || std::abs(normal.x * normal.x + normal.y * normal.y + normal.z * normal.z - 1.0F) > 1.0e-3F) {
                summary.points[0].normal = normal;
                summary.pointCount = 1;
                return summary;
            }
            JPH::CollisionEstimationResult estimate;
            const bool canEstimate =
                !settings.mIsSensor &&
                (first.GetMotionType() == JPH::EMotionType::Dynamic || second.GetMotionType() == JPH::EMotionType::Dynamic) &&
                manifold.mRelativeContactPointsOn1.size() == manifold.mRelativeContactPointsOn2.size();
            if (canEstimate)
                JPH::EstimateCollisionResponse(first, second, manifold, estimate, settings.mCombinedFriction, settings.mCombinedRestitution,
                                               1.0F, 4);
            for (JPH::uint index = 0; index < count; ++index) {
                PhysicsContactPoint point{.positionOnFirst = ToScene(manifold.GetWorldSpaceContactPointOn1(index)),
                                          .positionOnSecond = ToScene(manifold.GetWorldSpaceContactPointOn2(index)),
                                          .normal = normal,
                                          .penetrationDepthMeters = manifold.mPenetrationDepth};
                if (canEstimate && index < estimate.mContactImpulse.size())
                    point.normalImpulseEstimateNewtonSeconds = estimate.mContactImpulse[index];
                if (!Math::IsFinite(point.positionOnFirst) || !Math::IsFinite(point.positionOnSecond) || !Math::IsFinite(point.normal) ||
                    !std::isfinite(point.penetrationDepthMeters) ||
                    (point.normalImpulseEstimateNewtonSeconds.has_value() &&
                     (!std::isfinite(*point.normalImpulseEstimateNewtonSeconds) || *point.normalImpulseEstimateNewtonSeconds < 0.0F))) {
                    summary.points[0] = point;
                    summary.pointCount = 1;
                    return summary;
                }
                RetainPoint(summary, point);
            }
            return summary;
        }
    }  // namespace

    /** @copydoc CanonicalContactListener::OnContactValidate */
    JPH::ValidateResult CanonicalContactListener::OnContactValidate(const JPH::Body &body1, const JPH::Body &body2, JPH::RVec3Arg,
                                                                    const JPH::CollideShapeResult &) {
        const std::uint64_t key = CollisionPairKey(body1.GetID(), body2.GetID());
        return world_.simulation.Response(body1.GetObjectLayer(), body2.GetObjectLayer()) == SimulationPairResponse::Ignore ||
                       std::ranges::binary_search(world_.scene.disabledJointCollisionPairs, key)
                   ? JPH::ValidateResult::RejectAllContactsForThisBodyPair
                   : JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    /** @copydoc CanonicalContactListener::OnContactAdded */
    void CanonicalContactListener::OnContactAdded(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                                                  JPH::ContactSettings &settings) {
        settings.mIsSensor = settings.mIsSensor ||
                             world_.simulation.Response(body1.GetObjectLayer(), body2.GetObjectLayer()) == SimulationPairResponse::Overlap;
        Emit(body1, body2, manifold, settings);
    }

    /** @copydoc CanonicalContactListener::OnContactPersisted */
    void CanonicalContactListener::OnContactPersisted(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                                                      JPH::ContactSettings &settings) {
        OnContactAdded(body1, body2, manifold, settings);
    }

    /** @brief Copies one complete callback manifold before native storage is released. */
    void CanonicalContactListener::Emit(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                                        const JPH::ContactSettings &settings) const noexcept {
        const ContactCaptureRoute *route = world_.contactRoute.load(std::memory_order::seq_cst);
        if (route == nullptr || route->Sink().append == nullptr || route->Sink().context == nullptr)
            return;
        const auto first = ResolveContactEndpoint(world_, body1, manifold.mSubShapeID1);
        const auto second = ResolveContactEndpoint(world_, body2, manifold.mSubShapeID2);
        if (!first || !second)
            return;
        const PhysicsContactSummary contact = CopyContactSummary(body1, body2, manifold, settings);
        if (contact.pointCount == 0)
            return;
        const PhysicsContactObservation observation{.simulationTick = route->SimulationTick(),
                                                    .first = first->endpoint,
                                                    .second = second->endpoint,
                                                    .firstMaterial = first->material,
                                                    .secondMaterial = second->material,
                                                    .contact = contact,
                                                    .sensor = settings.mIsSensor};
        static_cast<void>(route->Sink().append(route->Sink().context, observation));
    }

    /** @copydoc InvokeCanonicalContactCallbackForTesting */
    bool InvokeCanonicalContactCallbackForTesting(const CanonicalWorldHandle world, const PhysicsQueryFixture &first,
                                                  const PhysicsQueryFixture &second, const std::uint64_t simulationTick,
                                                  const CanonicalContactSink contactSink, const CanonicalContactTestOptions options) {
        if (world.value == nullptr || simulationTick == 0 || options.contactPointCount == 0 ||
            options.contactPointCount > JPH::ContactPoints::Capacity)
            return false;
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        const auto *firstFixture = FindFixture(canonical, first.body);
        const auto *secondFixture = FindFixture(canonical, second.body);
        if (firstFixture == nullptr || secondFixture == nullptr)
            return false;

        const ContactCaptureRoute route{canonical, simulationTick, contactSink};
        JPH::BodyLockRead firstLock(canonical.native.system->GetBodyLockInterface(), firstFixture->nativeBody);
        JPH::BodyLockRead secondLock(canonical.native.system->GetBodyLockInterface(), secondFixture->nativeBody);
        if (!firstLock.Succeeded() || !secondLock.Succeeded())
            return false;

        JPH::ContactManifold manifold;
        if (firstLock.GetBody().GetShape()->GetType() == JPH::EShapeType::Compound) {
            const auto &shape = static_cast<const JPH::CompoundShape &>(*firstLock.GetBody().GetShape());
            manifold.mSubShapeID1 = shape.GetSubShapeIDFromIndex(0, JPH::SubShapeIDCreator{}).GetID();
        }
        if (secondLock.GetBody().GetShape()->GetType() == JPH::EShapeType::Compound) {
            const auto &shape = static_cast<const JPH::CompoundShape &>(*secondLock.GetBody().GetShape());
            manifold.mSubShapeID2 = shape.GetSubShapeIDFromIndex(0, JPH::SubShapeIDCreator{}).GetID();
        }
        manifold.mBaseOffset = JPH::RVec3::sZero();
        manifold.mWorldSpaceNormal = JPH::Vec3::sAxisY();
        manifold.mPenetrationDepth = 0.1F;
        for (std::uint32_t index = 0; index < options.contactPointCount; ++index) {
            const JPH::Vec3 position(static_cast<float>(options.contactPointCount - index - 1), 0.0F, 0.0F);
            manifold.mRelativeContactPointsOn1.emplace_back(position);
            manifold.mRelativeContactPointsOn2.emplace_back(position);
        }
        JPH::ContactSettings settings{};
        settings.mIsSensor = options.sensor;
        if (options.persisted)
            canonical.contactListener.OnContactPersisted(firstLock.GetBody(), secondLock.GetBody(), manifold, settings);
        else
            canonical.contactListener.OnContactAdded(firstLock.GetBody(), secondLock.GetBody(), manifold, settings);
        return true;
    }
}  // namespace Horo::Physics::Detail
