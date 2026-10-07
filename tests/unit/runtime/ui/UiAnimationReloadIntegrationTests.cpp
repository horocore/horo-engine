#include "UiAnimationOwnerIntegrationFixture.h"

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace Horo::Runtime::Ui;
    using namespace Horo::Runtime::Ui::ReloadTests;
    using namespace Horo::Runtime::Ui::AnimationTests;
}  // namespace

TEST_CASE("Real asset generation re-admission uses the same canvas issuer and rejects old animation and input incarnations",
          "[runtime_ui][animation][integration][reload][identity]") {
    auto issued = UiElementSlotAllocator::Create(ReloadTests::Owner());
    REQUIRE(issued.HasValue());
    auto allocator = std::move(issued).Value();
    HostFixture firstHost{{.allocator = &allocator}};
    auto &original = *firstHost.participant;
    REQUIRE(firstHost.host->RunFrame().HasValue());
    auto retained = Frame(original);
    const auto oldSource = retained.Controls().front().source;
    auto oldTimeline = original.Start(Stable<UiAnimationId>(1));
    REQUIRE(oldTimeline.HasValue());
    const auto oldClock = retained.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::PresentationUnscaled)].clock;
    firstHost.host->Shutdown();
    // Reload is an explicit application retirement/re-admission; this does not pretend a live host resumes after teardown.
    HostFixture replacement{{.allocator = &allocator, .version = 2}};
    auto &current = *replacement.participant;
    REQUIRE(replacement.host->RunFrame().HasValue());
    auto admitted = Frame(current);
    CHECK(admitted.Controls().front().source.element != oldSource.element);
    CHECK(admitted.Controls().front().source.owner.documentRevision == Revision<UiDocumentRevision>(2));
    CHECK(admitted.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::PresentationUnscaled)].clock != oldClock);
    REQUIRE(current.ApplyPresentation(Receipt(admitted, 1)).HasValue());
    CHECK(current.HandleControl(Receipt(admitted, 1).view, Edge(oldSource, UiControlInputKind::FocusGained, 1)).HasError());
    CHECK(current.Cancel(oldTimeline.Value(), UiAnimationCancellation::Explicit).HasError());
    CHECK(retained.Controls().front().source == oldSource);
    CHECK(retained.Clocks().domains[static_cast<std::size_t>(UiTimeDomain::PresentationUnscaled)].clock == oldClock);
    auto fresh = current.Start(Stable<UiAnimationId>(1));
    REQUIRE(fresh.HasValue());
    CHECK(fresh.Value() != oldTimeline.Value());
    replacement.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(replacement.host->RunFrame().HasValue());
}

TEST_CASE("Real animation reload reconciles compatible draft and invalidates source and controller handles",
          "[runtime_ui][animation][integration][reload]") {
    auto issued = UiElementSlotAllocator::Create(ReloadTests::Owner());
    REQUIRE(issued.HasValue());
    auto allocator = std::move(issued).Value();
    HostFixture fixture{{.allocator = &allocator, .application = UiAnimationApplication::Manual}};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto retained = Frame(participant);
    auto receipt = Receipt(retained, 1);
    REQUIRE(participant.ApplyPresentation(receipt).HasValue());
    const auto source = retained.Controls().front().source;
    REQUIRE(participant.HandleControl(receipt.view, Edge(source, UiControlInputKind::FocusGained, 1)).HasValue());
    REQUIRE(participant.HandleControl(receipt.view, Edge(source, UiControlInputKind::TextInput, 2, "draft")).HasValue());
    const auto oldClock = fixture.controller.Clock(UiTimeDomain::Manual);
    REQUIRE(oldClock.HasValue());
    auto replacement = Generation(allocator, 100);
    auto resources = Resources(replacement.Canvases().front());
    auto reloaded = participant.Reload(std::move(replacement), allocator, std::move(resources.registry), std::move(resources.styles),
                                       std::move(resources.definition), UiAnimationReloadPolicy::Cancel,
                                       UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands);
    REQUIRE(reloaded.HasValue());
    CHECK(reloaded.Value().result.state.preservedControls == 1);
    CHECK(fixture.controller.Step(oldClock.Value(), UiDuration{1}).HasError());
    CHECK(participant.Acquire().HasError());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto current = Frame(participant);
    CHECK(current.Controls().front().source != source);
    CHECK(std::get<UiTextInputControlState>(current.Controls().front().state).text.View() == "basedraft");
    CHECK(std::get<UiTextInputControlState>(retained.Controls().front().state).text.View() == "base");
    REQUIRE(participant.ApplyPresentation(Receipt(current, 2)).HasValue());
    CHECK(participant.HandleControl(receipt.view, Edge(source, UiControlInputKind::Cancel, 3)).HasError());
    const auto cancelled = participant.HandleControl(receipt.view, Edge(current.Controls().front().source, UiControlInputKind::Cancel, 3));
    REQUIRE(cancelled.HasValue());
    CHECK(std::get<UiTextInputControlState>(cancelled.Value().state).text.View() == "base");
    const auto freshClock = reloaded.Value().controller.Clock(UiTimeDomain::Manual);
    REQUIRE(freshClock.HasValue());
    CHECK(freshClock.Value() != oldClock.Value());
}

TEST_CASE("Rejected and cancelled real animation reloads preserve the existing frame and cursor",
          "[runtime_ui][animation][integration][reload][rollback]") {
    auto issued = UiElementSlotAllocator::Create(ReloadTests::Owner());
    REQUIRE(issued.HasValue());
    auto allocator = std::move(issued).Value();
    HostFixture fixture{{.allocator = &allocator}};
    auto &participant = *fixture.participant;
    auto timeline = participant.Start(Stable<UiAnimationId>(1));
    REQUIRE(timeline.HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto retained = Frame(participant);
    auto replacement = Generation(allocator, 100);
    auto resources = Resources(replacement.Canvases().front());
    CancellationSource cancelled;
    SECTION("actual load ancestry cancelled") {
        cancelled.RequestCancellation();
    }
    SECTION("authored replacement missing actual element") {
        resources.definition.elements.pop_back();
    }
    auto result = participant.Reload(std::move(replacement), allocator, std::move(resources.registry), std::move(resources.styles),
                                     std::move(resources.definition), UiAnimationReloadPolicy::Cancel,
                                     UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands, cancelled.Token());
    REQUIRE(result.HasError());
    auto unchanged = Frame(participant);
    CHECK(unchanged.Timelines().front().timeline == timeline.Value());
    CHECK(unchanged.Clocks().updateSequence == retained.Clocks().updateSequence);
    CHECK(unchanged.Layout().Descriptor().interaction == retained.Layout().Descriptor().interaction);
    fixture.clock.Advance(Duration::FromMilliseconds(100));
    REQUIRE(fixture.host->RunFrame().HasValue());
    CHECK(Frame(participant).Timelines().front().playback.outcome == UiAnimationOutcome::Completed);
}

TEST_CASE("Explicit real reload restart closes the old instance once and starts a fresh cursor",
          "[runtime_ui][animation][integration][reload][restart]") {
    auto issued = UiElementSlotAllocator::Create(ReloadTests::Owner());
    REQUIRE(issued.HasValue());
    auto allocator = std::move(issued).Value();
    HostFixture fixture{{.allocator = &allocator}};
    auto &participant = *fixture.participant;
    auto timeline = participant.Start(Stable<UiAnimationId>(1));
    REQUIRE(timeline.HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    fixture.clock.Advance(Duration::FromMilliseconds(40));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto retained = Frame(participant);
    auto replacement = Generation(allocator, 100);
    auto resources = Resources(replacement.Canvases().front());
    auto result = participant.Reload(std::move(replacement), allocator, std::move(resources.registry), std::move(resources.styles),
                                     std::move(resources.definition), UiAnimationReloadPolicy::Restart,
                                     UiStructuralCommitPoint::CommitDeferredLifecycleChanges);
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().result.terminal.size() == 1);
    CHECK(result.Value().result.terminal.front().timeline == timeline.Value());
    CHECK(result.Value().result.terminal.front().playback.cancellation == UiAnimationCancellation::Reload);
    CHECK(result.Value().result.terminal.front().playback.newTerminalOutcome);
    CHECK(result.Value().result.restartedTimelines == 1);
    CHECK(participant.Cancel(timeline.Value(), UiAnimationCancellation::Explicit).HasError());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto fresh = Frame(participant);
    REQUIRE(fresh.Timelines().size() == 1);
    CHECK(fresh.Timelines().front().timeline != timeline.Value());
    CHECK(fresh.Timelines().front().playback.elapsed == UiDuration{});
    CHECK(retained.Timelines().front().playback.elapsed == UiDuration{40000000});
    fixture.clock.Advance(Duration::FromMilliseconds(100));
    REQUIRE(fixture.host->RunFrame().HasValue());
    CHECK(Frame(participant).Timelines().front().playback.outcome == UiAnimationOutcome::Completed);
}

TEST_CASE("A pending actual route gate rejects reload without cancelling its admitted transition",
          "[runtime_ui][animation][integration][reload][routes]") {
    auto issued = UiElementSlotAllocator::Create(ReloadTests::Owner());
    REQUIRE(issued.HasValue());
    auto allocator = std::move(issued).Value();
    HostFixture fixture{{.routeMotion = true, .allocator = &allocator}};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto operation = participant.Navigate(UiRouteOperationRequest::Push(Stable<UiRouteId>(9)));
    REQUIRE(operation.HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto retained = Frame(participant);
    auto replacement = Generation(allocator, 100);
    auto resources = Resources(replacement.Canvases().front(), true);
    CHECK(participant
              .Reload(std::move(replacement), allocator, std::move(resources.registry), std::move(resources.styles),
                      std::move(resources.definition), UiAnimationReloadPolicy::Cancel,
                      UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands)
              .HasError());
    fixture.clock.Advance(Duration::FromMilliseconds(100));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto completed = Frame(participant);
    REQUIRE(completed.RouteOperation());
    CHECK(completed.RouteOperation()->operation == operation.Value());
    CHECK(completed.RouteOperation()->phase == UiAnimationRoutePhase::Completed);
    REQUIRE(retained.RouteOperation());
    CHECK(retained.RouteOperation()->phase != UiAnimationRoutePhase::Completed);
}
