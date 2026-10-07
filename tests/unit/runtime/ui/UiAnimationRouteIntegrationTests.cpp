#include "UiAnimationOwnerIntegrationFixture.h"

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace Horo::Runtime::Ui;
    using namespace Horo::Runtime::Ui::ReloadTests;
    using namespace Horo::Runtime::Ui::AnimationTests;

    /** @brief Qualifies the prospective child scope and receipt gate before the real route instance is committed. */
    UiClockSample CheckEnteringRoute(UiAnimationRuntimeParticipant &participant, const UiAnimationFrameLease &entering,
                                     const UiRouteOperationId operation) {
        REQUIRE(entering.RouteOperation().has_value());
        CHECK(entering.RouteOperation()->operation == operation);
        CHECK(entering.RouteOperation()->phase == UiAnimationRoutePhase::Entering);
        CHECK_FALSE(entering.RouteOperation()->terminal.has_value());
        CHECK(entering.Routes().empty());
        const auto child = entering.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)];
        CHECK(child.available);
        CHECK(child.route.IsValid());
        CHECK(child.routeOperation == operation.sequence);
        REQUIRE(participant.ApplyPresentation(Receipt(entering, 1)).HasValue());
        CHECK_FALSE(participant.InputEligible(Receipt(entering, 1).view));
        return child;
    }
}  // namespace

TEST_CASE("Real host navigation gates stack publication on its privately scoped enter and exit timelines",
          "[runtime_ui][animation][integration][routes]") {
    HostFixture fixture{{.routeMotion = true}};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto initial = Frame(participant);
    REQUIRE(initial.Routes().empty());
    CHECK(participant.Start(Stable<UiAnimationId>(2)).HasError());
    auto admitted = participant.Navigate(UiRouteOperationRequest::Push(Stable<UiRouteId>(9)));
    REQUIRE(admitted.HasValue());
    const auto operation = admitted.Value();
    CHECK(participant.Navigate(UiRouteOperationRequest::Push(Stable<UiRouteId>(9))).HasError());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto entering = Frame(participant);
    const auto child = CheckEnteringRoute(participant, entering, operation);
    entering = {};
    fixture.clock.Advance(Duration::FromMilliseconds(100));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto entered = Frame(participant);
    REQUIRE(entered.RouteOperation()->terminal.has_value());
    CHECK(entered.RouteOperation()->terminal->outcome == UiRouteOperationOutcome::Committed);
    REQUIRE(entered.Routes().size() == 1);
    const auto route = entered.Routes().front().id;
    CHECK(route == child.route);
    CHECK(initial.Routes().empty());
    CHECK_FALSE(participant.InputEligible(Receipt(entered, 2).view));
    REQUIRE(participant.ApplyPresentation(Receipt(entered, 2)).HasValue());
    CHECK(participant.InputEligible(Receipt(entered, 2).view));
    initial = {};
    auto exiting = participant.Navigate(UiRouteOperationRequest::Pop());
    REQUIRE(exiting.HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto pending = Frame(participant);
    CHECK(pending.RouteOperation()->phase == UiAnimationRoutePhase::Exiting);
    CHECK(pending.Routes().front().id == route);
    CHECK(pending.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)].clock != child.clock);
    CHECK(pending.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)].route == route);
    pending = {};
    fixture.clock.Advance(Duration::FromMilliseconds(100));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto removed = Frame(participant);
    CHECK(removed.Routes().empty());
    CHECK(removed.RouteOperation()->phase == UiAnimationRoutePhase::Completed);
    CHECK(entered.Routes().front().id == route);
    CHECK(participant.CancelNavigation(operation, UiAnimationCancellation::Explicit).HasError());
}

TEST_CASE("Actual route cancellation keeps the prior stack and rejects late or foreign operation identities",
          "[runtime_ui][animation][integration][routes][cancel]") {
    HostFixture fixture{{.routeMotion = true}};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    const auto operation = participant.Navigate(UiRouteOperationRequest::Push(Stable<UiRouteId>(9)));
    REQUIRE(operation.HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto pending = Frame(participant);
    auto foreign = operation.Value();
    foreign.ownership = UiOwnershipGeneration::Create(ReloadTests::Owner().Value() + 1).Value();
    CHECK(participant.CancelNavigation(foreign, UiAnimationCancellation::Explicit).HasError());
    REQUIRE(participant.CancelNavigation(operation.Value(), UiAnimationCancellation::Explicit).HasValue());
    REQUIRE(participant.CancelNavigation(operation.Value(), UiAnimationCancellation::Explicit).HasValue());
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto cancelled = Frame(participant);
    REQUIRE(cancelled.RouteOperation()->terminal.has_value());
    CHECK(cancelled.RouteOperation()->phase == UiAnimationRoutePhase::Cancelled);
    CHECK(cancelled.RouteOperation()->cancellation == UiAnimationCancellation::Explicit);
    CHECK(cancelled.RouteOperation()->terminal->rejection == UiRouteOperationRejection::Cancelled);
    CHECK(cancelled.Routes().empty());
    CHECK_FALSE(pending.RouteOperation()->terminal.has_value());
    CHECK(participant.CancelNavigation(operation.Value(), UiAnimationCancellation::Explicit).HasError());
    pending = {};
    CHECK(participant.Navigate(UiRouteOperationRequest::Push(Stable<UiRouteId>(9))).HasValue());
}

TEST_CASE("The real child-clock deadline cancels an incomplete required route without publishing its instance",
          "[runtime_ui][animation][integration][routes][deadline]") {
    HostFixture fixture{{.routeMotion = true, .deadline = 20000000}};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    REQUIRE(participant.Navigate(UiRouteOperationRequest::Push(Stable<UiRouteId>(9))).HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    fixture.clock.Advance(Duration::FromMilliseconds(20));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto cancelled = Frame(participant);
    REQUIRE(cancelled.RouteOperation()->terminal.has_value());
    CHECK(cancelled.RouteOperation()->cancellation == UiAnimationCancellation::Deadline);
    CHECK(cancelled.RouteOperation()->phase == UiAnimationRoutePhase::Cancelled);
    CHECK(cancelled.Routes().empty());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto stable = Frame(participant);
    CHECK_FALSE(stable.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)].available);
    CHECK(stable.RouteOperation()->terminal->operation == cancelled.RouteOperation()->terminal->operation);
    CHECK(stable.RouteOperation()->terminal->revision == cancelled.RouteOperation()->terminal->revision);
    CHECK(stable.RouteOperation()->terminal->rejection == cancelled.RouteOperation()->terminal->rejection);
}

TEST_CASE("Shutdown revokes a real pending gate while its immutable frame and scoped child evidence stay pinned",
          "[runtime_ui][animation][integration][routes][lifecycle]") {
    HostFixture fixture{{.routeMotion = true}};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto operation = participant.Navigate(UiRouteOperationRequest::Push(Stable<UiRouteId>(9)));
    REQUIRE(operation.HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto retained = Frame(participant);
    const auto child = retained.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)];
    fixture.host->Shutdown();
    CHECK(participant.Acquire().HasError());
    CHECK(participant.Navigate(UiRouteOperationRequest::Push(Stable<UiRouteId>(9))).HasError());
    CHECK(participant.CancelNavigation(operation.Value(), UiAnimationCancellation::Explicit).HasError());
    CHECK(retained.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)].clock == child.clock);
    CHECK(retained.RouteOperation()->phase == UiAnimationRoutePhase::Entering);
    CHECK_FALSE(retained.RouteOperation()->terminal.has_value());
}

TEST_CASE("Actual replacement progresses exit then enter with fresh child and route incarnations and no old-terminal replay",
          "[runtime_ui][animation][integration][routes][replacement]") {
    HostFixture fixture{{.routeMotion = true}};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    REQUIRE(participant.Navigate(UiRouteOperationRequest::Push(Stable<UiRouteId>(9))).HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    fixture.clock.Advance(Duration::FromMilliseconds(100));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto original = Frame(participant);
    REQUIRE(original.Routes().size() == 1);
    const auto originalRoute = original.Routes().front().id;
    const auto originalChild = original.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)].clock;
    REQUIRE_FALSE(original.Timelines().empty());
    const auto terminal = original.Timelines().front().timeline;
    auto replacement = participant.Navigate(UiRouteOperationRequest::Replace(Stable<UiRouteId>(9)));
    REQUIRE(replacement.HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto exiting = Frame(participant);
    REQUIRE(exiting.RouteOperation()->phase == UiAnimationRoutePhase::Exiting);
    CHECK(exiting.RouteOperation()->scope == originalRoute);
    CHECK(exiting.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)].clock != originalChild);
    CHECK(participant.Cancel(terminal, UiAnimationCancellation::Explicit).HasError());
    exiting = {};
    fixture.clock.Advance(Duration::FromMilliseconds(100));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto staged = Frame(participant);
    CHECK(staged.RouteOperation()->phase == UiAnimationRoutePhase::Waiting);
    CHECK_FALSE(staged.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)].available);
    CHECK(staged.Routes().front().id == originalRoute);
    CHECK(staged.RouteOperation()->scope != originalRoute);
    staged = {};
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto entering = Frame(participant);
    const auto enteringClock = entering.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)].clock;
    CHECK(enteringClock != originalChild);
    CHECK(entering.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)].route != originalRoute);
    entering = {};
    fixture.clock.Advance(Duration::FromMilliseconds(100));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto replaced = Frame(participant);
    CHECK(replaced.RouteOperation()->phase == UiAnimationRoutePhase::Completed);
    REQUIRE(replaced.Routes().size() == 1);
    CHECK(replaced.Routes().front().id != originalRoute);
    CHECK(original.Routes().front().id == originalRoute);
    CHECK(replaced.RouteOperation()->terminal->operation == replacement.Value());
}

TEST_CASE("The owner preserves actual stack admission rejection and never enables a child for an unknown catalog route",
          "[runtime_ui][animation][integration][routes][admission]") {
    HostFixture fixture{{.routeMotion = true}};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto refused = participant.Navigate(UiRouteOperationRequest::Push(Stable<UiRouteId>(99)));
    REQUIRE(refused.HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto rejected = Frame(participant);
    REQUIRE(rejected.RouteOperation()->terminal.has_value());
    CHECK(rejected.RouteOperation()->phase == UiAnimationRoutePhase::Rejected);
    CHECK(rejected.RouteOperation()->terminal->operation == refused.Value());
    CHECK(rejected.RouteOperation()->terminal->rejection == UiRouteOperationRejection::NotFound);
    CHECK(rejected.Routes().empty());
    CHECK_FALSE(rejected.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::ScreenTransition)].available);
}

TEST_CASE("Actual drained route action owners retire without frame reclamation and reclaim only at explicit quiescence",
          "[runtime_ui][animation][integration][routes][actions][allocation]") {
    HostFixture fixture{{.routeMotion = true, .routeActions = true}};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.attachedRouteActions);
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto first = Frame(participant);
    const auto source = first.Controls().front().source;
    auto &router = *fixture.attachedRouteActions;
    REQUIRE(router.Enqueue(source, UiButtonActionCommand{Stable<UiActionId>(8), {}}).HasValue());
    RetainedAnimationHandler handler;
    REQUIRE(router.DispatchNext(handler).HasValue());
    REQUIRE(handler.producer.has_value());
    const auto key = handler.producer->Key();
    REQUIRE(handler.producer->Complete().HasValue());
    REQUIRE(router.AsyncActions()->Release(key).HasValue());
    handler.producer.reset();
    REQUIRE(router.LastIssuedSequence() == key.request.sequence);
    REQUIRE(participant.Navigate(UiRouteOperationRequest::Pop()).HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    first = {};
    fixture.clock.Advance(Duration::FromMilliseconds(100));
    const auto allocations = Horo::Tests::AllocationProbe::Count();
    const auto frees = Horo::Tests::AllocationProbe::FreeCount();
    const auto completed = fixture.host->RunFrame();
    const auto after = Horo::Tests::AllocationProbe::Count();
    const auto freed = Horo::Tests::AllocationProbe::FreeCount();
    REQUIRE(completed.HasValue());
    CHECK(after == allocations);
    CHECK(freed == frees);
    auto removed = Frame(participant);
    CHECK(removed.Routes().empty());
    CHECK(removed.RouteOperation()->phase == UiAnimationRoutePhase::Completed);
    auto reclaimed = participant.DrainRetired();
    REQUIRE(reclaimed.HasValue());
    CHECK(reclaimed.Value() >= 1);
    CHECK(removed.Routes().empty());
}
