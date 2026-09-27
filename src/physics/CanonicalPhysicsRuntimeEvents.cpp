#include "CanonicalPhysicsRuntimeInternal.h"

#include <Jolt/Physics/Body/BodyLock.h>
#include <algorithm>
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
        [[nodiscard]] PhysicsEventEndpoint ToEventEndpoint(const CanonicalWorld &world,
                                                           const CanonicalQueryFixtureRecord &fixture) noexcept {
            return {.body = fixture.fixture.body,
                    .shape = fixture.fixture.shape,
                    .subshape = fixture.descriptor.subshape,
                    .layer = fixture.descriptor.layer,
                    .profile = fixture.descriptor.profile,
                    .filterSchemaGeneration = world.query.querySchemaGeneration};
        }

        /** @brief Copies optional query material evidence without retaining descriptor storage. */
        [[nodiscard]] std::optional<PhysicsEventMaterial> ToEventMaterial(const std::optional<PhysicsQueryMaterial> &material) noexcept {
            if (!material.has_value())
                return std::nullopt;
            return PhysicsEventMaterial{.asset = material->asset, .assetGeneration = material->assetGeneration, .slot = material->slot};
        }

        /** @brief Chooses a deterministic copied point from one native contact manifold. */
        [[nodiscard]] Math::Vec3 ContactPoint(const JPH::ContactManifold &manifold) noexcept {
            Math::Vec3 selected = ToScene(manifold.mBaseOffset);
            bool selectedPoint = false;
            for (JPH::uint index = 0; index < manifold.mRelativeContactPointsOn1.size(); ++index) {
                const Math::Vec3 candidate = ToScene(manifold.GetWorldSpaceContactPointOn1(index));
                if (!selectedPoint || std::tie(candidate.x, candidate.y, candidate.z) < std::tie(selected.x, selected.y, selected.z)) {
                    selected = candidate;
                    selectedPoint = true;
                }
            }
            return selected;
        }
    }  // namespace

    /** @copydoc CanonicalContactListener::OnContactValidate */
    JPH::ValidateResult CanonicalContactListener::OnContactValidate(const JPH::Body &body1, const JPH::Body &body2, JPH::RVec3Arg,
                                                                    const JPH::CollideShapeResult &) {
        const std::uint64_t key = CollisionPairKey(body1.GetID(), body2.GetID());
        return std::ranges::binary_search(world_.scene.disabledJointCollisionPairs, key)
                   ? JPH::ValidateResult::RejectAllContactsForThisBodyPair
                   : JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }

    /** @copydoc CanonicalContactListener::OnContactAdded */
    void CanonicalContactListener::OnContactAdded(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                                                  JPH::ContactSettings &settings) {
        Emit(body1, body2, manifold, settings);
    }

    /** @copydoc CanonicalContactListener::OnContactPersisted */
    void CanonicalContactListener::OnContactPersisted(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                                                      JPH::ContactSettings &settings) {
        Emit(body1, body2, manifold, settings);
    }

    /** @brief Copies one complete callback manifold before native storage is released. */
    void CanonicalContactListener::Emit(const JPH::Body &body1, const JPH::Body &body2, const JPH::ContactManifold &manifold,
                                        const JPH::ContactSettings &settings) const noexcept {
        const ContactCaptureRoute *route = world_.contactRoute.load(std::memory_order::seq_cst);
        if (route == nullptr || route->Sink().append == nullptr || route->Sink().context == nullptr)
            return;
        const auto *fixture1 = FindFixture(world_, body1.GetID());
        const auto *fixture2 = FindFixture(world_, body2.GetID());
        if (fixture1 == nullptr || fixture2 == nullptr)
            return;
        const PhysicsContactObservation observation{.simulationTick = route->SimulationTick(),
                                                    .first = ToEventEndpoint(world_, *fixture1),
                                                    .second = ToEventEndpoint(world_, *fixture2),
                                                    .firstMaterial = ToEventMaterial(fixture1->descriptor.material),
                                                    .secondMaterial = ToEventMaterial(fixture2->descriptor.material),
                                                    .contact = {.position = ContactPoint(manifold),
                                                                .normal = {manifold.mWorldSpaceNormal.GetX(),
                                                                           manifold.mWorldSpaceNormal.GetY(),
                                                                           manifold.mWorldSpaceNormal.GetZ()},
                                                                .penetrationDepthMeters = manifold.mPenetrationDepth,
                                                                .normalImpulseNewtonSeconds = 0.0F},
                                                    .sensor = settings.mIsSensor};
        static_cast<void>(route->Sink().append(route->Sink().context, observation));
    }

    /** @copydoc InvokeCanonicalContactCallbackForTesting */
    bool InvokeCanonicalContactCallbackForTesting(const CanonicalWorldHandle world, const PhysicsQueryFixture &first,
                                                  const PhysicsQueryFixture &second, const std::uint64_t simulationTick, const bool sensor,
                                                  const bool persisted, const CanonicalContactSink contactSink) {
        if (world.value == nullptr || simulationTick == 0)
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
        manifold.mBaseOffset = JPH::RVec3::sZero();
        manifold.mWorldSpaceNormal = JPH::Vec3::sAxisY();
        manifold.mPenetrationDepth = 0.1F;
        manifold.mRelativeContactPointsOn1.emplace_back(JPH::Vec3::sZero());
        manifold.mRelativeContactPointsOn2.emplace_back(JPH::Vec3::sZero());
        JPH::ContactSettings settings{};
        settings.mIsSensor = sensor;
        if (persisted)
            canonical.contactListener.OnContactPersisted(firstLock.GetBody(), secondLock.GetBody(), manifold, settings);
        else
            canonical.contactListener.OnContactAdded(firstLock.GetBody(), secondLock.GetBody(), manifold, settings);
        return true;
    }
}  // namespace Horo::Physics::Detail
