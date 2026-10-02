#include "Horo/Foundation/JobSystem.h"
#include "Horo/Runtime/Ui/UiAsyncActions.h"
#include "Horo/Runtime/Ui/UiControls.h"
#include "Horo/Runtime/Ui/UiErrors.h"
#include "Horo/Runtime/Ui/UiScreenStack.h"
#include "support/AllocationProbe.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <utility>

namespace Horo::Runtime::Ui {
    namespace {
        template <typename Id> Id AuthoredId(const std::uint8_t marker = 1) {
            SerializedUiId bytes{};
            bytes.back() = marker;
            return Id::Create(bytes).Value();
        }

        UiActionOwnerContext Context(const std::uint64_t revision = 1) {
            const auto owner = UiOwnershipGeneration::Create(752).Value();
            return {{owner, 1, 1},
                    {owner, 2, 1},
                    AuthoredId<UiDocumentId>(),
                    UiDocumentRevision::Create(revision).Value(),
                    UiRuntimeTreeRevision::Create(revision).Value(),
                    UiInteractionRevision::Create(revision).Value()};
        }

        UiActionSource Source(const UiActionOwnerContext context = Context(), const std::uint32_t slot = 3) {
            return {context, {context.instance.ownership, slot, 1}};
        }

        UiActionRequest Request(const std::uint64_t sequence = 1, const UiActionSource source = Source()) {
            return {{source.owner.instance.ownership, UiActionSequence::Create(sequence).Value()},
                    source,
                    UiActionOrigin::Button,
                    UiButtonActionCommand{AuthoredId<UiActionId>(), {}}};
        }

        UiActionRouter Router(const std::uint32_t capacity = 2, const UiActionOwnerContext context = Context()) {
            return std::move(UiActionRouter::Create({context, capacity})).Value();
        }

        UiAsyncActionStore Store(const std::uint32_t capacity = 2) {
            return std::move(UiAsyncActionStore::Create(Context(), capacity)).Value();
        }

        UiControlStateMachine Control(const bool enabled = true, const UiActionOwnerContext context = Context()) {
            UiControlDescriptorBase base;
            base.owner = context;
            base.element = Source(context).element;
            base.action = AuthoredId<UiActionId>();
            base.initiallyEnabled = enabled;
            return std::move(UiControlStateMachine::Create(UiButtonControlDescriptor{base})).Value();
        }

        UiScreenStack ScreenStack(const std::uint32_t capacity = 2) {
            const std::array definitions{UiRouteMetadata{AuthoredId<UiRouteId>(), UiPresentationBand::Screen, 0, false},
                                         UiRouteMetadata{AuthoredId<UiRouteId>(2), UiPresentationBand::Screen, 0, false}};
            const auto owner = Context().instance.ownership;
            return std::move(UiScreenStack::Create({owner, {owner, 1, 1}, definitions, capacity})).Value();
        }

        template <typename T> void ExpectError(const Result<T> &result, const ErrorCodeDescriptor &error) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == error.code.Value());
        }

        class RetainingHandler final : public UiAsyncActionHandler {
        public:
            Result<void> Start(const UiActionRequest &request, UiAsyncActionProducer pending) override {
                lastRequest = request;
                ++calls;
                producer.emplace(std::move(pending));
                return Result<void>::Success();
            }

            std::optional<UiAsyncActionProducer> producer;
            UiActionRequest lastRequest;
            std::size_t calls{};
        };

        UiAsyncActionKey Dispatch(UiActionRouter &router, RetainingHandler &handler) {
            REQUIRE(router.Enqueue(Source(router.Owner()), UiButtonActionCommand{AuthoredId<UiActionId>(), {}}).HasValue());
            auto dispatched = router.DispatchNext(handler);
            REQUIRE(dispatched.HasValue());
            REQUIRE(dispatched.Value());
            CHECK(dispatched.Value()->kind == UiActionResultKind::Pending);
            REQUIRE(handler.producer);
            CHECK(dispatched.Value()->operation == handler.producer->Key().operation);
            return handler.producer->Key();
        }

        TEST_CASE("Async actions project progress busy state and terminal payload through the production control",
                  "[runtime_ui][async_actions]") {
            auto router = Router();
            auto control = Control();
            RetainingHandler handler;
            const auto key = Dispatch(router, handler);
            REQUIRE(handler.producer->PublishProgress({1, 250, true}).HasValue());
            REQUIRE(control.ObserveAsyncActions(*router.AsyncActions()).HasValue());
            CHECK(std::get<UiButtonControlState>(control.Snapshot().Value()).availability == UiControlAvailability::Busy);
            CHECK(control.AsyncAction().Value()->progress == UiAsyncActionProgress{1, 250, true});
            const auto input = control.Handle({Source(), UiControlInputKind::PointerPress, UiControlActivationSource::Pointer, 1});
            REQUIRE(input.HasValue());
            CHECK(input.Value().transition == UiControlTransitionKind::IgnoredDisabled);
            CHECK_FALSE(control.ApplyDefault().Value());

            UiActionPayload payload;
            REQUIRE(payload.Add(std::uint64_t{42}).HasValue());
            REQUIRE(handler.producer->Complete(payload).HasValue());
            REQUIRE(control.ObserveAsyncActions(*router.AsyncActions()).HasValue());
            CHECK(std::get<UiButtonControlState>(control.Snapshot().Value()).availability == UiControlAvailability::Enabled);
            CHECK(control.AsyncAction().Value()->state == UiAsyncActionState::Completed);
            CHECK(std::get<std::uint64_t>(control.AsyncAction().Value()->payload.Values()[0]) == 42);
            REQUIRE(router.AsyncActions()->Cancel(key, UiActionCancellationReason::Requested).HasValue());
            CHECK(router.AsyncActions()->Snapshot(key).Value().state == UiAsyncActionState::Completed);
            ExpectError(handler.producer->Complete(), UiErrors::AsyncActionAlreadyTerminal);
            ExpectError(handler.producer->PublishProgress({1, 1000, true}), UiErrors::AsyncActionAlreadyTerminal);
        }

        TEST_CASE("Async completion never re-enables a separately disabled control", "[runtime_ui][async_actions]") {
            auto router = Router();
            auto control = Control();
            RetainingHandler handler;
            Dispatch(router, handler);
            REQUIRE(control.ObserveAsyncActions(*router.AsyncActions()).HasValue());
            REQUIRE(control.SetAvailability(UiControlAvailability::Disabled).HasValue());
            REQUIRE(handler.producer->Complete().HasValue());
            REQUIRE(control.ObserveAsyncActions(*router.AsyncActions()).HasValue());
            CHECK(std::get<UiButtonControlState>(control.Snapshot().Value()).availability == UiControlAvailability::Disabled);
            REQUIRE(control.SetAvailability(UiControlAvailability::Enabled).HasValue());
            CHECK(std::get<UiButtonControlState>(control.Snapshot().Value()).availability == UiControlAvailability::Enabled);
            ExpectError(control.SetAvailability(UiControlAvailability::Busy), UiErrors::ControlInputInvalid);
        }

        TEST_CASE("Async progress validates phase resets bounds and monotonicity atomically", "[runtime_ui][async_actions]") {
            auto store = Store();
            auto producer = std::move(store.Start(Request())).Value();
            REQUIRE(producer.PublishProgress({1, 700, true}).HasValue());
            for (const auto progress :
                 std::array{UiAsyncActionProgress{1, 699, true}, {0, 900, true}, {1, 0, false}, {2, 1001, true}, {2, 1, false}}) {
                ExpectError(producer.PublishProgress(progress), UiErrors::AsyncActionProgressInvalid);
                CHECK(store.Snapshot(producer.Key()).Value().progress == UiAsyncActionProgress{1, 700, true});
            }
            REQUIRE(producer.PublishProgress({2, 0, false}).HasValue());
            REQUIRE(producer.PublishProgress({2, 500, true}).HasValue());
            REQUIRE(producer.PublishProgress({2, 500, true}).HasValue());
        }

        TEST_CASE("Async failures retain the original typed immutable cause and bounded diagnostics", "[runtime_ui][async_actions]") {
            auto router = Router();
            auto control = Control();
            RetainingHandler handler;
            const auto key = Dispatch(router, handler);
            const auto error = std::make_shared<const Error>(WrapError(UiErrors::ActionHandlerFailed, MakeError(UiErrors::PayloadInvalid)));
            ExpectError(handler.producer->Fail({}), UiErrors::AsyncActionFailureInvalid);
            auto oversized = MakeError(UiErrors::ActionHandlerFailed, std::string(4097, 'x'));
            ExpectError(handler.producer->Fail(std::make_shared<const Error>(std::move(oversized))), UiErrors::AsyncActionFailureInvalid);
            REQUIRE(handler.producer->Fail(error).HasValue());
            REQUIRE(control.ObserveAsyncActions(*router.AsyncActions()).HasValue());
            CHECK(control.AsyncAction().Value()->error == error);
            CHECK(control.AsyncAction().Value()->state == UiAsyncActionState::Failed);
            CHECK(
                ErrorChainContains(*control.AsyncAction().Value()->error, UiErrors::PayloadInvalid.domain, UiErrors::PayloadInvalid.code));
            REQUIRE(router.AsyncActions()->Cancel(key, UiActionCancellationReason::Shutdown).HasValue());
            CHECK(router.AsyncActions()->Snapshot(key).Value().state == UiAsyncActionState::Failed);
            ExpectError(handler.producer->Complete(), UiErrors::AsyncActionAlreadyTerminal);
        }

        TEST_CASE("Async cancellation is terminal once and pinned tokens prevent slot reuse", "[runtime_ui][async_actions]") {
            auto store = Store(1);
            auto producer = std::move(store.Start(Request())).Value();
            const auto key = producer.Key();
            auto token = producer.Cancellation();
            CHECK_FALSE(token.IsCancellationRequested());
            ExpectError(store.Release(key), UiErrors::AsyncActionBusy);
            REQUIRE(store.Cancel(key, UiActionCancellationReason::Reload).HasValue());
            REQUIRE(store.Cancel(key, UiActionCancellationReason::Shutdown).HasValue());
            CHECK(store.Snapshot(key).Value().cancellation == UiActionCancellationReason::Reload);
            CHECK(token.IsCancellationRequested());
            REQUIRE(store.Release(key).HasValue());
            ExpectError(store.Start(Request(2)), UiErrors::AsyncActionCapacityExceeded);
            {
                auto retiredProducer = std::move(producer);
            }
            ExpectError(store.Start(Request(2)), UiErrors::AsyncActionCapacityExceeded);
            token = {};
            auto next = store.Start(Request(2));
            REQUIRE(next.HasValue());
            CHECK_FALSE(next.Value().Cancellation().IsCancellationRequested());
            ExpectError(store.Snapshot(key), UiErrors::ActionResultStale);
            ExpectError(store.Cancel(key, UiActionCancellationReason::Requested), UiErrors::ActionResultStale);
        }

        TEST_CASE("Async admission fences exact source request identity and duplicate activation", "[runtime_ui][async_actions]") {
            auto store = Store();
            auto producer = std::move(store.Start(Request())).Value();
            ExpectError(store.Start(Request(2)), UiErrors::AsyncActionBusy);
            ExpectError(store.Start(Request(1, Source(Context(), 4))), UiErrors::ActionSourceStale);
            ExpectError(store.Start(Request(2, Source(Context(2)))), UiErrors::ActionSourceStale);
            auto foreignKey = producer.Key();
            foreignKey.source.owner.interaction = UiInteractionRevision::Create(2).Value();
            ExpectError(store.Snapshot(foreignKey), UiErrors::ActionResultStale);
            REQUIRE(producer.Complete().HasValue());
            auto second = store.Start(Request(2));
            REQUIRE(second.HasValue());
            CHECK(store.Project(Source()).Value()->request.sequence.Value() == 2);
            CHECK(store.Project(Source(Context(), 4)).Value() == std::nullopt);
        }

        TEST_CASE("Async queue backpressure retains the head until operation capacity retires", "[runtime_ui][async_actions]") {
            auto router = Router(1);
            RetainingHandler handler;
            const auto key = Dispatch(router, handler);
            REQUIRE(router.Enqueue(Source(), UiButtonActionCommand{AuthoredId<UiActionId>(), {}}).HasValue());
            ExpectError(router.DispatchNext(handler), UiErrors::AsyncActionBusy);
            CHECK(router.QueuedCount() == 1);
            REQUIRE(handler.producer->Complete().HasValue());
            ExpectError(router.DispatchNext(handler), UiErrors::AsyncActionCapacityExceeded);
            REQUIRE(router.AsyncActions()->Release(key).HasValue());
            handler.producer.reset();
            REQUIRE(router.DispatchNext(handler).HasValue());
            CHECK(router.QueuedCount() == 0);
            CHECK(handler.calls == 2);
        }

        TEST_CASE("Completion leases survive tree reload router destruction and retired control", "[runtime_ui][async_actions]") {
            RetainingHandler handler;
            auto control = Control();
            UiAsyncActionCancellation token;
            {
                auto router = Router();
                Dispatch(router, handler);
                token = handler.producer->Cancellation();
                REQUIRE(control.ObserveAsyncActions(*router.AsyncActions()).HasValue());
                REQUIRE(router.BeginRetirement(UiActionCancellationReason::Reload).HasValue());
                REQUIRE(control.BeginRetirement().HasValue());
                ExpectError(control.ObserveAsyncActions(*router.AsyncActions()), UiErrors::ControlLifecycleUnavailable);
                router.Shutdown();
                router.Shutdown();
            }
            CHECK(token.IsCancellationRequested());
            ExpectError(handler.producer->Complete(), UiErrors::AsyncActionAlreadyTerminal);
            auto replacement = Router(2, Context(2));
            auto nextControl = Control(true, Context(2));
            REQUIRE(nextControl.ObserveAsyncActions(*replacement.AsyncActions()).HasValue());
            CHECK_FALSE(nextControl.AsyncAction().Value());
            ExpectError(replacement.AsyncActions()->Snapshot(handler.producer->Key()), UiErrors::ActionResultStale);
        }

        TEST_CASE("Screen close back clear replacement and shutdown cancel attached operations",
                  "[runtime_ui][async_actions][screen_stack]") {
            const auto kind = GENERATE(UiRouteOperationKind::Pop, UiRouteOperationKind::Back, UiRouteOperationKind::Clear,
                                       UiRouteOperationKind::ReplaceTop, UiRouteOperationKind::Count);
            auto stack = ScreenStack();
            REQUIRE(stack.Push(AuthoredId<UiRouteId>()).Value().IsCommitted());
            const auto route = stack.Top()->id;
            auto router = Router();
            REQUIRE(stack.AttachActions(route, std::move(router)).HasValue());
            RetainingHandler handler;
            Dispatch(*stack.Actions(route), handler);
            auto token = handler.producer->Cancellation();
            if (kind == UiRouteOperationKind::Count) {
                stack.Shutdown();
                stack.Shutdown();
            } else {
                const auto request = kind == UiRouteOperationKind::ReplaceTop ? UiRouteOperationRequest::Replace(AuthoredId<UiRouteId>(2))
                                     : kind == UiRouteOperationKind::Clear    ? UiRouteOperationRequest::Clear()
                                     : kind == UiRouteOperationKind::Back     ? UiRouteOperationRequest::Back()
                                                                              : UiRouteOperationRequest::Pop();
                REQUIRE(stack.Navigate(request).Value().IsCommitted());
            }
            CHECK(stack.Actions(route) == nullptr);
            CHECK(token.IsCancellationRequested());
            ExpectError(handler.producer->Complete(), UiErrors::AsyncActionAlreadyTerminal);
        }

        TEST_CASE("Rejected and cancelled screen mutations preserve pending action ownership",
                  "[runtime_ui][async_actions][screen_stack]") {
            auto stack = ScreenStack(1);
            REQUIRE(stack.Push(AuthoredId<UiRouteId>()).Value().IsCommitted());
            const auto route = stack.Top()->id;
            auto router = Router();
            REQUIRE(stack.AttachActions(route, std::move(router)).HasValue());
            RetainingHandler handler;
            const auto key = Dispatch(*stack.Actions(route), handler);
            auto token = handler.producer->Cancellation();
            REQUIRE(stack.Push(AuthoredId<UiRouteId>(2)).HasValue());
            CHECK_FALSE(token.IsCancellationRequested());
            auto prepared = stack.Prepare(UiRouteOperationRequest::Pop());
            REQUIRE(prepared.HasValue());
            REQUIRE(prepared.Value().Cancel().HasValue());
            CHECK_FALSE(token.IsCancellationRequested());
            CHECK(stack.Actions(route)->AsyncActions()->Snapshot(key).Value().Busy());
            REQUIRE(handler.producer->Complete().HasValue());
        }

        TEST_CASE("Async self-closing route scheduler cannot return into freed dispatch state",
                  "[runtime_ui][async_actions][screen_stack]") {
            class ClosingHandler final : public UiAsyncActionHandler {
            public:
                explicit ClosingHandler(UiScreenStack &stack) : stack_(stack) {}

                Result<void> Start(const UiActionRequest &, UiAsyncActionProducer producer) override {
                    token = producer.Cancellation();
                    REQUIRE(stack_.Pop().Value().IsCommitted());
                    CHECK(token.IsCancellationRequested());
                    ExpectError(producer.Complete(), UiErrors::AsyncActionAlreadyTerminal);
                    return Result<void>::Success();
                }

                UiAsyncActionCancellation token;

            private:
                UiScreenStack &stack_;
            };

            auto stack = ScreenStack();
            REQUIRE(stack.Push(AuthoredId<UiRouteId>()).Value().IsCommitted());
            const auto route = stack.Top()->id;
            auto router = Router();
            REQUIRE(stack.AttachActions(route, std::move(router)).HasValue());
            auto *actions = stack.Actions(route);
            REQUIRE(actions->Enqueue(Source(), UiButtonActionCommand{AuthoredId<UiActionId>(), {}}).HasValue());
            ClosingHandler handler{stack};
            ExpectError(actions->DispatchNext(handler), UiErrors::ActionLifecycleUnavailable);
            CHECK(stack.Empty());
        }

        TEST_CASE("Async abandoned and moved leases cancel only their prior operation", "[runtime_ui][async_actions]") {
            auto store = Store();
            auto first = std::move(store.Start(Request())).Value();
            const auto oldKey = first.Key();
            auto second = std::move(store.Start(Request(2, Source(Context(), 4)))).Value();
            const auto nextKey = second.Key();
            first = std::move(second);
            CHECK(store.Snapshot(oldKey).Value().cancellation == UiActionCancellationReason::Requested);
            CHECK(store.Snapshot(nextKey).Value().Busy());
            {
                auto moved = std::move(first);
            }
            CHECK(store.Snapshot(nextKey).Value().state == UiAsyncActionState::Cancelled);
        }

        TEST_CASE("Async successful frame operations use preallocated storage without allocation",
                  "[runtime_ui][async_actions][allocation]") {
            auto router = Router();
            auto control = Control();
            RetainingHandler handler;
            const auto command = UiButtonActionCommand{AuthoredId<UiActionId>(), {}};
            const auto before = Horo::Tests::AllocationProbe::Count();
            const auto enqueued = router.Enqueue(Source(), command);
            const auto dispatched = router.DispatchNext(handler);
            const auto progress = handler.producer->PublishProgress({1, 900, true});
            const auto observed = control.ObserveAsyncActions(*router.AsyncActions());
            const auto projected = control.AsyncAction();
            const auto cancelled = router.AsyncActions()->Cancel(handler.producer->Key(), UiActionCancellationReason::Requested);
            const auto terminal = router.AsyncActions()->Snapshot(handler.producer->Key());
            const auto after = Horo::Tests::AllocationProbe::Count();
            REQUIRE(enqueued.HasValue());
            REQUIRE(dispatched.HasValue());
            REQUIRE(progress.HasValue());
            REQUIRE(observed.HasValue());
            REQUIRE(projected.HasValue());
            REQUIRE(cancelled.HasValue());
            REQUIRE(terminal.HasValue());
            CHECK(after == before);
        }

        TEST_CASE("Provider-owned structured jobs observe cancellation after their screen retires", "[runtime_ui][async_actions][jobs]") {
            Horo::JobSystem jobs{Horo::JobSystemConfig{.workerCount = 0, .maxQueuedJobs = 1}};
            Horo::TaskGroup group{jobs};
            RetainingHandler handler;
            {
                auto router = Router();
                Dispatch(router, handler);
                const auto cancellation = handler.producer->Cancellation();
                REQUIRE(group
                            .Spawn({}, [cancellation](const Horo::CancellationToken &) {
                    return cancellation.IsCancellationRequested() ? Horo::JobCancelled() : Result<void>::Success();
                }).HasValue());
                router.Shutdown();
            }
            // Explicit bounded test drain executes the queued job; ordinary owner frames never wait.
            REQUIRE(group.Join({Horo::WaitPolicy::MainThreadPumpAllowed, std::chrono::seconds(1)}).HasError());
            CHECK(group.Outcome() == Horo::TaskGroupOutcome::Cancelled);
            ExpectError(handler.producer->Complete(), UiErrors::AsyncActionAlreadyTerminal);
            jobs.Shutdown(Horo::ShutdownPolicy::Drain);
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
