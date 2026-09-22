#include "CanonicalPhysicsRuntimeInternal.h"

namespace Horo::Physics::Detail {
    namespace {
        [[nodiscard]] CanonicalQueryAccess MakeQueryAccess(CanonicalWorld &world) {
            return {.system = *world.native.system,
                    .fixtures = world.query.fixtures,
                    .nativeFixtureIndices = world.query.nativeFixtureIndices,
                    .maximumFixtures = world.query.maximumFixtures,
                    .nextFixtureSlot = world.query.nextFixtureSlot,
                    .nextFixtureGeneration = world.query.nextFixtureGeneration,
                    .querySchemaGeneration = world.query.querySchemaGeneration,
                    .storage = world.query.storage};
        }
    }  // namespace

    /** @copydoc CreateCanonicalQueryFixture */
    Result<PhysicsQueryFixture> CreateCanonicalQueryFixture(const CanonicalWorldHandle world, const PhysicsWorldId owner,
                                                            const PhysicsQueryFixtureDescriptor &fixture) {
        if (world.value == nullptr)
            return Result<PhysicsQueryFixture>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        auto access = MakeQueryAccess(canonical);
        return CreateCanonicalQueryFixtureFromAccess(access, owner, fixture);
    }

    /** @copydoc DestroyCanonicalQueryFixture */
    Result<void> DestroyCanonicalQueryFixture(const CanonicalWorldHandle world, const PhysicsQueryFixture &fixture) {
        if (world.value == nullptr)
            return Result<void>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        auto access = MakeQueryAccess(canonical);
        return DestroyCanonicalQueryFixtureFromAccess(access, fixture);
    }

    /** @copydoc ExecuteCanonicalQuery */
    Result<PhysicsQueryResult> ExecuteCanonicalQuery(const CanonicalWorldHandle world, const PhysicsQueryDescriptor &descriptor,
                                                     const std::span<PhysicsQueryHit> hits) {
        if (world.value == nullptr)
            return Result<PhysicsQueryResult>::Failure(MakeError(PhysicsErrors::WorldInvalid));
        auto &canonical = *static_cast<CanonicalWorld *>(world.value);
        auto access = MakeQueryAccess(canonical);
        return ExecuteCanonicalQueryFromAccess(access, descriptor, hits);
    }
}  // namespace Horo::Physics::Detail
