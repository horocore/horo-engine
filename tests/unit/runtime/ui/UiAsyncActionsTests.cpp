#include "UiAsyncActionsFixture.h"

namespace Horo::Runtime::Ui {
    namespace {
        using namespace AsyncActionTests;

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
                 std::array<UiAsyncActionProgress, 5>{{{1, 699, true}, {0, 900, true}, {1, 0, false}, {2, 1001, true}, {2, 1, false}}}) {
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

        TEST_CASE("Async scheduling rejection exceptions and abandonment release admission safely", "[runtime_ui][async_actions]") {
            class SubmissionFailure final : public std::runtime_error {
            public:
                SubmissionFailure() : std::runtime_error("provider submission failed") {}
            };

            class RejectingHandler final : public UiAsyncActionHandler {
            public:
                explicit RejectingHandler(const int outcome) : outcome_(outcome) {}

                Result<void> Start(const UiActionRequest &, UiAsyncActionProducer producer) override {
                    key = producer.Key();
                    token = producer.Cancellation();
                    if (outcome_ == 0)
                        return Result<void>::Failure(MakeError(UiErrors::PayloadInvalid));
                    if (outcome_ == 1)
                        throw SubmissionFailure{};
                    return Result<void>::Success();
                }

                UiAsyncActionKey key;
                UiAsyncActionCancellation token;

                int outcome_;
            };

            const auto outcome = GENERATE(0, 1, 2);
            auto router = Router(1);
            RejectingHandler handler{outcome};
            REQUIRE(router.Enqueue(Source(), UiButtonActionCommand{AuthoredId<UiActionId>(), {}}).HasValue());
            const auto dispatched = router.DispatchNext(handler);
            if (outcome == 2) {
                REQUIRE(dispatched.HasValue());
                CHECK(router.AsyncActions()->Snapshot(handler.key).Value().state == UiAsyncActionState::Cancelled);
                REQUIRE(router.AsyncActions()->Release(handler.key).HasValue());
            } else {
                ExpectError(dispatched, outcome == 0 ? UiErrors::PayloadInvalid : UiErrors::ActionHandlerFailed);
                ExpectError(router.AsyncActions()->Snapshot(handler.key), UiErrors::ActionResultStale);
            }
            CHECK(handler.token.IsCancellationRequested());
            CHECK(router.QueuedCount() == 0);
            handler.token = {};
            RetainingHandler replacement;
            Dispatch(router, replacement);
            REQUIRE(replacement.producer->Complete().HasValue());
        }

        TEST_CASE("Async error validation rejects malformed and oversized evidence without consuming the lease",
                  "[runtime_ui][async_actions]") {
            auto store = Store();
            auto producer = std::move(store.Start(Request())).Value();
            const auto kind = GENERATE(0, 1, 2, 3, 4);
            auto error = MakeError(UiErrors::ActionHandlerFailed);
            if (kind == 0)
                error.domain = ErrorDomainId{};
            else if (kind == 1)
                error.severity = static_cast<ErrorSeverity>(255);
            else if (kind == 2)
                error.diagnostics.resize(17);
            else if (kind == 3)
                error.diagnostics.push_back({DiagnosticCode{"provider.invalid"}, static_cast<DiagnosticSeverity>(255), {}, {}, {}});
            else
                for (int node = 0; node < 8; ++node)
                    error = WrapError(UiErrors::ActionHandlerFailed, std::move(error));
            ExpectError(producer.Fail(std::make_shared<const Error>(std::move(error))), UiErrors::AsyncActionFailureInvalid);
            CHECK(store.Snapshot(producer.Key()).Value().Busy());
            auto valid = MakeError(UiErrors::ActionHandlerFailed);
            valid.diagnostics.push_back(
                {DiagnosticCode{"provider.failure"}, DiagnosticSeverity::Error, "detail", {"input", 1, 1}, "/task"});
            REQUIRE(producer.Fail(std::make_shared<const Error>(std::move(valid))).HasValue());
            ExpectError(producer.Fail({}), UiErrors::AsyncActionAlreadyTerminal);
        }

        TEST_CASE("Async moved stores and producers preserve authority and retire displaced work", "[runtime_ui][async_actions]") {
            ExpectError(UiAsyncActionStore::Create({}, 1), UiErrors::ActionInvalid);
            ExpectError(UiAsyncActionStore::Create(Context(), 0), UiErrors::ActionInvalid);
            ExpectError(UiAsyncActionStore::Create(Context(), MaximumUiActionCommands + 1), UiErrors::ActionInvalid);
            auto source = Store();
            // A reference borrowed before transfer still refers to the original facade, whose authority must retire.
            UiAsyncActionStore &sourceView = source;
            auto producer = std::move(source.Start(Request())).Value();
            const auto key = producer.Key();
            REQUIRE(sourceView.Snapshot(key).HasValue());
            auto destination = Store();
            auto displaced = std::move(destination.Start(Request(7))).Value();
            const auto cancelled = displaced.Cancellation();
            destination = std::move(source);
            CHECK(cancelled.IsCancellationRequested());
            ExpectError(sourceView.Start(Request(2)), UiErrors::ActionLifecycleUnavailable);
            ExpectError(sourceView.Snapshot(key), UiErrors::ActionLifecycleUnavailable);
            ExpectError(sourceView.Project(Source()), UiErrors::ActionLifecycleUnavailable);
            ExpectError(sourceView.Cancel(key, UiActionCancellationReason::Requested), UiErrors::ActionLifecycleUnavailable);
            ExpectError(sourceView.Release(key), UiErrors::ActionLifecycleUnavailable);
            auto moved = std::move(producer);
            CHECK(producer.Key() == UiAsyncActionKey{});
            CHECK(producer.Cancellation().IsCancellationRequested());
            ExpectError(producer.PublishProgress({}), UiErrors::ActionResultStale);
            ExpectError(producer.Complete(), UiErrors::ActionResultStale);
            ExpectError(producer.Fail({}), UiErrors::ActionResultStale);
            ExpectError(destination.Cancel(key, UiActionCancellationReason::Count), UiErrors::ActionResultInvalid);
            ExpectError(destination.Release(displaced.Key()), UiErrors::ActionResultStale);
            ExpectError(destination.Project(Source(Context(2))), UiErrors::ActionSourceStale);
            destination.Retire(UiActionCancellationReason::Count);
            CHECK(destination.Snapshot(key).Value().Busy());
            REQUIRE(moved.Complete().HasValue());
            REQUIRE(destination.Release(key).HasValue());
        }

        TEST_CASE("Async owner-thread guards reject worker writes while cancellation remains observable",
                  "[runtime_ui][async_actions][jobs]") {
            auto store = Store();
            auto producer = std::move(store.Start(Request())).Value();
            const auto key = producer.Key();
            const auto token = producer.Cancellation();
            std::array<bool, 8> rejected{};
            // Deliberately invoke forbidden operations to verify they return before reading owner-mutated state.
            std::jthread worker{[&store, &producer, &rejected, &key] {
                rejected = {store.Start(Request(2)).HasError(), store.Snapshot(key).HasError(),
                            store.Project(Source()).HasError(), store.Cancel(key, UiActionCancellationReason::Requested).HasError(),
                            store.Release(key).HasError(),      producer.PublishProgress({1, 500, true}).HasError(),
                            producer.Complete().HasError(),     producer.Fail({}).HasError()};
            }};
            worker.join();
            for (const auto result : rejected)
                CHECK(result);
            CHECK_FALSE(token.IsCancellationRequested());
            CHECK(store.Snapshot(key).Value().Busy());
            REQUIRE(store.Cancel(key, UiActionCancellationReason::Requested).HasValue());
            bool observed{};
            std::jthread observer{[&observed, &token] {
                observed = token.IsCancellationRequested();
            }};
            observer.join();
            CHECK(observed);
        }

    }  // namespace
}  // namespace Horo::Runtime::Ui
