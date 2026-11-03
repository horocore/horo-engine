#include "CanonicalPhysicsStateCloneInternal.h"
#include "PhysicsStateNativeTestSupport.h"

#include <limits>

namespace Horo::Physics::Detail {
    using namespace StateTest;

    TEST_CASE("Real Physics solver snapshot captures complete warm state without "
              "heap growth",
              "[physics][native][state][allocation]") {
        Fixture fixture;
        const auto state = fixture.Capture(*fixture.captured);
        REQUIRE(state.bodies.size() == 5);
        REQUIRE(state.constraints.size() == 4);
        REQUIRE_FALSE(state.manifolds.empty());
        REQUIRE(std::ranges::any_of(state.manifolds, [](const auto &manifold) {
            return std::ranges::any_of(std::span{manifold.points}.first(manifold.pointCount), [](const auto &point) {
                return point.normalLambda > 0;
            });
        }));
        Tests::AllocationProbe::Measurement measured;
        bool captured{};
        {
            Tests::AllocationProbe::ScopedMeasurement measurement;
            auto result = CaptureCanonicalPhysicsState(fixture.world.handle, fixture.settings, fixture.bindings.View(), *fixture.latest,
                                                       {fixture.temporary, fixture.nativeBytes, fixture.active, &fixture.metrics});
            captured = result.HasValue();
            measured = measurement.Snapshot();
        }
        REQUIRE(captured);
        CHECK(measured.requests == 0);
        CHECK(fixture.metrics.nativeTemporaryAllocations > 0);
        CHECK(fixture.metrics.nativeTemporaryBytes <= fixture.temporary.size());
        CHECK(fixture.metrics.nativeForeignReallocations == 0);
    }

    TEST_CASE("Real native Physics restore prepares exact canonical state while "
              "preserving the live solver",
              "[physics][native][state]") {
        Fixture fixture;
        const auto state = fixture.Capture(*fixture.captured);
        fixture.Step(31, 20);
        const auto before = fixture.Capture(*fixture.latest);
        std::vector<std::byte> beforeWire(1024 * 1024), afterWire(1024 * 1024);
        const auto beforeBytes =
            Take(StateDetail::EncodeSolverState(before, fixture.latest->limits, fixture.latest->activeBodyScratch, beforeWire));
        {
            const WorldOwner candidate{Take(CloneCanonicalPhysicsStateTopology(fixture.world.handle, fixture.settings))};
            Tests::AllocationProbe::Measurement measured;
            bool restored{};
            {
                Tests::AllocationProbe::ScopedMeasurement measurement;
                restored =
                    PrepareCanonicalPhysicsStateRestore(fixture.world.handle, candidate.handle, fixture.settings, fixture.bindings.View(),
                                                        state, fixture.Native().continuousCollision.policy, fixture.Scratch())
                        .HasValue();
                measured = measurement.Snapshot();
            }
            REQUIRE(restored);
            CHECK(measured.requests == 0);
            REQUIRE(candidate.handle.value != fixture.world.handle.value);
            CHECK(InspectCanonicalResources(fixture.runtime.handle) == CanonicalResourceCounts{2, 2, 2, 2});
            const auto unchanged = fixture.Capture(*fixture.latest);
            const auto afterBytes =
                Take(StateDetail::EncodeSolverState(unchanged, fixture.latest->limits, fixture.latest->activeBodyScratch, afterWire));
            REQUIRE(afterBytes == beforeBytes);
            CHECK(std::ranges::equal(std::span{beforeWire}.first(beforeBytes), std::span{afterWire}.first(afterBytes)));
        }
        CHECK(InspectCanonicalResources(fixture.runtime.handle) == CanonicalResourceCounts{1, 1, 1, 1});
    }

    TEST_CASE("Invalid real Physics restores never change source state or allocate "
              "another native candidate",
              "[physics][native][state][malformed]") {
        Fixture fixture;
        auto state = fixture.Capture(*fixture.captured);
        const WorldOwner candidate{Take(CloneCanonicalPhysicsStateTopology(fixture.world.handle, fixture.settings))};
        const auto resources = InspectCanonicalResources(fixture.runtime.handle);
        SECTION("missing body") {
            state.bodies = state.bodies.first(state.bodies.size() - 1);
        }
        SECTION("unknown native subshape") {
            fixture.captured->manifolds[0].key.firstSubshape = 0;
        }
        SECTION("non-finite warm-start impulse") {
            fixture.captured->manifolds[0].points[0].normalLambda = std::numeric_limits<float>::infinity();
        }
        SECTION("aliased expected and recapture bytes") {
            auto scratch = fixture.Scratch();
            scratch.actualWire = scratch.expectedWire;
            CHECK(PrepareCanonicalPhysicsStateRestore(fixture.world.handle, candidate.handle, fixture.settings, fixture.bindings.View(),
                                                      state, fixture.Native().continuousCollision.policy, scratch)
                      .HasError());
            CHECK(InspectCanonicalResources(fixture.runtime.handle) == resources);
            return;
        }
        CHECK(PrepareCanonicalPhysicsStateRestore(fixture.world.handle, candidate.handle, fixture.settings, fixture.bindings.View(), state,
                                                  fixture.Native().continuousCollision.policy, fixture.Scratch())
                  .HasError());
        CHECK(InspectCanonicalResources(fixture.runtime.handle) == resources);
    }
}  // namespace Horo::Physics::Detail
