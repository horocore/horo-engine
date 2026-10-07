#include "UiAnimationOwnerIntegrationFixture.h"

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace Horo::Runtime::Ui;
    using namespace Horo::Runtime::Ui::ReloadTests;
    using namespace Horo::Runtime::Ui::AnimationTests;
}  // namespace

TEST_CASE("Real host animation publishes matching style layout and clipping while retained geometry remains immutable",
          "[runtime_ui][animation][integration]") {
    HostFixture fixture;
    auto &clock = fixture.clock;
    auto &host = fixture.host;
    auto *participant = fixture.participant;
    REQUIRE(participant->Start(Stable<UiAnimationId>(1)).HasValue());
    REQUIRE(host->RunFrame().HasValue());
    auto first = Frame(*participant);
    CheckGeometry(first, 200);
    auto receipt = Receipt(first, 1);
    CHECK_FALSE(participant->InputEligible(receipt.view));
    REQUIRE(participant->ApplyPresentation(receipt).HasValue());
    CHECK(participant->InputEligible(receipt.view));
    clock.Advance(Duration::FromMilliseconds(100));
    REQUIRE(host->RunFrame().HasValue());
    auto next = Frame(*participant);
    CheckGeometry(next, 50);
    CHECK_FALSE(participant->InputEligible(receipt.view));
    CHECK(participant->ApplyPresentation(receipt).HasError());
    CHECK_FALSE(participant->InputEligible(receipt.view));
    receipt = Receipt(next, 2);
    REQUIRE(participant->ApplyPresentation(receipt).HasValue());
    CHECK(participant->InputEligible(receipt.view));
    CheckGeometry(first, 200);
    CHECK(first.Layout().Descriptor().interaction != next.Layout().Descriptor().interaction);
    host->Shutdown();
    CHECK(first.Clipping()->Scrolls().front().maximumOffset.y == 200);
    CHECK(next.Clipping()->Scrolls().front().maximumOffset.y == 50);
    CHECK(participant->Acquire().HasError());
}

TEST_CASE("Actual presented control replacement preserves draft Cancel baseline and rejects previous frame sources",
          "[runtime_ui][animation][integration][controls]") {
    HostFixture fixture;
    auto *participant = fixture.participant;
    REQUIRE(participant->Start(Stable<UiAnimationId>(1)).HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto old = Frame(*participant);
    REQUIRE(old.Controls().size() == 1);
    const auto oldSource = old.Controls().front().source;
    auto receipt = Receipt(old, 1);
    CHECK(participant->HandleControl(receipt.view, Edge(oldSource, UiControlInputKind::FocusGained, 1)).HasError());
    REQUIRE(participant->ApplyPresentation(receipt).HasValue());
    REQUIRE(participant->HandleControl(receipt.view, Edge(oldSource, UiControlInputKind::FocusGained, 1)).HasValue());
    REQUIRE(participant->HandleControl(receipt.view, Edge(oldSource, UiControlInputKind::TextInput, 2, "draft")).HasValue());
    CHECK(std::get<UiTextInputControlState>(old.Controls().front().state).text.View() == "base");
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto current = Frame(*participant);
    REQUIRE(current.Controls().size() == 1);
    const auto currentSource = current.Controls().front().source;
    CHECK(currentSource.owner.interaction == current.Layout().Descriptor().interaction);
    CHECK(currentSource.owner != oldSource.owner);
    CHECK(std::get<UiTextInputControlState>(current.Controls().front().state).text.View() == "basedraft");
    CHECK(participant->HandleControl(receipt.view, Edge(currentSource, UiControlInputKind::Cancel, 3)).HasError());
    receipt = Receipt(current, 2);
    REQUIRE(participant->ApplyPresentation(receipt).HasValue());
    CHECK(participant->HandleControl(receipt.view, Edge(oldSource, UiControlInputKind::Cancel, 3)).HasError());
    const auto cancelled = participant->HandleControl(receipt.view, Edge(currentSource, UiControlInputKind::Cancel, 3));
    REQUIRE(cancelled.HasValue());
    CHECK(std::get<UiTextInputControlState>(cancelled.Value().state).text.View() == "base");
    CHECK(std::get<UiTextInputControlState>(current.Controls().front().state).text.View() == "basedraft");
}

TEST_CASE("Real host animation preserves capture on stable geometry and cancels it before changed geometry becomes eligible",
          "[runtime_ui][animation][integration][capture]") {
    HostFixture fixture;
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto first = Frame(participant);
    REQUIRE_FALSE(first.Controls().empty());
    const auto receipt = Receipt(first, 1);
    const auto &layout = first.Layout().Descriptor();
    UiPointerCaptureRequest request{{ReloadTests::Owner(), 11, 1},
                                    UiPointerId::Create(1).Value(),
                                    UiPointerButton::Primary,
                                    receipt.view,
                                    {layout.instance,
                                     layout.canvas,
                                     layout.document,
                                     layout.sources.tree,
                                     layout.interaction,
                                     first.Controls().front().source.element,
                                     {}}};
    CHECK(participant.CapturePointer(request).HasError());
    REQUIRE(participant.ApplyPresentation(receipt).HasValue());
    auto admitted = participant.CapturePointer(request);
    REQUIRE(admitted.HasValue());
    auto token = std::move(admitted).Value();
    REQUIRE(token.IsActive());
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto stable = Frame(participant);
    CHECK(stable.Styles().Descriptor().geometry == first.Styles().Descriptor().geometry);
    CHECK(stable.Layout().Descriptor().interaction == layout.interaction);
    CHECK(participant.InputEligible(receipt.view));
    CHECK(token.IsActive());
    REQUIRE(participant.Start(Stable<UiAnimationId>(1)).HasValue());
    stable = {};
    REQUIRE(fixture.host->RunFrame().HasValue());
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto changed = Frame(participant);
    CHECK(changed.Layout().Descriptor().interaction != layout.interaction);
    CHECK_FALSE(token.IsActive());
    CHECK(token.CancellationReason() == UiPointerCaptureCancellationReason::InteractionRevisionLost);
    CHECK_FALSE(participant.InputEligible(receipt.view));
    CHECK(participant.CapturePointer(request).HasError());
    REQUIRE(participant.ApplyPresentation(Receipt(changed, 2)).HasValue());
    CHECK(participant.InputEligible(receipt.view));
    CHECK(participant.CapturePointer(request).HasError());
}

TEST_CASE("Stable host animation frames preserve an active press and its pending default source",
          "[runtime_ui][animation][integration][input]") {
    HostFixture fixture;
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto first = Frame(participant);
    REQUIRE_FALSE(first.Controls().empty());
    const auto source = first.Controls().front().source;
    const auto receipt = Receipt(first, 1);
    REQUIRE(participant.ApplyPresentation(receipt).HasValue());
    REQUIRE(participant.HandleControl(receipt.view, Edge(source, UiControlInputKind::FocusGained, 1)).HasValue());
    auto pressed = participant.HandleControl(receipt.view, Edge(source, UiControlInputKind::SubmitPress, 2));
    REQUIRE(pressed.HasValue());
    REQUIRE(std::get<UiTextInputControlState>(pressed.Value().state).pressed);
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto held = Frame(participant);
    REQUIRE(held.Controls().front().source == source);
    REQUIRE(std::get<UiTextInputControlState>(held.Controls().front().state).pressed);
    REQUIRE(held.Layout().Descriptor().interaction == first.Layout().Descriptor().interaction);
    REQUIRE(participant.InputEligible(receipt.view));
    auto released = participant.HandleControl(receipt.view, Edge(source, UiControlInputKind::SubmitRelease, 3));
    REQUIRE(released.HasValue());
    REQUIRE(released.Value().defaultActionPending);
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto pending = Frame(participant);
    REQUIRE(pending.Controls().front().source == source);
    REQUIRE(pending.Layout().Descriptor().interaction == first.Layout().Descriptor().interaction);
    auto applied = participant.ApplyControlDefault(receipt.view, source);
    REQUIRE(applied.HasValue());
    REQUIRE(applied.Value().has_value());
    CHECK(applied.Value()->source == source);
    CHECK(applied.Value()->eventSequence == 3);
    CHECK(applied.Value()->kind == UiControlActionKind::Submit);
    CHECK(std::get<UiTextInputControlState>(held.Controls().front().state).pressed);
    CHECK_FALSE(participant.ApplyControlDefault(receipt.view, source).Value().has_value());
}

TEST_CASE("A real attached async router remains live on stable geometry and blocks replacement until its producer drains",
          "[runtime_ui][animation][integration][async][rollback]") {
    HostFixture fixture{{.actions = true}};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.attachedActions);
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto initial = Frame(participant);
    const auto source = initial.Controls().front().source;
    REQUIRE(participant.ApplyPresentation(Receipt(initial, 1)).HasValue());
    auto &router = *fixture.attachedActions;
    REQUIRE(router.Enqueue(source, UiButtonActionCommand{Stable<UiActionId>(8), {}}).HasValue());
    RetainedAnimationHandler handler;
    auto dispatched = router.DispatchNext(handler);
    REQUIRE(dispatched.HasValue());
    REQUIRE(dispatched.Value().has_value());
    REQUIRE(handler.producer.has_value());
    CHECK(handler.actual.source == source);
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto stable = Frame(participant);
    CHECK(stable.Controls().front().source == source);
    CHECK(router.Owner() == source.owner);
    CHECK(participant.InputEligible(Receipt(initial, 1).view));
    REQUIRE(participant.Start(Stable<UiAnimationId>(1)).HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    const auto failed = fixture.host->RunFrame();
    REQUIRE(failed.HasError());
    // RuntimeHost's real fatal policy shuts down; the last immutable publication is nevertheless unchanged and readable.
    CHECK(stable.Styles().Descriptor().geometry == initial.Styles().Descriptor().geometry);
    CHECK(stable.Layout().Descriptor().interaction == initial.Layout().Descriptor().interaction);
    CHECK(handler.producer->Cancellation().IsCancellationRequested());
    CHECK(participant.Acquire().HasError());
    handler.producer.reset();
}

TEST_CASE("Actual frame and route publication use preallocated owners without C++ allocation or reclamation",
          "[runtime_ui][animation][integration][allocation][routes]") {
    HostFixture fixture{{.routeMotion = true}};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    REQUIRE(participant.Navigate(UiRouteOperationRequest::Push(Stable<UiRouteId>(9))).HasValue());
    const auto allocations = Horo::Tests::AllocationProbe::Count();
    const auto frees = Horo::Tests::AllocationProbe::FreeCount();
    const auto prepared = fixture.host->RunFrame();
    const auto afterPrepare = Horo::Tests::AllocationProbe::Count();
    const auto afterPrepareFree = Horo::Tests::AllocationProbe::FreeCount();
    REQUIRE(prepared.HasValue());
    CHECK(afterPrepare == allocations);
    CHECK(afterPrepareFree == frees);
    fixture.clock.Advance(Duration::FromMilliseconds(100));
    const auto completionAllocations = Horo::Tests::AllocationProbe::Count();
    const auto completionFrees = Horo::Tests::AllocationProbe::FreeCount();
    const auto completed = fixture.host->RunFrame();
    const auto afterCompletion = Horo::Tests::AllocationProbe::Count();
    const auto afterCompletionFree = Horo::Tests::AllocationProbe::FreeCount();
    REQUIRE(completed.HasValue());
    CHECK(afterCompletion == completionAllocations);
    CHECK(afterCompletionFree == completionFrees);
    const auto frame = Frame(participant);
    REQUIRE(frame.RouteOperation()->terminal.has_value());
    CHECK(frame.RouteOperation()->phase == UiAnimationRoutePhase::Completed);
}

TEST_CASE("Actual admitted pending writes survive stable frames and refuse an animated source replacement without provider callbacks",
          "[runtime_ui][animation][integration][write][rollback]") {
    HostFixture fixture{{.actions = true, .writes = true}};
    auto &participant = *fixture.participant;
    auto &attachment = fixture.attachedWrites;
    REQUIRE(attachment.store);
    REQUIRE(attachment.tree);
    REQUIRE(fixture.attachedActions);
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto initial = Frame(participant);
    const auto source = initial.Controls().front().source;
    REQUIRE(participant.ApplyPresentation(Receipt(initial, 1)).HasValue());
    auto edit = attachment.store->BeginEdit(*attachment.tree, Stable<UiBindingId>(11), source);
    REQUIRE(edit.HasValue());
    UiActionPayload payload;
    REQUIRE(payload.Add(Text("draft")).HasValue());
    REQUIRE(fixture.attachedActions->Enqueue(source, UiGameplayActionCommand{Stable<UiActionId>(8), payload}).HasValue());
    auto routed = fixture.attachedActions->TryDequeue();
    REQUIRE(routed.HasValue());
    REQUIRE(routed.Value().has_value());
    auto queued = attachment.store->QueueWrite(*attachment.tree, edit.Value(), *routed.Value(), UiBindingCommitTrigger::Submit);
    REQUIRE(queued.HasValue());
    REQUIRE(attachment.state->prepares == 0);
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto stable = Frame(participant);
    REQUIRE(stable.Controls().front().source == source);
    CHECK(attachment.state->prepares == 0);
    CHECK(attachment.state->commits == 0);
    CHECK(attachment.state->abandons == 0);
    REQUIRE(participant.Start(Stable<UiAnimationId>(1)).HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasError());
    CHECK(stable.Layout().Descriptor().interaction == initial.Layout().Descriptor().interaction);
    CHECK(stable.Styles().Descriptor().geometry == initial.Styles().Descriptor().geometry);
    CHECK(attachment.state->prepares == 0);
    CHECK(attachment.state->commits == 0);
    CHECK_FALSE(participant.InputEligible(Receipt(stable, 2).view));
}
