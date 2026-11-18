#include "UiAnimationOwnerIntegrationFixture.h"

#include <catch2/generators/catch_generators.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace Horo::Runtime::Ui;
    using namespace Horo::Runtime::Ui::ReloadTests;
    using namespace Horo::Runtime::Ui::AnimationTests;

}  // namespace

TEST_CASE("Runtime accessibility publishes registry contrast and focused variants on the same immutable frame",
          "[runtime_ui][animation][integration][accessibility]") {
    HostFixture fixture{{.contrastVariants = true, .focusAccess = true}};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto ordinary = Frame(participant);
    REQUIRE(fixture.attachedFocus);
    REQUIRE(fixture.attachedFocus->FocusDefault().HasValue());
    const auto focus = fixture.attachedFocus->CurrentFocus();
    REQUIRE(focus.HasValue());
    REQUIRE(focus.Value());
    const auto focused = focus.Value()->element;
    const auto fill = CoreUiStylePropertyId(UiCoreStyleProperty::Fill);
    auto color = [&](const UiAnimationFrameLease &frame, const UiElementHandle target, const float expected) {
        const auto record = frame.Styles().Get(target);
        REQUIRE(record.HasValue());
        const auto properties = frame.Styles().Properties(record.Value());
        const auto found = std::ranges::find(properties, fill, &UiComputedStyleProperty::property);
        REQUIRE(found != properties.end());
        CHECK(std::get<UiStyleColor>(found->value).red == expected);
        if (expected != 0.3F)
            CHECK(found->provenance.origin == UiStyleOrigin::AccessibilityPolicy);
    };
    color(ordinary, focused, 0.3F);
    REQUIRE(participant.SetVisualAccessibility({20, true, false}).HasValue());
    CHECK(participant.SetVisualAccessibility({19, false, false}).HasError());
    CHECK(participant.SetVisualAccessibility({20, false, false}).HasError());
    color(ordinary, focused, 0.3F);
    const auto beforeContrast = Horo::Tests::AllocationProbe::Count();
    const auto contrasted = fixture.host->RunFrame();
    const auto afterContrast = Horo::Tests::AllocationProbe::Count();
    REQUIRE(contrasted.HasValue());
    CHECK(afterContrast == beforeContrast);
    auto contrast = Frame(participant);
    color(contrast, focused, 1.0F);
    for (const auto &row : contrast.Styles().Records())
        if (row.element != focused)
            color(contrast, row.element, 0.0F);
    CHECK(contrast.Styles().Descriptor().geometry == ordinary.Styles().Descriptor().geometry);
    REQUIRE(participant.SetVisualAccessibility({21, false, false}).HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    color(Frame(participant), focused, 0.3F);
    color(contrast, focused, 1.0F);
    fixture.host->Shutdown();
    CHECK(participant.SetVisualAccessibility({22, true, true}).HasError());
}

TEST_CASE("Runtime reduced motion completes decorative timelines once and preserves essential timing",
          "[runtime_ui][animation][integration][accessibility]") {
    bool decorative{};
    SECTION("explicit decorative motion") {
        decorative = true;
    }
    SECTION("authored essential timing") {}
    HostFixture fixture{{.decorativeMotion = decorative}};
    auto &participant = *fixture.participant;
    const auto timeline = participant.Start(Stable<UiAnimationId>(1));
    REQUIRE(timeline.HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto original = Frame(participant);
    REQUIRE(original.Timelines().size() == 1);
    CHECK(original.Timelines().front().playback.outcome == UiAnimationOutcome::None);
    REQUIRE(participant.SetVisualAccessibility({1, false, true}).HasValue());
    const auto beforeMotionChange = Horo::Tests::AllocationProbe::Count();
    const auto applied = fixture.host->RunFrame();
    const auto afterMotionChange = Horo::Tests::AllocationProbe::Count();
    REQUIRE(applied.HasValue());
    CHECK(afterMotionChange == beforeMotionChange);
    auto next = Frame(participant);
    REQUIRE(next.Timelines().size() == 1);
    CHECK(next.Timelines().front().timeline == timeline.Value());
    CHECK(next.Timelines().front().playback.newTerminalOutcome == decorative);
    CHECK(next.Timelines().front().playback.outcome == (decorative ? UiAnimationOutcome::Completed : UiAnimationOutcome::None));
    CHECK(next.Timelines().front().playback.elapsed == original.Timelines().front().playback.elapsed);
    CheckGeometry(next, decorative ? 50 : 200);
    original = {};
    REQUIRE(participant.SetVisualAccessibility({2, false, false}).HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto repeated = Frame(participant);
    REQUIRE(repeated.Timelines().size() == 1);
    CHECK_FALSE(repeated.Timelines().front().playback.newTerminalOutcome);
    CHECK(repeated.Timelines().front().playback.outcome == next.Timelines().front().playback.outcome);
    CheckGeometry(repeated, decorative ? 50 : 200);
}

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

TEST_CASE("Runtime host direction change publishes new geometry before renewed input admission",
          "[runtime_ui][animation][integration][direction]") {
    HostFixture fixture;
    auto *participant = fixture.participant;
    auto viewport = Config().viewport;
    viewport.constraints.maximum.width = 200;
    viewport.content.extent.width = 200;
    REQUIRE(participant->SetViewport(viewport).HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    const auto left = Frame(*participant);
    const auto before = Receipt(left, 1);
    REQUIRE(participant->ApplyPresentation(before).HasValue());
    REQUIRE(participant->InputEligible(before.view));
    const auto leaf = left.Layout().Records().back().element;
    const auto original = left.Layout().Get(leaf).Value().arrangement.contentBox;
    REQUIRE(original.origin.x == 0);

    viewport.inlineDirection = UiLayoutInlineDirection::RightToLeft;
    REQUIRE(participant->SetViewport(viewport).HasValue());
    fixture.clock.Advance(Duration::FromMilliseconds(16));
    REQUIRE(fixture.host->RunFrame().HasValue());
    const auto right = Frame(*participant);
    CHECK(right.Layout().Descriptor().inlineDirection == UiLayoutInlineDirection::RightToLeft);
    CHECK(right.Layout().Get(leaf).Value().arrangement.contentBox.origin.x == 200 - original.extent.width);
    CHECK(left.Layout().Get(leaf).Value().arrangement.contentBox == original);
    CHECK_FALSE(participant->InputEligible(before.view));
    CHECK(participant->ApplyPresentation(before).HasError());
    const auto presented = Receipt(right, 2);
    REQUIRE(participant->ApplyPresentation(presented).HasValue());
    CHECK(participant->InputEligible(presented.view));
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

TEST_CASE("Presented text commands preserve selection and history across interaction publication", "[runtime_ui][text_edit][integration]") {
    const bool changeFontScale = GENERATE(false, true);
    HostFixture fixture;
    auto &participant = *fixture.participant;
    if (!changeFontScale)
        REQUIRE(participant.Start(Stable<UiAnimationId>(1)).HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto first = Frame(participant);
    const auto source = first.Controls().front().source;
    auto receipt = Receipt(first, 1);
    CHECK(participant.EditControlText(receipt.view, source, 1, {UiTextEditKind::Home}).HasError());
    REQUIRE(participant.ApplyPresentation(receipt).HasValue());
    REQUIRE(participant.HandleControl(receipt.view, Edge(source, UiControlInputKind::FocusGained, 1)).HasValue());
    REQUIRE(participant.EditControlText(receipt.view, source, 2, {UiTextEditKind::Home}).HasValue());
    REQUIRE(participant.EditControlText(receipt.view, source, 3, {UiTextEditKind::Insert, Text("x")}).HasValue());
    auto viewport = Config().viewport;
    if (changeFontScale) {
        viewport.fontScale = {3, 2};
        REQUIRE(participant.SetViewport(viewport).HasValue());
    }
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto current = Frame(participant);
    CHECK(current.Layout().Descriptor().fontScale == viewport.fontScale);
    const auto currentSource = current.Controls().front().source;
    REQUIRE(currentSource.owner != source.owner);
    CHECK(std::get<UiTextInputControlState>(current.Controls().front().state).text.View() == "xbase");
    CHECK(participant.EditControlText(receipt.view, currentSource, 4, {UiTextEditKind::Undo}).HasError());
    receipt = Receipt(current, 2);
    REQUIRE(participant.ApplyPresentation(receipt).HasValue());
    CHECK(participant.EditControlText(receipt.view, source, 4, {UiTextEditKind::Undo}).HasError());
    REQUIRE(participant.EditControlText(receipt.view, currentSource, 4, {UiTextEditKind::Undo}).Value().textChanged);
    const auto copied = participant.EditControlText(receipt.view, currentSource, 5, {UiTextEditKind::SelectAll});
    REQUIRE(copied.HasValue());
    REQUIRE(participant.EditControlText(receipt.view, currentSource, 6, {UiTextEditKind::Copy}).Value().clipboard->View() == "base");
    fixture.host->Shutdown();
    CHECK(participant.EditControlText(receipt.view, currentSource, 7, {UiTextEditKind::Undo}).HasError());
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
