#include "PhysicsStateWorldTestSupport.h"

#include <optional>
#include <thread>

namespace Horo::Physics::StateTest {
    TEST_CASE("Real Physics snapshot survives public runtime release and foreign last-drop requires explicit owner collection",
              "[physics][native][state][retirement]") {
        WorldFixture fixture;
        static_cast<void>(fixture.Configure());
        auto snapshot = fixture.Capture();
        const auto size = snapshot->Bytes().size();
        auto retirement = Take(fixture.runtime->BeginRetirement());
        auto waitingWorld = Take(retirement.Collect(0));
        CHECK(waitingWorld.phase == PhysicsRuntimeRetirementPhase::WaitingWorlds);
        CHECK(waitingWorld.pendingWorlds == 1);
        fixture.world.reset();
        fixture.runtime.reset();
        auto waitingReader = Take(retirement.Collect(64));
        CHECK(waitingReader.phase == PhysicsRuntimeRetirementPhase::WaitingStateLeases);
        CHECK(waitingReader.stateLeaseRecords == 1);
        bool immutable{};
        std::thread foreign([owned = std::move(snapshot), &immutable, size]() mutable {
            const auto copied = owned;
            immutable = copied.IsValid() && copied->Bytes().size() == size && copied->World().completedTick == 5;
            owned = {};
        });
        foreign.join();
        REQUIRE(immutable);
        // A foreign final release has no authority to erase the owner's record or tear down native globals.
        auto stillOwned = Take(retirement.Collect(0));
        CHECK(stillOwned.phase == PhysicsRuntimeRetirementPhase::WaitingStateLeases);
        CHECK(stillOwned.stateLeaseRecords == 1);
        CHECK(Take(retirement.Collect(1)).phase == PhysicsRuntimeRetirementPhase::Drained);
        CHECK(Take(retirement.Collect(1)).phase == PhysicsRuntimeRetirementPhase::Drained);
    }

    TEST_CASE("Real Physics bounded retirement is fair past a live prefix and wrong-thread collection has no authority",
              "[physics][native][state][retirement]") {
        WorldFixture fixture;
        static_cast<void>(fixture.Configure());
        auto first = fixture.Capture();
        static_cast<void>(fixture.Configure());
        auto later = fixture.Capture();
        auto retirement = Take(fixture.runtime->BeginRetirement());
        fixture.world.reset();
        fixture.runtime.reset();
        later = {};
        bool wrongThread{};
        std::thread foreign([&] {
            const auto result = retirement.Collect(1);
            wrongThread = result.HasError() && result.ErrorValue().code.Value() == PhysicsErrors::ThreadAffinityViolation.code.Value();
        });
        foreign.join();
        REQUIRE(wrongThread);
        const auto firstScan = Take(retirement.Collect(1));
        CHECK(firstScan.stateLeaseRecords >= 1);
        CHECK(firstScan.stateLeaseRecords <= 2);
        const auto nextScan = Take(retirement.Collect(1));
        CHECK(nextScan.stateLeaseRecords == 1);
        CHECK(firstScan.retiredRecords + nextScan.retiredRecords == 1);
        CHECK(first->World().completedTick == 5);
        first = {};
        CHECK(Take(retirement.Collect(1)).phase == PhysicsRuntimeRetirementPhase::Drained);
    }

    TEST_CASE("Real Physics unpublished candidate remains observably revoked after world shutdown until its real leases settle",
              "[physics][native][state][retirement][candidate]") {
        WorldFixture fixture;
        static_cast<void>(fixture.Configure());
        auto snapshot = fixture.Capture();
        std::optional<PhysicsStateRestoreCandidate> candidate{Take(fixture.world->PrepareStateRestore(snapshot))};
        auto retirement = Take(fixture.runtime->BeginRetirement());
        fixture.world.reset();
        fixture.runtime.reset();
        Test::RequireError(candidate->ValidatePublication(), PhysicsErrors::CapabilityRevoked);
        snapshot = {};
        CHECK(Take(retirement.Collect(64)).phase == PhysicsRuntimeRetirementPhase::WaitingStateLeases);
        std::thread foreign([owned = std::move(candidate)]() mutable {
            owned.reset();
        });
        foreign.join();
        CHECK(Take(retirement.Collect(0)).phase == PhysicsRuntimeRetirementPhase::WaitingStateLeases);
        CHECK(Take(retirement.Collect(64)).phase == PhysicsRuntimeRetirementPhase::Drained);
    }
}  // namespace Horo::Physics::StateTest
