#include "UiAnimationOwnerIntegrationFixture.h"

#include <catch2/generators/catch_generators.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Runtime;
    using namespace Horo::Runtime::Ui;
    using namespace Horo::Runtime::Ui::ReloadTests;
    using namespace Horo::Runtime::Ui::AnimationTests;

    /** @brief Dispatch through the actual attached router against one successfully presented host publication. */
    UiAsyncActionKey StartHostAction(HostFixture &fixture, UiActionRouter &router, RetainedAnimationHandler &handler) {
        REQUIRE(fixture.host->RunFrame().HasValue());
        const auto initial = Frame(*fixture.participant);
        const auto source = initial.Controls().front().source;
        REQUIRE(fixture.participant->ApplyPresentation(Receipt(initial, 1)).HasValue());
        REQUIRE(router.Enqueue(source, UiButtonActionCommand{Stable<UiActionId>(8), {}}).HasValue());
        REQUIRE(router.DispatchNext(handler).HasValue());
        REQUIRE(handler.producer);
        return handler.producer->Key();
    }

    /** @brief Current receipt advances input authority without changing a previously issued operation key. */
    void CheckAsyncAdmission(UiAnimationRuntimeParticipant &participant, UiActionRouter &router, const UiAnimationFrameLease &animated,
                             const UiAsyncActionKey &key) {
        const auto current = animated.Controls().front().source;
        CHECK(current.owner.interaction != key.source.owner.interaction);
        CHECK(router.Owner() == current.owner);
        CHECK(router.AsyncActions()->Snapshot(key).HasValue());
        REQUIRE(participant.ApplyPresentation(Receipt(animated, 2)).HasValue());
        CHECK(participant.HandleControl(Receipt(animated, 2).view, Edge(key.source, UiControlInputKind::SubmitPress, 1)).HasError());
        CHECK(router.Enqueue(key.source, UiButtonActionCommand{Stable<UiActionId>(8), {}}).HasError());
    }

    /** @brief Measure only real owner frame work, outside test assertion/reporting allocations. */
    void RunAsyncFrameWithoutAllocation(HostFixture &fixture) {
        const auto allocations = Horo::Tests::AllocationProbe::Count();
        const auto frees = Horo::Tests::AllocationProbe::FreeCount();
        const auto result = fixture.host->RunFrame();
        const auto afterAllocations = Horo::Tests::AllocationProbe::Count();
        const auto afterFrees = Horo::Tests::AllocationProbe::FreeCount();
        REQUIRE(result.HasValue());
        CHECK(afterAllocations == allocations);
        CHECK(afterFrees == frees);
    }

    /** @brief Real owner-phase handoff; never runs on a worker or mutates a control directly. */
    class AsyncOwnerHandoff final : public RuntimeLifecycleParticipant {
    public:
        explicit AsyncOwnerHandoff(RetainedAnimationHandler &handler) : handler(handler) {}

        Result<void> Startup(const CancellationToken &) override {
            return Result<void>::Success();
        }

        Result<void> OnPhase(const RuntimePhase phase, const FrameContext &) override {
            if (!armed || phase != cutoff)
                return Result<void>::Success();
            armed = false;
            if (retiring) {
                retiring->Retire(UiActionCancellationReason::Reload);
                return Result<void>::Success();
            }
            return complete ? handler.producer->Complete() : handler.producer->PublishProgress({1, 700, true});
        }

        Result<void> OnFixedUpdate(const FixedStepContext &) override {
            return Result<void>::Success();
        }

        void Shutdown() noexcept override {}

        RetainedAnimationHandler &handler;
        RuntimePhase cutoff{RuntimePhase::ApplyQueuedOwnerThreadCommands};
        bool armed{};
        bool complete{};
        UiAsyncActionStore *retiring{};
    };
}  // namespace

TEST_CASE("Real host projects async busy progress and terminal state across animated interaction generations",
          "[runtime_ui][animation][integration][async]") {
    const bool routed = GENERATE(false, true);
    HostOptions options{.routeMotion = routed, .actions = !routed, .busyStyle = true, .routeActions = routed};
    HostFixture fixture{options};
    auto &participant = *fixture.participant;
    auto &router = *(routed ? fixture.attachedRouteActions : fixture.attachedActions);
    RetainedAnimationHandler handler;
    const auto key = StartHostAction(fixture, router, handler);
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto busy = Frame(participant);
    CHECK(std::get<UiTextInputControlState>(busy.Controls().front().state).availability == UiControlAvailability::Busy);
    REQUIRE(busy.Controls().front().asyncAction);
    CHECK(busy.Controls().front().asyncAction->source == key.source);
    CheckGeometry(busy, 140);
    REQUIRE(participant.Start(Stable<UiAnimationId>(1)).HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto animated = Frame(participant);
    CheckAsyncAdmission(participant, router, animated, key);
    CHECK(handler.producer->Key() == key);
    REQUIRE(handler.producer->PublishProgress({1, 700, true}).HasValue());
    busy = {};
    fixture.clock.Advance(Duration::FromMilliseconds(10));
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto progress = Frame(participant);
    REQUIRE(progress.Controls().front().asyncAction);
    CHECK(progress.Controls().front().asyncAction->progress == UiAsyncActionProgress{1, 700, true});
    CHECK(std::get<UiTextInputControlState>(progress.Controls().front().state).availability == UiControlAvailability::Busy);
    animated = {};
    REQUIRE(handler.producer->Complete().HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto terminal = Frame(participant);
    REQUIRE(terminal.Controls().front().asyncAction);
    CHECK(terminal.Controls().front().asyncAction->state == UiAsyncActionState::Completed);
    CHECK(std::get<UiTextInputControlState>(terminal.Controls().front().state).availability == UiControlAvailability::Enabled);
    CHECK(progress.Controls().front().asyncAction->state == UiAsyncActionState::Pending);
    CHECK_FALSE(handler.producer->Cancellation().IsCancellationRequested());
    fixture.host->Shutdown();
    CHECK(router.AsyncActions()->Snapshot(key).Value().state == UiAsyncActionState::Completed);
}

TEST_CASE("Real owner command handoff projects async progress without warmed Unicode draft allocation",
          "[runtime_ui][animation][integration][async][allocation]") {
    RetainedAnimationHandler handler;
    auto handoff = std::make_unique<AsyncOwnerHandoff>(handler);
    auto *commands = handoff.get();
    HostFixture fixture{{.actions = true}, std::move(handoff)};
    auto &participant = *fixture.participant;
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto frame = Frame(participant);
    auto source = frame.Controls().front().source;
    REQUIRE(participant.ApplyPresentation(Receipt(frame, 1)).HasValue());
    REQUIRE(participant.HandleControl(Receipt(frame, 1).view, Edge(source, UiControlInputKind::FocusGained, 1)).HasValue());
    REQUIRE(
        participant.EditControlText(Receipt(frame, 1).view, source, 2, {UiTextEditKind::Paste, Text("Türkçe 👩‍💻")}).HasValue());
    frame = {};
    REQUIRE(fixture.host->RunFrame().HasValue());
    frame = Frame(participant);
    CHECK(std::get<UiTextInputControlState>(frame.Controls().front().state).text.View() == "baseTürkçe 👩‍💻");
    source = frame.Controls().front().source;
    REQUIRE(fixture.attachedActions->Enqueue(source, UiButtonActionCommand{Stable<UiActionId>(8), {}}).HasValue());
    REQUIRE(fixture.attachedActions->DispatchNext(handler).HasValue());
    frame = {};
    RunAsyncFrameWithoutAllocation(fixture);
    commands->armed = true;
    RunAsyncFrameWithoutAllocation(fixture);
    frame = Frame(participant);
    REQUIRE(frame.Controls().front().asyncAction);
    CHECK(frame.Controls().front().asyncAction->progress == UiAsyncActionProgress{1, 700, true});
    CHECK(std::get<UiTextInputControlState>(frame.Controls().front().state).text.View() == "baseTürkçe 👩‍💻");
}

TEST_CASE("Animated source admission preserves issued operation keys and duplicate backpressure",
          "[runtime_ui][animation][integration][async][identity]") {
    HostFixture fixture{{.actions = true, .busyStyle = true}};
    RetainedAnimationHandler handler;
    auto &router = *fixture.attachedActions;
    const auto key = StartHostAction(fixture, router, handler);
    REQUIRE(fixture.host->RunFrame().HasValue());
    const auto frame = Frame(*fixture.participant);
    const auto source = frame.Controls().front().source;
    REQUIRE(source.owner != key.source.owner);
    CHECK(router.AsyncActions()->Project(key.source).HasError());
    REQUIRE(router.AsyncActions()->Project(source).HasValue());
    auto forged = key;
    forged.source = source;
    CHECK(router.AsyncActions()->Snapshot(forged).HasError());
    REQUIRE(router.Enqueue(source, UiButtonActionCommand{Stable<UiActionId>(8), {}}).HasValue());
    const auto duplicate = router.DispatchNext(handler);
    REQUIRE(duplicate.HasError());
    CHECK(duplicate.ErrorValue().code.Value() == UiErrors::AsyncActionBusy.code.Value());
    CHECK(router.QueuedCount() == 1);
    REQUIRE(router.TryDequeue().HasValue());
    CHECK(handler.producer->Key() == key);
    CHECK(router.AsyncActions()->Snapshot(key).Value().state == UiAsyncActionState::Pending);
}

TEST_CASE("Async copied failure storage is reclaimed only at actual owner quiescence",
          "[runtime_ui][animation][integration][async][allocation][errors]") {
    HostFixture fixture{{.actions = true}};
    RetainedAnimationHandler handler;
    auto &router = *fixture.attachedActions;
    const auto key = StartHostAction(fixture, router, handler);
    RunAsyncFrameWithoutAllocation(fixture);
    auto error = std::make_shared<const Error>(WrapError(UiErrors::ActionHandlerFailed, MakeError(UiErrors::PayloadInvalid)));
    std::weak_ptr<const Error> observed = error;
    REQUIRE(handler.producer->Fail(error).HasValue());
    RunAsyncFrameWithoutAllocation(fixture);
    auto failed = Frame(*fixture.participant);
    REQUIRE(failed.Controls().front().asyncAction);
    CHECK(failed.Controls().front().asyncAction->error == error);
    CHECK(std::get<UiTextInputControlState>(failed.Controls().front().state).availability == UiControlAvailability::Enabled);
    REQUIRE(router.AsyncActions()->Release(key).HasValue());
    handler.producer.reset();
    error.reset();
    REQUIRE(fixture.participant->DrainRetired().HasValue());
    CHECK_FALSE(observed.expired());
    for (int index = 0; index < 3; ++index)
        RunAsyncFrameWithoutAllocation(fixture);
    failed = {};
    for (int index = 0; index < 3; ++index)
        RunAsyncFrameWithoutAllocation(fixture);
    CHECK_FALSE(observed.expired());
    REQUIRE(fixture.participant->DrainRetired().HasValue());
    CHECK(observed.expired());
}

TEST_CASE("Late async progress and completion publish the prepared cutoff then become visible next frame",
          "[runtime_ui][animation][integration][async][cutoff]") {
    const bool finish = GENERATE(false, true);
    RetainedAnimationHandler handler;
    auto handoff = std::make_unique<AsyncOwnerHandoff>(handler);
    auto *commands = handoff.get();
    commands->cutoff = RuntimePhase::VariableUpdate;
    commands->complete = finish;
    HostFixture fixture{{.actions = true}, std::move(handoff)};
    const auto key = StartHostAction(fixture, *fixture.attachedActions, handler);
    REQUIRE(fixture.host->RunFrame().HasValue());
    const auto old = Frame(*fixture.participant);
    REQUIRE(old.Controls().front().asyncAction);
    commands->armed = true;
    REQUIRE(fixture.host->RunFrame().HasValue());
    CHECK(fixture.host->State() == RuntimeLifecycleState::Running);
    const auto cutoff = Frame(*fixture.participant);
    REQUIRE(cutoff.Controls().front().asyncAction);
    CHECK(cutoff.Controls().front().asyncAction->state == UiAsyncActionState::Pending);
    CHECK(cutoff.Controls().front().asyncAction->progress == UiAsyncActionProgress{});
    REQUIRE(fixture.host->RunFrame().HasValue());
    CHECK(fixture.host->State() == RuntimeLifecycleState::Running);
    const auto next = Frame(*fixture.participant);
    REQUIRE(next.Controls().front().asyncAction);
    CHECK(next.Controls().front().asyncAction->state == (finish ? UiAsyncActionState::Completed : UiAsyncActionState::Pending));
    if (!finish)
        CHECK(next.Controls().front().asyncAction->progress == UiAsyncActionProgress{1, 700, true});
    CHECK(std::get<UiTextInputControlState>(next.Controls().front().state).availability ==
          (finish ? UiControlAvailability::Enabled : UiControlAvailability::Busy));
    CHECK(old.Controls().front().asyncAction->state == UiAsyncActionState::Pending);
    CHECK(handler.producer->Key() == key);
    CHECK_FALSE(handler.producer->Cancellation().IsCancellationRequested());
}

TEST_CASE("Actual async store retirement invalidates a prepared observation without rewriting prior frames",
          "[runtime_ui][animation][integration][async][lifecycle]") {
    RetainedAnimationHandler handler;
    auto handoff = std::make_unique<AsyncOwnerHandoff>(handler);
    auto *commands = handoff.get();
    commands->cutoff = RuntimePhase::VariableUpdate;
    HostFixture fixture{{.actions = true}, std::move(handoff)};
    StartHostAction(fixture, *fixture.attachedActions, handler);
    REQUIRE(fixture.host->RunFrame().HasValue());
    const auto old = Frame(*fixture.participant);
    commands->retiring = fixture.attachedActions->AsyncActions();
    commands->armed = true;
    REQUIRE(fixture.host->RunFrame().HasError());
    CHECK(old.Controls().front().asyncAction->state == UiAsyncActionState::Pending);
    CHECK(handler.producer->Cancellation().IsCancellationRequested());
    CHECK(handler.producer->Complete().HasError());
}

TEST_CASE("Retained async route operation cancels on actual animated close and rejects late completion",
          "[runtime_ui][animation][integration][async][routes]") {
    HostFixture fixture{{.routeMotion = true, .routeActions = true}};
    RetainedAnimationHandler handler;
    StartHostAction(fixture, *fixture.attachedRouteActions, handler);
    REQUIRE(fixture.host->RunFrame().HasValue());
    REQUIRE(fixture.participant->Navigate(UiRouteOperationRequest::Pop()).HasValue());
    REQUIRE(fixture.host->RunFrame().HasValue());
    CHECK_FALSE(handler.producer->Cancellation().IsCancellationRequested());
    fixture.clock.Advance(Duration::FromMilliseconds(100));
    REQUIRE(fixture.host->RunFrame().HasValue());
    CHECK(Frame(*fixture.participant).Routes().empty());
    CHECK(handler.producer->Cancellation().IsCancellationRequested());
    CHECK(handler.producer->Complete().HasError());
    REQUIRE(fixture.host->RunFrame().HasValue());
    CHECK_FALSE(Frame(*fixture.participant).Controls().front().asyncAction);
}

TEST_CASE("Actual async reload commits cancellation once while rejected reload preserves operation authority",
          "[runtime_ui][animation][integration][async][reload]") {
    const bool reject = GENERATE(false, true);
    auto issued = UiElementSlotAllocator::Create(ReloadTests::Owner());
    REQUIRE(issued.HasValue());
    auto allocator = std::move(issued).Value();
    HostFixture fixture{{.actions = true, .allocator = &allocator}};
    RetainedAnimationHandler handler;
    const auto key = StartHostAction(fixture, *fixture.attachedActions, handler);
    REQUIRE(fixture.host->RunFrame().HasValue());
    const auto old = Frame(*fixture.participant);
    auto replacement = Generation(allocator, 100);
    auto resources = Resources(replacement.Canvases().front());
    CancellationSource cancelled;
    if (reject)
        cancelled.RequestCancellation();
    auto result = fixture.participant->Reload(std::move(replacement), allocator, std::move(resources.registry), std::move(resources.styles),
                                              std::move(resources.definition),
                                              {UiAnimationReloadPolicy::Cancel, UiStructuralCommitPoint::ApplyQueuedOwnerThreadCommands},
                                              cancelled.Token());
    REQUIRE(old.Controls().front().asyncAction);
    CHECK(old.Controls().front().asyncAction->state == UiAsyncActionState::Pending);
    CHECK(handler.producer->Key() == key);
    if (reject) {
        REQUIRE(result.HasError());
        CHECK_FALSE(handler.producer->Cancellation().IsCancellationRequested());
        CHECK(fixture.attachedActions->AsyncActions()->Snapshot(key).Value().state == UiAsyncActionState::Pending);
        fixture.host->Shutdown();
    } else {
        REQUIRE(result.HasValue());
        REQUIRE(fixture.host->RunFrame().HasValue());
        const auto fresh = Frame(*fixture.participant);
        CHECK_FALSE(fresh.Controls().front().asyncAction);
        REQUIRE(fixture.participant->ApplyPresentation(Receipt(fresh, 2)).HasValue());
        CHECK(fixture.participant->HandleControl(Receipt(fresh, 2).view, Edge(key.source, UiControlInputKind::FocusGained, 1)).HasError());
    }
    CHECK(handler.producer->Cancellation().IsCancellationRequested());
    CHECK(handler.producer->Complete().HasError());
}

TEST_CASE("Real host explicit style origins restore responsive authored layout after busy override",
          "[runtime_ui][animation][integration][layout]") {
    HostFixture fixture{{.actions = true, .busyStyle = true, .explicitLayoutOrigins = true}};
    auto &participant = *fixture.participant;
    RetainedAnimationHandler handler;
    const auto key = StartHostAction(fixture, *fixture.attachedActions, handler);
    const auto check = [&](const UiAnimationFrameLease &frame, const std::int32_t width, const std::int32_t padding) {
        const auto source = frame.Controls().front().source;
        const auto records = frame.Layout().Records();
        const auto child = std::ranges::find(records, source.element, &UiLayoutRecord::element);
        REQUIRE(child != records.end());
        CHECK(child->arrangement.contentBox.extent.width == width);
        CHECK(child->arrangement.contentBox.origin.x - child->arrangement.paddingBox.origin.x == padding);
        CHECK(child->arrangement.contentBox.origin.y - child->arrangement.paddingBox.origin.y == padding);
    };
    auto initial = Frame(participant);
    check(initial, 50, 3);
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto busy = Frame(participant);
    check(busy, 0, 4);  // Explicit visual-state zero remains authoritative.
    REQUIRE(handler.producer->Complete().HasValue());
    initial = {};
    REQUIRE(fixture.host->RunFrame().HasValue());
    auto restored = Frame(participant);
    check(restored, 50, 3);  // The new RegisteredDefault restores this update's authored percentage/padding.
    REQUIRE(restored.Controls().front().asyncAction);
    CHECK(restored.Controls().front().asyncAction->source == key.source);
    CHECK(restored.Controls().front().asyncAction->operation == key.operation);
    fixture.host->Shutdown();
}
