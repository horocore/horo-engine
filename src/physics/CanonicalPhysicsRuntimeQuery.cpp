#include "CanonicalPhysicsRuntimeQuery.h"

#include "Horo/Physics/PhysicsErrors.h"

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include <ranges>
#include <type_traits>
#include <utility>

namespace Horo::Physics::Detail {
    namespace {
        [[nodiscard]] JPH::Vec3 ToNative(const Math::Vec3 value) noexcept {
            return {value.x, value.y, value.z};
        }

        [[nodiscard]] JPH::Quat ToNative(const Math::Quaternion value) noexcept {
            return {value.x, value.y, value.z, value.w};
        }

        [[nodiscard]] Result<JPH::Ref<JPH::Shape>> CreateNativeShape(const PhysicsShapeDescriptor &descriptor) {
            JPH::ShapeSettings::ShapeResult created;
            std::visit([&created]<typename Shape>(const Shape &shape) {
                using ShapeType = std::decay_t<Shape>;
                if constexpr (std::is_same_v<ShapeType, PhysicsBoxShape>)
                    created = JPH::BoxShapeSettings(ToNative(shape.halfExtentsMeters)).Create();
                else if constexpr (std::is_same_v<ShapeType, PhysicsSphereShape>)
                    created = JPH::SphereShapeSettings(shape.radiusMeters).Create();
                else if constexpr (std::is_same_v<ShapeType, PhysicsCapsuleShape>)
                    created = JPH::CapsuleShapeSettings(shape.cylindricalHalfHeightMeters, shape.radiusMeters).Create();
                else {
                    static_assert(std::is_same_v<ShapeType, PhysicsStaticPlaneShape>);
                    created = JPH::PlaneShapeSettings(JPH::Plane(ToNative(shape.normal), shape.signedDistanceMeters)).Create();
                }
            }, descriptor);
            if (created.HasError())
                return Result<JPH::Ref<JPH::Shape>>::Failure(
                    MakeError(PhysicsErrors::ShapeArtifactInvalid, "Canonical solver rejected the admitted analytic query shape."));
            return Result<JPH::Ref<JPH::Shape>>::Success(created.Get());
        }

        [[nodiscard]] const CanonicalQueryFixtureRecord *FindCanonicalFixture(const CanonicalQueryAccess &access, const BodyHandle body) {
            const auto found = std::ranges::find_if(access.fixtures, [body](const auto &fixture) {
                return fixture.fixture.body == body;
            });
            return found == access.fixtures.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] const CanonicalQueryFixtureRecord *FindCanonicalFixture(const CanonicalQueryAccess &access, const ShapeHandle shape) {
            const auto found = std::ranges::find_if(access.fixtures, [shape](const auto &fixture) {
                return fixture.fixture.shape == shape;
            });
            return found == access.fixtures.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] const CanonicalQueryFixtureRecord *FindCanonicalFixture(const CanonicalQueryAccess &access, const JPH::BodyID body) {
            const std::size_t nativeIndex = body.GetIndex();
            if (nativeIndex >= access.nativeFixtureIndices.size())
                return nullptr;
            const std::size_t fixtureIndex = access.nativeFixtureIndices[nativeIndex];
            if (fixtureIndex == std::numeric_limits<std::size_t>::max() || fixtureIndex >= access.fixtures.size() ||
                access.fixtures[fixtureIndex].nativeBody != body)
                return nullptr;
            return &access.fixtures[fixtureIndex];
        }

        [[nodiscard]] bool Admits(const CanonicalQueryFixtureRecord &fixture, const PhysicsQueryDescriptor &descriptor) noexcept {
            if (fixture.descriptor.channel != descriptor.filter.channel ||
                fixture.descriptor.response == PhysicsQueryFixtureResponse::Ignore ||
                (fixture.descriptor.trigger && descriptor.filter.triggers == PhysicsQueryTriggerPolicy::Exclude))
                return false;
            if (descriptor.filter.requiredLayer.has_value() && fixture.descriptor.layer != *descriptor.filter.requiredLayer)
                return false;
            if (descriptor.filter.requiredProfile.has_value() && fixture.descriptor.profile != *descriptor.filter.requiredProfile)
                return false;
            if (descriptor.filter.excludedBody.has_value() && fixture.fixture.body == *descriptor.filter.excludedBody)
                return false;
            return true;
        }

        /** @brief Applies the stable Horo query filter before native collectors can short-circuit. */
        class QueryBodyFilter final : public JPH::BodyFilter {
        public:
            QueryBodyFilter(const CanonicalQueryAccess &access, const PhysicsQueryDescriptor &descriptor)
                : access_(access), descriptor_(descriptor) {}

            [[nodiscard]] bool ShouldCollide(const JPH::BodyID &body) const override {
                const auto *fixture = FindCanonicalFixture(access_, body);
                return fixture != nullptr && Admits(*fixture, descriptor_);
            }

        private:
            const CanonicalQueryAccess &access_;
            const PhysicsQueryDescriptor &descriptor_;
        };

        template <typename Collector> class FixedQueryCollector final : public Collector {
        public:
            using ResultType = typename Collector::ResultType;

            FixedQueryCollector(ResultType *storage, const PhysicsQueryCollection collection) : values(storage), collection_(collection) {}

            void AddHit(const ResultType &hit) override {
                if (collection_ == PhysicsQueryCollection::Any) {
                    if (count == 0)
                        values[count++] = hit;
                    this->ForceEarlyOut();
                    return;
                }
                if (count < MaximumPhysicsQueryHits)
                    values[count++] = hit;
                else
                    overflow = true;
            }

            ResultType *values{};
            std::size_t count{};
            bool overflow{};
            PhysicsQueryCollection collection_;
        };

        struct CanonicalQueryCollectors final {
            CanonicalQueryCollectors(CanonicalQueryStorage &storage, const PhysicsQueryCollection collection)
                : ray(storage.rayQueryResults.data(), collection), point(storage.pointQueryResults.data(), collection),
                  overlap(storage.overlapQueryResults.data(), collection), sweep(storage.sweepQueryResults.data(), collection) {}

            FixedQueryCollector<JPH::CastRayCollector> ray;
            FixedQueryCollector<JPH::CollidePointCollector> point;
            FixedQueryCollector<JPH::CollideShapeCollector> overlap;
            FixedQueryCollector<JPH::CastShapeCollector> sweep;
        };

        [[nodiscard]] const CanonicalQueryFixtureRecord *FindSourceShape(const CanonicalQueryAccess &access,
                                                                         const PhysicsQueryGeometry &geometry) {
            if (const auto *sweep = std::get_if<PhysicsSweepQuery>(&geometry))
                return FindCanonicalFixture(access, sweep->shape);
            if (const auto *overlap = std::get_if<PhysicsOverlapQuery>(&geometry))
                return FindCanonicalFixture(access, overlap->shape);
            return nullptr;
        }

        [[nodiscard]] PhysicsQueryResponse ToResponse(const PhysicsQueryFixtureResponse response) noexcept {
            return response == PhysicsQueryFixtureResponse::Overlap ? PhysicsQueryResponse::Overlap : PhysicsQueryResponse::Block;
        }

        [[nodiscard]] std::optional<Math::Vec3> RayNormal(const CanonicalQueryAccess &access, const JPH::BodyID body,
                                                          const JPH::SubShapeID &subshape, const Math::Vec3 position) {
            JPH::BodyLockRead lock(access.system.GetBodyLockInterface(), body);
            if (!lock.Succeeded())
                return std::nullopt;
            const JPH::Vec3 normal = lock.GetBody().GetWorldSpaceSurfaceNormal(subshape, {position.x, position.y, position.z});
            const Math::Vec3 result{normal.GetX(), normal.GetY(), normal.GetZ()};
            return Math::IsFinite(result) ? std::optional<Math::Vec3>{result} : std::nullopt;
        }

        void CollectRayQuery(const CanonicalQueryAccess &access, const PhysicsRayQuery &query, CanonicalQueryCollectors &collectors,
                             const QueryBodyFilter &bodyFilter) {
            access.system.GetNarrowPhaseQuery().CastRay({JPH::RVec3(query.origin.x, query.origin.y, query.origin.z),
                                                         ToNative(query.direction * query.maximumDistanceMeters)},
                                                        JPH::RayCastSettings{}, collectors.ray, {}, {}, bodyFilter);
        }

        void CollectPointQuery(const CanonicalQueryAccess &access, const PhysicsPointQuery &query, CanonicalQueryCollectors &collectors,
                               const QueryBodyFilter &bodyFilter) {
            access.system.GetNarrowPhaseQuery().CollidePoint({query.point.x, query.point.y, query.point.z}, collectors.point, {}, {},
                                                             bodyFilter);
        }

        [[nodiscard]] JPH::RMat44 ToNativeTransform(const PhysicsPose &pose) {
            return JPH::RMat44::sRotationTranslation(ToNative(pose.rotation),
                                                     JPH::RVec3(pose.translation.x, pose.translation.y, pose.translation.z));
        }

        void CollectOverlapQuery(const CanonicalQueryAccess &access, const PhysicsOverlapQuery &query,
                                 const CanonicalQueryFixtureRecord &source, CanonicalQueryCollectors &collectors,
                                 const QueryBodyFilter &bodyFilter) {
            const JPH::RMat44 transform = ToNativeTransform(query.pose);
            access.system.GetNarrowPhaseQuery().CollideShape(source.shape, JPH::Vec3::sOne(), transform, JPH::CollideShapeSettings{},
                                                             JPH::RVec3::sZero(), collectors.overlap, {}, {}, bodyFilter);
        }

        void CollectSweepQuery(const CanonicalQueryAccess &access, const PhysicsSweepQuery &query,
                               const CanonicalQueryFixtureRecord &source, CanonicalQueryCollectors &collectors,
                               const QueryBodyFilter &bodyFilter) {
            const JPH::RMat44 transform = ToNativeTransform(query.pose);
            const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(source.shape, JPH::Vec3::sOne(), transform,
                                                                              ToNative(query.direction * query.maximumDistanceMeters));
            access.system.GetNarrowPhaseQuery().CastShape(cast, JPH::ShapeCastSettings{}, JPH::RVec3::sZero(), collectors.sweep, {}, {},
                                                          bodyFilter);
        }

        [[nodiscard]] Result<void> CollectCanonicalQuery(const CanonicalQueryAccess &access, const PhysicsQueryDescriptor &descriptor,
                                                         CanonicalQueryCollectors &collectors, const QueryBodyFilter &bodyFilter) {
            const auto *source = FindSourceShape(access, descriptor.geometry);
            if ((std::holds_alternative<PhysicsSweepQuery>(descriptor.geometry) ||
                 std::holds_alternative<PhysicsOverlapQuery>(descriptor.geometry)) &&
                source == nullptr)
                return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));

            std::visit([&]<typename Query>(const Query &query) {
                if constexpr (std::is_same_v<Query, PhysicsRayQuery>)
                    CollectRayQuery(access, query, collectors, bodyFilter);
                else if constexpr (std::is_same_v<Query, PhysicsPointQuery>)
                    CollectPointQuery(access, query, collectors, bodyFilter);
                else if constexpr (std::is_same_v<Query, PhysicsOverlapQuery>)
                    CollectOverlapQuery(access, query, *source, collectors, bodyFilter);
                else {
                    static_assert(std::is_same_v<Query, PhysicsSweepQuery>);
                    CollectSweepQuery(access, query, *source, collectors, bodyFilter);
                }
            }, descriptor.geometry);
            if (collectors.ray.overflow || collectors.point.overflow || collectors.overlap.overflow || collectors.sweep.overflow)
                return Result<void>::Failure(
                    MakeError(PhysicsErrors::CapacityExceeded, "Canonical query exceeded the fixed native hit bound."));
            return Result<void>::Success();
        }

        [[nodiscard]] std::optional<Math::Vec3> ContactNormal(const JPH::Vec3 axis) {
            if (axis.LengthSq() <= 1.0e-12F)
                return std::nullopt;
            const JPH::Vec3 normalized = axis.Normalized();
            return Math::Vec3{-normalized.GetX(), -normalized.GetY(), -normalized.GetZ()};
        }

        struct CanonicalHitEvidence final {
            JPH::BodyID body;
            Math::Vec3 position;
            std::optional<Math::Vec3> normal;
            float distance{};
        };

        [[nodiscard]] Result<void> AppendCanonicalHit(const CanonicalQueryAccess &access, const PhysicsQueryDescriptor &descriptor,
                                                      std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> &candidates,
                                                      std::size_t &candidateCount, const CanonicalHitEvidence &evidence) {
            const auto *fixture = FindCanonicalFixture(access, evidence.body);
            if (fixture == nullptr || !Admits(*fixture, descriptor))
                return Result<void>::Success();
            if (candidateCount == candidates.size())
                return Result<void>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            PhysicsQueryHit hit{.body = fixture->fixture.body,
                                .shape = fixture->fixture.shape,
                                .subshape = fixture->descriptor.subshape,
                                .material = fixture->descriptor.material,
                                .layer = fixture->descriptor.layer,
                                .profile = fixture->descriptor.profile,
                                .channel = fixture->descriptor.channel,
                                .filterSchemaGeneration = access.querySchemaGeneration,
                                .response = ToResponse(fixture->descriptor.response),
                                .position = evidence.position,
                                .normal = evidence.normal,
                                .distanceMeters = evidence.distance};
            if (const Result<void> valid = ValidatePhysicsQueryHit(hit, descriptor); valid.HasError())
                return valid;
            candidates[candidateCount++] = std::move(hit);
            return Result<void>::Success();
        }

        template <typename Collector, typename EvidenceFunction>
        [[nodiscard]] Result<void> AppendCollectedQueryHits(const CanonicalQueryAccess &access, const PhysicsQueryDescriptor &descriptor,
                                                            const FixedQueryCollector<Collector> &collector,
                                                            std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> &candidates,
                                                            std::size_t &candidateCount, const EvidenceFunction &evidenceFunction) {
            for (std::size_t index = 0; index < collector.count; ++index) {
                if (const Result<void> appended =
                        AppendCanonicalHit(access, descriptor, candidates, candidateCount, evidenceFunction(collector.values[index]));
                    appended.HasError())
                    return appended;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AppendRayQueryHits(const CanonicalQueryAccess &access, const PhysicsQueryDescriptor &descriptor,
                                                      const PhysicsRayQuery &ray,
                                                      const FixedQueryCollector<JPH::CastRayCollector> &collector,
                                                      std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> &candidates,
                                                      std::size_t &candidateCount) {
            return AppendCollectedQueryHits(access, descriptor, collector, candidates, candidateCount, [&access, &ray](const auto &hit) {
                const Math::Vec3 position = ray.origin + ray.direction * (ray.maximumDistanceMeters * hit.mFraction);
                return CanonicalHitEvidence{.body = hit.mBodyID,
                                            .position = position,
                                            .normal = RayNormal(access, hit.mBodyID, hit.mSubShapeID2, position),
                                            .distance = ray.maximumDistanceMeters * hit.mFraction};
            });
        }

        [[nodiscard]] Result<void> AppendPointQueryHits(const CanonicalQueryAccess &access, const PhysicsQueryDescriptor &descriptor,
                                                        const PhysicsPointQuery &point,
                                                        const FixedQueryCollector<JPH::CollidePointCollector> &collector,
                                                        std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> &candidates,
                                                        std::size_t &candidateCount) {
            return AppendCollectedQueryHits(access, descriptor, collector, candidates, candidateCount, [&point](const auto &hit) {
                return CanonicalHitEvidence{.body = hit.mBodyID, .position = point.point, .normal = std::nullopt, .distance = 0.0F};
            });
        }

        template <typename Collector, typename DistanceFunction>
        [[nodiscard]] Result<void> AppendContactQueryHits(const CanonicalQueryAccess &access, const PhysicsQueryDescriptor &descriptor,
                                                          const FixedQueryCollector<Collector> &collector,
                                                          std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> &candidates,
                                                          std::size_t &candidateCount, const DistanceFunction &distanceFunction) {
            return AppendCollectedQueryHits(access, descriptor, collector, candidates, candidateCount,
                                            [&distanceFunction](const auto &hit) {
                const Math::Vec3 position{hit.mContactPointOn2.GetX(), hit.mContactPointOn2.GetY(), hit.mContactPointOn2.GetZ()};
                return CanonicalHitEvidence{.body = hit.mBodyID2,
                                            .position = position,
                                            .normal = ContactNormal(hit.mPenetrationAxis),
                                            .distance = distanceFunction(hit)};
            });
        }

        [[nodiscard]] Result<void> ProjectCanonicalQueryHits(const CanonicalQueryAccess &access, const PhysicsQueryDescriptor &descriptor,
                                                             const CanonicalQueryCollectors &collectors,
                                                             std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> &candidates,
                                                             std::size_t &candidateCount) {
            if (const auto *ray = std::get_if<PhysicsRayQuery>(&descriptor.geometry))
                return AppendRayQueryHits(access, descriptor, *ray, collectors.ray, candidates, candidateCount);
            if (const auto *point = std::get_if<PhysicsPointQuery>(&descriptor.geometry))
                return AppendPointQueryHits(access, descriptor, *point, collectors.point, candidates, candidateCount);
            if (std::holds_alternative<PhysicsOverlapQuery>(descriptor.geometry))
                return AppendContactQueryHits(access, descriptor, collectors.overlap, candidates, candidateCount, [](const auto &) {
                    return 0.0F;
                });
            const auto &sweep = std::get<PhysicsSweepQuery>(descriptor.geometry);
            return AppendContactQueryHits(access, descriptor, collectors.sweep, candidates, candidateCount, [&sweep](const auto &hit) {
                return hit.mFraction * sweep.maximumDistanceMeters;
            });
        }

        [[nodiscard]] Result<PhysicsQueryResult> FinalizeCanonicalQuery(const CanonicalQueryAccess &access,
                                                                        const PhysicsQueryDescriptor &descriptor,
                                                                        std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> &candidates,
                                                                        const std::size_t candidateCount,
                                                                        const std::span<PhysicsQueryHit> hits) {
            std::ranges::sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(candidateCount), PhysicsQueryHitLess);
            std::size_t selectedCount = candidateCount;
            if (descriptor.collection == PhysicsQueryCollection::Closest || descriptor.collection == PhysicsQueryCollection::Any)
                selectedCount = std::min<std::size_t>(selectedCount, 1);
            else if (descriptor.collection == PhysicsQueryCollection::ThroughFirstBlock) {
                const auto end = candidates.begin() + static_cast<std::ptrdiff_t>(candidateCount);
                const auto firstBlock = std::ranges::find_if(candidates.begin(), end, [](const PhysicsQueryHit &hit) {
                    return hit.response == PhysicsQueryResponse::Block;
                });
                selectedCount =
                    firstBlock == end ? candidateCount : static_cast<std::size_t>(std::distance(candidates.begin(), firstBlock) + 1);
            }
            if (selectedCount > descriptor.maximumHitCount)
                return Result<PhysicsQueryResult>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
            const std::size_t copied = std::min(selectedCount, hits.size());
            std::copy_n(candidates.begin(), copied, hits.begin());
            PhysicsQueryResult result{.hitCount = static_cast<std::uint32_t>(copied),
                                      .truncated = copied < selectedCount,
                                      .filterSchemaGeneration = access.querySchemaGeneration,
                                      .broadphaseSnapshotGeneration = access.querySchemaGeneration};
            if (const Result<void> valid = ValidatePhysicsQueryResult(result, descriptor); valid.HasError())
                return Result<PhysicsQueryResult>::Failure(valid.ErrorValue());
            return Result<PhysicsQueryResult>::Success(result);
        }
    }  // namespace

    /** @copydoc CreateCanonicalQueryFixtureFromAccess */
    Result<PhysicsQueryFixture> CreateCanonicalQueryFixtureFromAccess(CanonicalQueryAccess &access, const PhysicsWorldId owner,
                                                                      const PhysicsQueryFixtureDescriptor &fixture) {
        if (!owner.IsValid())
            return Result<PhysicsQueryFixture>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        if (access.nextFixtureSlot == std::numeric_limits<std::uint32_t>::max() ||
            access.nextFixtureGeneration == std::numeric_limits<std::uint32_t>::max() || access.fixtures.size() >= access.maximumFixtures)
            return Result<PhysicsQueryFixture>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        const auto nativeShape = CreateNativeShape(fixture.shape);
        if (nativeShape.HasError())
            return Result<PhysicsQueryFixture>::Failure(nativeShape.ErrorValue());

        const std::uint32_t slot = access.nextFixtureSlot++;
        const std::uint32_t generation = access.nextFixtureGeneration++;
        const PhysicsQueryFixture identity{.body = {owner, {slot, generation}}, .shape = {owner, {slot, generation}}};
        JPH::BodyCreationSettings settings(nativeShape.Value().GetPtr(),
                                           JPH::RVec3(fixture.pose.translation.x, fixture.pose.translation.y, fixture.pose.translation.z),
                                           ToNative(fixture.pose.rotation), JPH::EMotionType::Static, JPH::ObjectLayer{0});
        settings.mIsSensor = fixture.trigger;
        const JPH::BodyID nativeBody = access.system.GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        if (nativeBody.IsInvalid())
            return Result<PhysicsQueryFixture>::Failure(
                MakeError(PhysicsErrors::CapacityExceeded, "Canonical solver rejected the query fixture body admission."));
        if (nativeBody.GetIndex() >= access.nativeFixtureIndices.size()) {
            access.system.GetBodyInterface().RemoveBody(nativeBody);
            access.system.GetBodyInterface().DestroyBody(nativeBody);
            return Result<PhysicsQueryFixture>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }
        access.fixtures.push_back({.fixture = identity, .descriptor = fixture, .nativeBody = nativeBody, .shape = nativeShape.Value()});
        access.nativeFixtureIndices[nativeBody.GetIndex()] = access.fixtures.size() - 1;
        ++access.querySchemaGeneration;
        return Result<PhysicsQueryFixture>::Success(identity);
    }

    /** @copydoc DestroyCanonicalQueryFixtureFromAccess */
    Result<void> DestroyCanonicalQueryFixtureFromAccess(CanonicalQueryAccess &access, const PhysicsQueryFixture &fixture) {
        if (const auto owner = ValidatePhysicsHandleOwner(fixture.body, fixture.body.world); owner.HasError())
            return owner;
        if (fixture.body.world.Value() != fixture.shape.world.Value() || fixture.body.slot.index != fixture.shape.slot.index ||
            fixture.body.slot.generation != fixture.shape.slot.generation)
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Fixture body and shape identities must be paired."));
        const auto found = std::ranges::find_if(access.fixtures, [&fixture](const auto &entry) {
            return entry.fixture == fixture;
        });
        if (found == access.fixtures.end())
            return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
        access.system.GetBodyInterface().RemoveBody(found->nativeBody);
        access.system.GetBodyInterface().DestroyBody(found->nativeBody);
        const auto erasedIndex = static_cast<std::size_t>(std::distance(access.fixtures.begin(), found));
        access.nativeFixtureIndices[found->nativeBody.GetIndex()] = std::numeric_limits<std::size_t>::max();
        access.fixtures.erase(found);
        if (erasedIndex < access.fixtures.size())
            access.nativeFixtureIndices[access.fixtures[erasedIndex].nativeBody.GetIndex()] = erasedIndex;
        ++access.querySchemaGeneration;
        return Result<void>::Success();
    }

    /** @copydoc ExecuteCanonicalQueryFromAccess */
    Result<PhysicsQueryResult> ExecuteCanonicalQueryFromAccess(CanonicalQueryAccess &access, const PhysicsQueryDescriptor &descriptor,
                                                               const std::span<PhysicsQueryHit> hits) {
        CanonicalQueryCollectors collectors{access.storage, descriptor.collection};
        const QueryBodyFilter bodyFilter{access, descriptor};
        if (const Result<void> collected = CollectCanonicalQuery(access, descriptor, collectors, bodyFilter); collected.HasError())
            return Result<PhysicsQueryResult>::Failure(collected.ErrorValue());

        std::size_t candidateCount{};
        if (const Result<void> projected =
                ProjectCanonicalQueryHits(access, descriptor, collectors, access.storage.queryCandidates, candidateCount);
            projected.HasError())
            return Result<PhysicsQueryResult>::Failure(projected.ErrorValue());

        return FinalizeCanonicalQuery(access, descriptor, access.storage.queryCandidates, candidateCount, hits);
    }
}  // namespace Horo::Physics::Detail
