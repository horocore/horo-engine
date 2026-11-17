#include "AiSceneTestSupport.h"
#include "AllocationProbe.h"

#include <thread>
#include <type_traits>

namespace Horo::AI {
    static_assert(!std::is_default_constructible_v<AiDecisionTaskReservation>);
    static_assert(!std::is_copy_constructible_v<AiDecisionTaskReservation>);
    static_assert(std::is_nothrow_move_constructible_v<AiDecisionTaskReservation>);
    static_assert(std::is_nothrow_move_assignable_v<AiDecisionTaskReservation>);

    namespace {
        using namespace TestSupport;

        TEST_CASE("Scene decision reservations share the actual native task namespace without reusing released indices",
                  "[unit][ai][scene][decision_reservation]") {
            RestoreHarness h;
            auto native = h.runtime.StartTaskAtSafePoint(h.handle, MakeIdentity<TaskId>(901));
            REQUIRE(native.HasValue());
            auto reserved = h.runtime.ReserveDecisionTaskSlotsAtSafePoint(h.handle, 4);
            REQUIRE(reserved.HasValue());
            CHECK(reserved.Value().FirstSlot() == native.Value().slot.index + 1);
            CHECK(reserved.Value().Capacity() == 4);
            auto snapshot = h.runtime.CaptureDecisionSnapshotAtSafePoint(h.handle);
            REQUIRE(snapshot.HasValue());
            CHECK(snapshot.Value().Binding() == reserved.Value().Binding());
            CHECK(snapshot.Value().Schema() == reserved.Value().Schema());
            REQUIRE(h.runtime.CapabilityAtSafePoint().CompleteTask(h.handle, native.Value()).HasValue());
            REQUIRE(h.runtime.CapabilityAtSafePoint().ReleaseTask(h.handle, native.Value()).HasValue());
            auto next = h.runtime.StartTaskAtSafePoint(h.handle, MakeIdentity<TaskId>(902));
            REQUIRE(next.HasValue());
            CHECK(next.Value().slot.index == reserved.Value().FirstSlot() + reserved.Value().Capacity());
        }

        TEST_CASE("Reservation moves transfer one cancellation owner and cancel the replaced scope",
                  "[unit][ai][scene][decision_reservation]") {
            RestoreHarness h;
            auto first = h.runtime.ReserveDecisionTaskSlotsAtSafePoint(h.handle, 2);
            auto second = h.runtime.ReserveDecisionTaskSlotsAtSafePoint(h.handle, 3);
            REQUIRE(first.HasValue());
            REQUIRE(second.HasValue());
            const auto firstToken = first.Value().Cancellation();
            const auto secondToken = second.Value().Cancellation();
            {
                auto moved = std::move(first).Value();
                CHECK_FALSE(first.Value().CanAdmit());
                CHECK(moved.CanAdmit());
                moved = std::move(second).Value();
                CHECK(firstToken.IsCancellationRequested());
                CHECK_FALSE(secondToken.IsCancellationRequested());
                CHECK_FALSE(second.Value().CanAdmit());
                CHECK(moved.FirstSlot() == 2);
                CHECK(moved.Capacity() == 3);
            }
            CHECK(secondToken.IsCancellationRequested());
            CHECK(h.runtime.CaptureDecisionSnapshotAtSafePoint(h.handle).HasValue());
            auto next = h.runtime.ReserveDecisionTaskSlotsAtSafePoint(h.handle, 1);
            REQUIRE(next.HasValue());
            CHECK(next.Value().FirstSlot() == 5);
        }

        TEST_CASE("Failed Scene reservation admission leaves the allocator unchanged", "[unit][ai][scene][decision_reservation]") {
            RestoreHarness h;
            SECTION("invalid agent") {
                CHECK(h.runtime.ReserveDecisionTaskSlotsAtSafePoint({}, 1).HasError());
            }
            SECTION("allocation failure") {
                auto failed = [&] {
                    Tests::AllocationProbe::ScopedFailure fail;
                    return h.runtime.ReserveDecisionTaskSlotsAtSafePoint(h.handle, 1);
                }();
                ExpectError(failed, AIErrors::TaskCapacityExceeded);
            }
            SECTION("foreign owner lane") {
                bool reserveRejected = false;
                bool snapshotRejected = false;
                std::thread foreign([&] {
                    reserveRejected = h.runtime.ReserveDecisionTaskSlotsAtSafePoint(h.handle, 1).HasError();
                    snapshotRejected = h.runtime.CaptureDecisionSnapshotAtSafePoint(h.handle).HasError();
                });
                foreign.join();
                CHECK(reserveRejected);
                CHECK(snapshotRejected);
            }
            auto next = h.runtime.ReserveDecisionTaskSlotsAtSafePoint(h.handle, 1);
            REQUIRE(next.HasValue());
            CHECK(next.Value().FirstSlot() == 0);
        }

        TEST_CASE("Logical task namespace exhaustion does not allocate storage proportional to its extent",
                  "[unit][ai][scene][decision_reservation]") {
            RestoreHarness h;
            constexpr auto End = Horo::Handle<TaskHandleTag>::InvalidIndex;
            auto reserved = h.runtime.ReserveDecisionTaskSlotsAtSafePoint(h.handle, End);
            REQUIRE(reserved.HasValue());
            CHECK(reserved.Value().FirstSlot() == 0);
            CHECK(reserved.Value().Capacity() == End);
            ExpectError(h.runtime.ReserveDecisionTaskSlotsAtSafePoint(h.handle, 1), AIErrors::TaskCapacityExceeded);
            ExpectError(h.runtime.StartTaskAtSafePoint(h.handle, MakeIdentity<TaskId>(901)), AIErrors::TaskCapacityExceeded);
            auto empty = h.runtime.ReserveDecisionTaskSlotsAtSafePoint(h.handle, 0);
            REQUIRE(empty.HasValue());
            CHECK(empty.Value().CanAdmit());
            CHECK(empty.Value().FirstSlot() == End);
            CHECK(empty.Value().Capacity() == 0);
        }

        TEST_CASE("Actual Scene lifecycle revokes decision scopes and frozen snapshot reads", "[unit][ai][scene][decision_reservation]") {
            RestoreHarness h;
            auto reserved = h.runtime.ReserveDecisionTaskSlotsAtSafePoint(h.handle, 3);
            REQUIRE(reserved.HasValue());
            auto snapshot = h.runtime.CaptureDecisionSnapshotAtSafePoint(h.handle);
            REQUIRE(snapshot.HasValue());
            SECTION("disable") {
                REQUIRE(h.runtime.DisableAtSafePoint(h.handle).HasValue());
                CHECK(h.runtime.ReserveDecisionTaskSlotsAtSafePoint(h.handle, 1).HasError());
                CHECK(h.runtime.CaptureDecisionSnapshotAtSafePoint(h.handle).HasError());
            }
            SECTION("canonical restore") {
                auto staged = h.Stage(h.Capture());
                auto committed = h.runtime.CommitRestoreAtSafePoint(std::move(staged));
                REQUIRE(committed.HasValue());
                auto fresh = h.runtime.ReserveDecisionTaskSlotsAtSafePoint(h.handle, 1);
                REQUIRE(fresh.HasValue());
                CHECK(fresh.Value().Binding() != reserved.Value().Binding());
                CHECK(fresh.Value().FirstSlot() >= 3);
            }
            SECTION("shutdown") {
                h.runtime.BeginShutdown();
            }
            SECTION("publication replacement") {
                auto replacement = Publish(h.runtime, h.fixtures.front());
                CHECK(h.runtime.CaptureDecisionSnapshotAtSafePoint(h.handle).HasError());
            }
            CHECK_FALSE(reserved.Value().CanAdmit());
            CHECK(reserved.Value().Cancellation().IsCancellationRequested());
            ExpectError(snapshot.Value().Revision(), AIErrors::BlackboardInstanceStale);
        }
    }  // namespace
}  // namespace Horo::AI
