#include "UiAsyncActionsFixture.h"

namespace Horo::Runtime::Ui {
    namespace {
        using namespace AsyncActionTests;

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
                auto request = UiRouteOperationRequest::Pop();
                if (kind == UiRouteOperationKind::ReplaceTop)
                    request = UiRouteOperationRequest::Replace(AuthoredId<UiRouteId>(2));
                else if (kind == UiRouteOperationKind::Clear)
                    request = UiRouteOperationRequest::Clear();
                else if (kind == UiRouteOperationKind::Back)
                    request = UiRouteOperationRequest::Back();
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
            auto transaction = std::move(prepared).Value();
            REQUIRE(transaction.Cancel().HasValue());
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

        TEST_CASE("Reopened actual route routers reject old operation keys despite identical source and request sequences",
                  "[runtime_ui][async_actions][routes][identity]") {
            auto stack = ScreenStack();
            REQUIRE(stack.Push(AuthoredId<UiRouteId>()).HasValue());
            const auto oldRoute = stack.Top()->id;
            REQUIRE(stack.AttachActions(oldRoute, Router()).HasValue());
            RetainingHandler first;
            const auto key = Dispatch(*stack.Actions(oldRoute), first);
            REQUIRE(key.route == oldRoute);
            REQUIRE(stack.Pop().HasValue());
            CHECK(first.producer->Cancellation().IsCancellationRequested());
            REQUIRE(stack.Push(AuthoredId<UiRouteId>()).HasValue());
            const auto newRoute = stack.Top()->id;
            REQUIRE(newRoute != oldRoute);
            REQUIRE(stack.AttachActions(newRoute, Router()).HasValue());
            RetainingHandler second;
            const auto fresh = Dispatch(*stack.Actions(newRoute), second);
            CHECK(fresh.source == key.source);
            CHECK(fresh.request == key.request);
            CHECK(fresh.operation == key.operation);
            CHECK(fresh.route == newRoute);
            CHECK(stack.Actions(newRoute)->AsyncActions()->Snapshot(key).HasError());
            CHECK(stack.Actions(newRoute)->AsyncActions()->Cancel(key, UiActionCancellationReason::Requested).HasError());
            CHECK_FALSE(second.producer->Cancellation().IsCancellationRequested());
        }

        TEST_CASE("Actual route attachment rejects issued unbound routers and rebinding without changing keys",
                  "[runtime_ui][async_actions][routes][identity]") {
            auto stack = ScreenStack();
            REQUIRE(stack.Push(AuthoredId<UiRouteId>()).HasValue());
            const auto firstRoute = stack.Top()->id;
            auto issued = Router();
            RetainingHandler unbound;
            const auto unboundKey = Dispatch(issued, unbound);
            CHECK_FALSE(unboundKey.route.IsValid());
            CHECK(stack.AttachActions(firstRoute, std::move(issued)).HasError());
            CHECK(unbound.producer->Key() == unboundKey);
            CHECK(issued.AsyncActions()->Snapshot(unboundKey).HasValue());
            REQUIRE(stack.AttachActions(firstRoute, Router()).HasValue());
            REQUIRE(stack.Push(AuthoredId<UiRouteId>(2)).HasValue());
            const auto secondRoute = stack.Top()->id;
            CHECK(stack.AttachActions(secondRoute, std::move(*stack.Actions(firstRoute))).HasError());
            RetainingHandler bound;
            const auto key = Dispatch(*stack.Actions(firstRoute), bound);
            CHECK(key.route == firstRoute);
            auto forged = key;
            forged.route = {};
            CHECK(stack.Actions(firstRoute)->AsyncActions()->Snapshot(forged).HasError());
            forged.route = secondRoute;
            CHECK(stack.Actions(firstRoute)->AsyncActions()->Snapshot(forged).HasError());
            CHECK(bound.producer->Key() == key);
            CHECK_FALSE(bound.producer->Cancellation().IsCancellationRequested());
        }

        TEST_CASE("Released failed records retain copied errors until explicit quiescent collection", "[runtime_ui][async_actions]") {
            auto store = Store(1);
            std::optional<UiAsyncActionProducer> producer;
            producer.emplace(std::move(store.Start(Request())).Value());
            auto error = std::make_shared<const Error>(MakeError(UiErrors::ActionHandlerFailed));
            std::weak_ptr<const Error> weak = error;
            REQUIRE(producer->Fail(error).HasValue());
            auto snapshot = store.Snapshot(producer->Key()).Value();
            REQUIRE(store.Release(producer->Key()).HasValue());
            producer.reset();
            error.reset();
            CHECK(store.CollectReleased() == 0);
            ExpectError(store.Start(Request(2)), UiErrors::AsyncActionCapacityExceeded);
            snapshot = {};
            CHECK_FALSE(weak.expired());
            CHECK(store.CollectReleased() == 1);
            CHECK(weak.expired());
            CHECK(store.Start(Request(2)).HasValue());
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
            REQUIRE(group.Join({Horo::WaitPolicy::MainThreadPumpAllowed, Horo::Duration::FromMilliseconds(1000)}).HasError());
            CHECK(group.Outcome() == Horo::TaskGroupOutcome::Cancelled);
            ExpectError(handler.producer->Complete(), UiErrors::AsyncActionAlreadyTerminal);
            jobs.Shutdown(Horo::ShutdownPolicy::Drain);
        }
    }  // namespace
}  // namespace Horo::Runtime::Ui
