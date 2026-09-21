#include "CanonicalPhysicsRuntimeInternal.h"

#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/ShapeCast.h>

namespace Horo::Physics::Detail {
    namespace {
        [[nodiscard]] const CanonicalQueryFixtureRecord *FindFixture(const CanonicalWorld &world, const BodyHandle body) {
            const auto found = std::ranges::find_if(world.query.fixtures, [body](const auto &fixture) {
                return fixture.fixture.body == body;
            });
            return found == world.query.fixtures.end() ? nullptr : std::to_address(found);
        }

        [[nodiscard]] const CanonicalQueryFixtureRecord *FindFixture(const CanonicalWorld &world, const ShapeHandle shape) {
            const auto found = std::ranges::find_if(world.query.fixtures, [shape](const auto &fixture) {
                return fixture.fixture.shape == shape;
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
            QueryBodyFilter(const CanonicalWorld &world, const PhysicsQueryDescriptor &descriptor)
                : world_(world), descriptor_(descriptor) {}

            [[nodiscard]] bool ShouldCollide(const JPH::BodyID &body) const override {
                const auto *fixture = FindFixture(world_, body);
                return fixture != nullptr && Admits(*fixture, descriptor_);
            }

        private:
            const CanonicalWorld &world_;
            const PhysicsQueryDescriptor &descriptor_;
        };

        [[nodiscard]] const CanonicalQueryFixtureRecord *FindSourceShape(const CanonicalWorld &world,
                                                                         const PhysicsQueryGeometry &geometry) {
            if (const auto *sweep = std::get_if<PhysicsSweepQuery>(&geometry))
                return FindFixture(world, sweep->shape);
            if (const auto *overlap = std::get_if<PhysicsOverlapQuery>(&geometry))
                return FindFixture(world, overlap->shape);
            return nullptr;
        }

        [[nodiscard]] PhysicsQueryResponse ToResponse(const PhysicsQueryFixtureResponse response) noexcept {
            return response == PhysicsQueryFixtureResponse::Overlap ? PhysicsQueryResponse::Overlap : PhysicsQueryResponse::Block;
        }

        [[nodiscard]] std::optional<Math::Vec3> RayNormal(const CanonicalWorld &world, const JPH::BodyID body,
                                                          const JPH::SubShapeID &subshape, const Math::Vec3 position) {
            JPH::BodyLockRead lock(world.native.system->GetBodyLockInterface(), body);
            if (!lock.Succeeded())
                return std::nullopt;
            const JPH::Vec3 normal = lock.GetBody().GetWorldSpaceSurfaceNormal(subshape, {position.x, position.y, position.z});
            const Math::Vec3 result{normal.GetX(), normal.GetY(), normal.GetZ()};
            return Math::IsFinite(result) ? std::optional<Math::Vec3>{result} : std::nullopt;
        }

        void CollectRayQuery(const CanonicalWorld &world, const PhysicsRayQuery &query, CanonicalQueryCollectors &collectors,
                             const QueryBodyFilter &bodyFilter) {
            world.native.system->GetNarrowPhaseQuery().CastRay({JPH::RVec3(query.origin.x, query.origin.y, query.origin.z),
                                                                ToNative(query.direction * query.maximumDistanceMeters)},
                                                               JPH::RayCastSettings{}, collectors.ray, {}, {}, bodyFilter);
        }

        void CollectPointQuery(const CanonicalWorld &world, const PhysicsPointQuery &query, CanonicalQueryCollectors &collectors,
                               const QueryBodyFilter &bodyFilter) {
            world.native.system->GetNarrowPhaseQuery().CollidePoint({query.point.x, query.point.y, query.point.z}, collectors.point, {}, {},
                                                                    bodyFilter);
        }

        [[nodiscard]] JPH::RMat44 ToNativeTransform(const PhysicsPose &pose) {
            return JPH::RMat44::sRotationTranslation(ToNative(pose.rotation),
                                                     JPH::RVec3(pose.translation.x, pose.translation.y, pose.translation.z));
        }

        void CollectOverlapQuery(const CanonicalWorld &world, const PhysicsOverlapQuery &query, const CanonicalQueryFixtureRecord &source,
                                 CanonicalQueryCollectors &collectors, const QueryBodyFilter &bodyFilter) {
            const JPH::RMat44 transform = ToNativeTransform(query.pose);
            world.native.system->GetNarrowPhaseQuery().CollideShape(source.shape, JPH::Vec3::sOne(), transform, JPH::CollideShapeSettings{},
                                                                    JPH::RVec3::sZero(), collectors.overlap, {}, {}, bodyFilter);
        }

        void CollectSweepQuery(const CanonicalWorld &world, const PhysicsSweepQuery &query, const CanonicalQueryFixtureRecord &source,
                               CanonicalQueryCollectors &collectors, const QueryBodyFilter &bodyFilter) {
            const JPH::RMat44 transform = ToNativeTransform(query.pose);
            const JPH::RShapeCast cast = JPH::RShapeCast::sFromWorldTransform(source.shape, JPH::Vec3::sOne(), transform,
                                                                              ToNative(query.direction * query.maximumDistanceMeters));
            world.native.system->GetNarrowPhaseQuery().CastShape(cast, JPH::ShapeCastSettings{}, JPH::RVec3::sZero(), collectors.sweep, {},
                                                                 {}, bodyFilter);
        }

        /** @brief Executes one validated native query into the world-owned fixed collectors. */
        [[nodiscard]] Result<void> CollectCanonicalQuery(const CanonicalWorld &world, const PhysicsQueryDescriptor &descriptor,
                                                         CanonicalQueryCollectors &collectors, const QueryBodyFilter &bodyFilter) {
            const auto *source = FindSourceShape(world, descriptor.geometry);
            if ((std::holds_alternative<PhysicsSweepQuery>(descriptor.geometry) ||
                 std::holds_alternative<PhysicsOverlapQuery>(descriptor.geometry)) &&
                source == nullptr)
                return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));

            std::visit([&]<typename Query>(const Query &query) {
                if constexpr (std::is_same_v<Query, PhysicsRayQuery>) {
                    CollectRayQuery(world, query, collectors, bodyFilter);
                } else if constexpr (std::is_same_v<Query, PhysicsPointQuery>) {
                    CollectPointQuery(world, query, collectors, bodyFilter);
                } else if constexpr (std::is_same_v<Query, PhysicsOverlapQuery>) {
                    CollectOverlapQuery(world, query, *source, collectors, bodyFilter);
                } else {
                    static_assert(std::is_same_v<Query, PhysicsSweepQuery>);
                    CollectSweepQuery(world, query, *source, collectors, bodyFilter);
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

        /** @brief Copies one native result into stable Horo query evidence. */
        [[nodiscard]] Result<void> AppendCanonicalHit(const CanonicalWorld &world, const PhysicsQueryDescriptor &descriptor,
                                                      std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> &candidates,
                                                      std::size_t &candidateCount, const CanonicalHitEvidence &evidence) {
            const auto *fixture = FindFixture(world, evidence.body);
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
                                .filterSchemaGeneration = world.query.querySchemaGeneration,
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
        [[nodiscard]] Result<void> AppendCollectedQueryHits(const CanonicalWorld &world, const PhysicsQueryDescriptor &descriptor,
                                                            const FixedQueryCollector<Collector> &collector,
                                                            std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> &candidates,
                                                            std::size_t &candidateCount, const EvidenceFunction &evidenceFunction) {
            for (std::size_t index = 0; index < collector.count; ++index) {
                if (const Result<void> appended =
                        AppendCanonicalHit(world, descriptor, candidates, candidateCount, evidenceFunction(collector.values[index]));
                    appended.HasError())
                    return appended;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] Result<void> AppendRayQueryHits(const CanonicalWorld &world, const PhysicsQueryDescriptor &descriptor,
                                                      const PhysicsRayQuery &ray,
                                                      const FixedQueryCollector<JPH::CastRayCollector> &collector,
                                                      std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> &candidates,
                                                      std::size_t &candidateCount) {
            return AppendCollectedQueryHits(world, descriptor, collector, candidates, candidateCount, [&world, &ray](const auto &hit) {
                const Math::Vec3 position = ray.origin + ray.direction * (ray.maximumDistanceMeters * hit.mFraction);
                return CanonicalHitEvidence{.body = hit.mBodyID,
                                            .position = position,
                                            .normal = RayNormal(world, hit.mBodyID, hit.mSubShapeID2, position),
                                            .distance = ray.maximumDistanceMeters * hit.mFraction};
            });
        }

        [[nodiscard]] Result<void> AppendPointQueryHits(const CanonicalWorld &world, const PhysicsQueryDescriptor &descriptor,
                                                        const PhysicsPointQuery &point,
                                                        const FixedQueryCollector<JPH::CollidePointCollector> &collector,
                                                        std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> &candidates,
                                                        std::size_t &candidateCount) {
            return AppendCollectedQueryHits(world, descriptor, collector, candidates, candidateCount, [&point](const auto &hit) {
                return CanonicalHitEvidence{.body = hit.mBodyID, .position = point.point, .normal = std::nullopt, .distance = 0.0F};
            });
        }

        template <typename Collector, typename DistanceFunction>
        [[nodiscard]] Result<void> AppendContactQueryHits(const CanonicalWorld &world, const PhysicsQueryDescriptor &descriptor,
                                                          const FixedQueryCollector<Collector> &collector,
                                                          std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> &candidates,
                                                          std::size_t &candidateCount, const DistanceFunction &distanceFunction) {
            return AppendCollectedQueryHits(world, descriptor, collector, candidates, candidateCount, [&distanceFunction](const auto &hit) {
                const Math::Vec3 position{hit.mContactPointOn2.GetX(), hit.mContactPointOn2.GetY(), hit.mContactPointOn2.GetZ()};
                return CanonicalHitEvidence{.body = hit.mBodyID2,
                                            .position = position,
                                            .normal = ContactNormal(hit.mPenetrationAxis),
                                            .distance = distanceFunction(hit)};
            });
        }

        /** @brief Projects native query collectors into deterministic, bounded Horo hit storage. */
        [[nodiscard]] Result<void> ProjectCanonicalQueryHits(const CanonicalWorld &world, const PhysicsQueryDescriptor &descriptor,
                                                             const CanonicalQueryCollectors &collectors,
                                                             std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> &candidates,
                                                             std::size_t &candidateCount) {
            if (const auto *ray = std::get_if<PhysicsRayQuery>(&descriptor.geometry))
                return AppendRayQueryHits(world, descriptor, *ray, collectors.ray, candidates, candidateCount);
            if (const auto *point = std::get_if<PhysicsPointQuery>(&descriptor.geometry))
                return AppendPointQueryHits(world, descriptor, *point, collectors.point, candidates, candidateCount);
            if (std::holds_alternative<PhysicsOverlapQuery>(descriptor.geometry))
                return AppendContactQueryHits(world, descriptor, collectors.overlap, candidates, candidateCount, [](const auto &) {
                    return 0.0F;
                });
            const auto &sweep = std::get<PhysicsSweepQuery>(descriptor.geometry);
            return AppendContactQueryHits(world, descriptor, collectors.sweep, candidates, candidateCount, [&sweep](const auto &hit) {
                return hit.mFraction * sweep.maximumDistanceMeters;
            });
        }

        /** @brief Applies collection policy, caller-storage truncation and result metadata. */
        [[nodiscard]] Result<PhysicsQueryResult> FinalizeCanonicalQuery(const PhysicsQueryDescriptor &descriptor,
                                                                        std::array<PhysicsQueryHit, MaximumPhysicsQueryHits> &candidates,
                                                                        const std::size_t candidateCount,
                                                                        const std::span<PhysicsQueryHit> hits,
                                                                        const std::uint64_t schemaGeneration) {
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
                                      .filterSchemaGeneration = schemaGeneration,
                                      .broadphaseSnapshotGeneration = schemaGeneration};
            if (const Result<void> valid = ValidatePhysicsQueryResult(result, descriptor); valid.HasError())
                return Result<PhysicsQueryResult>::Failure(valid.ErrorValue());
            return Result<PhysicsQueryResult>::Success(result);
        }
    }  // namespace

    /** @copydoc CreateCanonicalQueryFixture */
    Result<PhysicsQueryFixture> CreateCanonicalQueryFixture(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                            const PhysicsQueryFixtureDescriptor &fixture) {
        if (world.value == nullptr || !owner.IsValid())
            return Result<PhysicsQueryFixture>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        if (canonical.query.nextFixtureSlot == std::numeric_limits<std::uint32_t>::max() ||
            canonical.query.nextFixtureGeneration == std::numeric_limits<std::uint32_t>::max() ||
            canonical.query.fixtures.size() >= canonical.query.maximumFixtures)
            return Result<PhysicsQueryFixture>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        const auto nativeShape = CreateNativeShape(fixture.shape);
        if (nativeShape.HasError())
            return Result<PhysicsQueryFixture>::Failure(nativeShape.ErrorValue());

        const std::uint32_t slot = canonical.query.nextFixtureSlot++;
        const std::uint32_t generation = canonical.query.nextFixtureGeneration++;
        const PhysicsQueryFixture identity{.body = {owner, {slot, generation}}, .shape = {owner, {slot, generation}}};
        JPH::BodyCreationSettings settings(nativeShape.Value().GetPtr(),
                                           JPH::RVec3(fixture.pose.translation.x, fixture.pose.translation.y, fixture.pose.translation.z),
                                           ToNative(fixture.pose.rotation), JPH::EMotionType::Static, JPH::ObjectLayer{0});
        settings.mIsSensor = fixture.trigger;
        const JPH::BodyID nativeBody =
            canonical.native.system->GetBodyInterface().CreateAndAddBody(settings, JPH::EActivation::DontActivate);
        if (nativeBody.IsInvalid())
            return Result<PhysicsQueryFixture>::Failure(
                MakeError(PhysicsErrors::CapacityExceeded, "Canonical solver rejected the query fixture body admission."));
        if (nativeBody.GetIndex() >= canonical.query.nativeFixtureIndices.size()) {
            canonical.native.system->GetBodyInterface().RemoveBody(nativeBody);
            canonical.native.system->GetBodyInterface().DestroyBody(nativeBody);
            return Result<PhysicsQueryFixture>::Failure(MakeError(PhysicsErrors::CapacityExceeded));
        }
        canonical.query.fixtures.push_back(
            {.fixture = identity, .descriptor = fixture, .nativeBody = nativeBody, .shape = nativeShape.Value()});
        canonical.query.nativeFixtureIndices[nativeBody.GetIndex()] = canonical.query.fixtures.size() - 1;
        ++canonical.query.querySchemaGeneration;
        return Result<PhysicsQueryFixture>::Success(identity);
    }

    /** @copydoc DestroyCanonicalQueryFixture */
    Result<void> DestroyCanonicalQueryFixture(const CanonicalWorldHandle world, const PhysicsQueryFixture &fixture) {
        if (world.value == nullptr)
            return Result<void>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        if (const auto owner = ValidatePhysicsHandleOwner(fixture.body, fixture.body.world); owner.HasError())
            return owner;
        if (fixture.body.world.Value() != fixture.shape.world.Value() || fixture.body.slot.index != fixture.shape.slot.index ||
            fixture.body.slot.generation != fixture.shape.slot.generation)
            return Result<void>::Failure(MakeError(PhysicsErrors::DescriptorInvalid, "Fixture body and shape identities must be paired."));
        const auto found = std::ranges::find_if(canonical.query.fixtures, [&fixture](const auto &entry) {
            return entry.fixture == fixture;
        });
        if (found == canonical.query.fixtures.end())
            return Result<void>::Failure(MakeError(PhysicsErrors::HandleStale));
        canonical.native.system->GetBodyInterface().RemoveBody(found->nativeBody);
        canonical.native.system->GetBodyInterface().DestroyBody(found->nativeBody);
        const auto erasedIndex = static_cast<std::size_t>(std::distance(canonical.query.fixtures.begin(), found));
        canonical.query.nativeFixtureIndices[found->nativeBody.GetIndex()] = CanonicalWorldQueryState::InvalidFixtureIndex;
        canonical.query.fixtures.erase(found);
        if (erasedIndex < canonical.query.fixtures.size())
            canonical.query.nativeFixtureIndices[canonical.query.fixtures[erasedIndex].nativeBody.GetIndex()] = erasedIndex;
        ++canonical.query.querySchemaGeneration;
        return Result<void>::Success();
    }

    /** @copydoc ExecuteCanonicalQuery */
    Result<PhysicsQueryResult> ExecuteCanonicalQuery(const CanonicalWorldHandle world, const PhysicsQueryDescriptor &descriptor,
                                                     const std::span<PhysicsQueryHit> hits) {
        if (world.value == nullptr)
            return Result<PhysicsQueryResult>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        CanonicalQueryCollectors collectors{canonical, descriptor.collection};
        const QueryBodyFilter bodyFilter{canonical, descriptor};
        if (const Result<void> collected = CollectCanonicalQuery(canonical, descriptor, collectors, bodyFilter); collected.HasError())
            return Result<PhysicsQueryResult>::Failure(collected.ErrorValue());

        auto &candidates = canonical.query.queryCandidates;
        std::size_t candidateCount{};
        if (const Result<void> projected = ProjectCanonicalQueryHits(canonical, descriptor, collectors, candidates, candidateCount);
            projected.HasError())
            return Result<PhysicsQueryResult>::Failure(projected.ErrorValue());

        return FinalizeCanonicalQuery(descriptor, candidates, candidateCount, hits, canonical.query.querySchemaGeneration);
    }
}  // namespace Horo::Physics::Detail
