#include "Horo/PlatformServices/PlatformRequest.h"
#include "Horo/PlatformServices/PlatformRequestErrors.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <thread>
#include <type_traits>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::PlatformServices;

    [[nodiscard]] Error TestFailure(const char *message = "provider failure") {
        return MakeError(RequestErrors::InvalidTransition, message);
    }

    template <typename T> [[nodiscard]] PlatformRequestHandle<T> Admit(PlatformRequestStore &store) {
        auto result = store.Admit<T>();
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }
}  // namespace

static_assert(!std::is_copy_constructible_v<PlatformRequestHandle<int>>);
static_assert(std::is_nothrow_move_constructible_v<PlatformRequestHandle<int>>);
static_assert(!std::is_copy_constructible_v<PlatformRequestSubscription>);
static_assert(std::is_nothrow_move_constructible_v<PlatformRequestSubscription>);

TEST_CASE("Platform request handle is move-safe and does not own its durable record", "[platform-services][request]") {
    PlatformRequestStore store({.activeCapacity = 2, .terminalCapacity = 2, .observerCapacity = 2, .generation = {7}});
    auto original = Admit<int>(store);
    const auto id = original.Id();
    auto moved = std::move(original);

    CHECK_FALSE(original.IsValid());
    REQUIRE(moved.IsValid());
    CHECK(moved.Id() == id);
    CHECK(store.RecordCount() == 1);
    CHECK(store.Query(original).HasError());

    REQUIRE(store.MarkRunning(moved).HasValue());
    REQUIRE(store.CompleteSuccess(moved, 42).HasValue());
    const auto snapshot = store.Query(moved);
    REQUIRE(snapshot.HasValue());
    CHECK(snapshot.Value().state == PlatformRequestState::Succeeded);
    REQUIRE(snapshot.Value().terminal.has_value());
    REQUIRE(snapshot.Value().terminal->Value() != nullptr);
    CHECK(*snapshot.Value().terminal->Value() == 42);
    CHECK(snapshot.Value().timing.startedAt.has_value());
    CHECK(snapshot.Value().timing.terminalAt.has_value());
    CHECK(*snapshot.Value().timing.terminalAt >= *snapshot.Value().timing.startedAt);
}

TEST_CASE("Request lifecycle rejects illegal transitions and keeps first terminal result immutable", "[platform-services][request]") {
    PlatformRequestStore store;
    auto request = Admit<int>(store);

    CHECK(store.CompleteSuccess(request, 1).HasError());
    REQUIRE(store.MarkRunning(request).HasValue());
    REQUIRE(store.MarkRunning(request).Value() == PlatformRequestMutation::Unchanged);
    REQUIRE(store.CompleteSuccess(request, 7).Value() == PlatformRequestMutation::Applied);
    REQUIRE(store.CompleteFailure(request, TestFailure()).Value() == PlatformRequestMutation::Unchanged);
    REQUIRE(store.CompleteSuccess(request, 9).Value() == PlatformRequestMutation::Unchanged);

    const auto snapshot = store.Query(request);
    REQUIRE(snapshot.HasValue());
    REQUIRE(snapshot.Value().terminal.has_value());
    REQUIRE(snapshot.Value().terminal->Value() != nullptr);
    CHECK(*snapshot.Value().terminal->Value() == 7);
    CHECK_FALSE(snapshot.Value().terminal->HasError());
}

TEST_CASE("Terminal state and typed error identity publish atomically", "[platform-services][request]") {
    PlatformRequestStore store;
    auto timedOut = Admit<int>(store);
    CHECK(store.CompleteTimedOut(timedOut, MakeError(RequestErrors::Cancelled)).HasError());
    REQUIRE(store.CompleteTimedOut(timedOut, MakeError(RequestErrors::TimedOut)).HasValue());
    const auto timeoutSnapshot = store.Query(timedOut);
    REQUIRE(timeoutSnapshot.HasValue());
    CHECK(timeoutSnapshot.Value().state == PlatformRequestState::TimedOut);
    REQUIRE(timeoutSnapshot.Value().terminal.has_value());
    REQUIRE(timeoutSnapshot.Value().terminal->ErrorValue() != nullptr);
    CHECK(timeoutSnapshot.Value().terminal->ErrorValue()->code.Value() == "platform.request.timed_out");

    auto failed = Admit<int>(store);
    CHECK(store.CompleteFailure(failed, MakeError(RequestErrors::Cancelled)).HasError());
    REQUIRE(store.CompleteFailure(failed, TestFailure()).HasValue());
    CHECK(store.Query(failed).Value().state == PlatformRequestState::Failed);
}

TEST_CASE("Cancellation is thread-safe idempotent and cannot rewrite a completed request", "[platform-services][request][concurrency]") {
    PlatformRequestStore store;
    auto request = Admit<int>(store);
    REQUIRE(store.MarkRunning(request).HasValue());

    std::atomic_uint applied{};
    std::atomic_bool allSucceeded{true};
    std::vector<std::thread> cancellers;
    for (int index = 0; index < 16; ++index) {
        cancellers.emplace_back([&] {
            const auto result = store.RequestCancel(request);
            if (result.HasError()) {
                allSucceeded = false;
                return;
            }
            if (result.Value() == PlatformRequestMutation::Applied)
                ++applied;
        });
    }
    for (auto &thread : cancellers)
        thread.join();

    CHECK(applied.load() == 1);
    CHECK(allSucceeded.load());
    const auto cancelling = store.Query(request);
    REQUIRE(cancelling.HasValue());
    CHECK(cancelling.Value().state == PlatformRequestState::Cancelling);
    CHECK(cancelling.Value().cancellationRequested);
    CHECK(cancelling.Value().timing.cancellationRequestedAt.has_value());

    REQUIRE(store.CompleteCancelled(request, MakeError(RequestErrors::Cancelled)).Value() == PlatformRequestMutation::Applied);
    CHECK(store.RequestCancel(request).Value() == PlatformRequestMutation::Unchanged);
    CHECK(store.CompleteFailure(request, TestFailure()).Value() == PlatformRequestMutation::Unchanged);
    CHECK(store.Query(request).Value().state == PlatformRequestState::Cancelled);
}

TEST_CASE("Backend cancellation uses the Horo request identity without knowing its result type", "[platform-services][request][cancel]") {
    PlatformRequestStore store({.generation = {9}});
    auto request = Admit<int>(store);

    REQUIRE(store.RequestCancel(request.Id(), request.Generation()).Value() == PlatformRequestMutation::Applied);
    CHECK(store.RequestCancel(request.Id(), request.Generation()).Value() == PlatformRequestMutation::Unchanged);
    const auto snapshot = store.Query(request);
    REQUIRE(snapshot.HasValue());
    CHECK(snapshot.Value().state == PlatformRequestState::Cancelling);
    CHECK(snapshot.Value().cancellationRequested);

    CHECK(store.RequestCancel(request.Id(), PlatformRequestGeneration{10}).ErrorValue().code.Value() == RequestErrors::Stale.code.Value());
}

TEST_CASE("Concurrent success and cancellation publish exactly one coherent terminal result", "[platform-services][request][concurrency]") {
    for (int iteration = 0; iteration < 128; ++iteration) {
        PlatformRequestStore store;
        auto request = Admit<int>(store);
        REQUIRE(store.MarkRunning(request).HasValue());
        std::thread cancel([&] {
            static_cast<void>(store.RequestCancel(request));
        });
        std::thread complete([&] {
            static_cast<void>(store.CompleteSuccess(request, iteration));
        });
        cancel.join();
        complete.join();

        const auto snapshot = store.Query(request);
        REQUIRE(snapshot.HasValue());
        REQUIRE(snapshot.Value().state == PlatformRequestState::Succeeded);
        REQUIRE(snapshot.Value().terminal.has_value());
        REQUIRE(snapshot.Value().terminal->Value() != nullptr);
        CHECK(*snapshot.Value().terminal->Value() == iteration);
    }
}

TEST_CASE("OnComplete is deferred for early and late registration and permits non-recursive reentry",
          "[platform-services][request][callback]") {
    PlatformRequestStore store;
    auto request = Admit<int>(store);
    int callbackCount = 0;
    bool sawCommittedSnapshot = false;
    std::size_t recursiveDispatchCount = 99;
    auto early = store.OnComplete<int>(request, [&](const PlatformRequestSnapshot<int> &snapshot) {
        ++callbackCount;
        sawCommittedSnapshot = store.Query(request).Value().state == PlatformRequestState::Succeeded;
        recursiveDispatchCount = store.DispatchCompletions();
        REQUIRE(snapshot.terminal->Value() != nullptr);
        CHECK(*snapshot.terminal->Value() == 17);
    });
    REQUIRE(early.HasValue());
    auto earlyToken = std::move(early).Value();
    REQUIRE(store.MarkRunning(request).HasValue());
    REQUIRE(store.CompleteSuccess(request, 17).HasValue());
    CHECK(callbackCount == 0);

    CHECK(store.DispatchCompletions() == 1);
    CHECK(callbackCount == 1);
    CHECK(sawCommittedSnapshot);
    CHECK(recursiveDispatchCount == 0);
    CHECK_FALSE(earlyToken.IsActive());

    auto late = store.OnComplete<int>(request, [&](const PlatformRequestSnapshot<int> &) {
        ++callbackCount;
    });
    REQUIRE(late.HasValue());
    auto lateToken = std::move(late).Value();
    CHECK(callbackCount == 1);
    CHECK(store.DispatchCompletions() == 1);
    CHECK(callbackCount == 2);
    CHECK_FALSE(lateToken.IsActive());
    CHECK(store.ObserverCount() == 0);
}

TEST_CASE("Destroyed subscription suppresses delivery and releases captured callback state", "[platform-services][request][lifetime]") {
    PlatformRequestStore store;
    auto request = Admit<int>(store);
    auto captured = std::make_shared<int>(4);
    std::weak_ptr<int> weakCaptured = captured;
    {
        auto subscription = store.OnComplete<int>(request, [captured](const PlatformRequestSnapshot<int> &) {
        });
        REQUIRE(subscription.HasValue());
        auto token = std::move(subscription).Value();
        captured.reset();
        CHECK_FALSE(weakCaptured.expired());
        CHECK(store.ObserverCount() == 1);
    }
    CHECK(weakCaptured.expired());
    CHECK(store.ObserverCount() == 0);
    REQUIRE(store.MarkRunning(request).HasValue());
    REQUIRE(store.CompleteSuccess(request, 5).HasValue());
    CHECK(store.DispatchCompletions() == 0);

    for (int index = 0; index < 128; ++index) {
        auto late = store.OnComplete<int>(request, [](const PlatformRequestSnapshot<int> &) {
        });
        REQUIRE(late.HasValue());
    }
    CHECK(store.ObserverCount() == 0);
    CHECK(store.DispatchCompletions() == 0);
}

TEST_CASE("Terminal retention is bounded and expired identities never alias newer records", "[platform-services][request][retention]") {
    PlatformRequestStore store({.activeCapacity = 2, .terminalCapacity = 1, .observerCapacity = 2, .generation = {11}});
    auto first = Admit<int>(store);
    REQUIRE(store.MarkRunning(first).HasValue());
    REQUIRE(store.CompleteSuccess(first, 1).HasValue());
    auto second = Admit<int>(store);
    REQUIRE(store.MarkRunning(second).HasValue());
    REQUIRE(store.CompleteSuccess(second, 2).HasValue());

    const auto expired = store.Query(first);
    REQUIRE(expired.HasError());
    CHECK(expired.ErrorValue().code.Value() == "platform.request.expired");
    CHECK(store.Query(second).HasValue());
    CHECK(store.RecordCount() == 1);
    CHECK(first.Id() != second.Id());
}

TEST_CASE("Frontend generation fences handles from another store", "[platform-services][request][generation]") {
    PlatformRequestStore oldStore({.generation = {8}});
    PlatformRequestStore replacement({.generation = {9}});
    auto oldRequest = Admit<int>(oldStore);

    const auto stale = replacement.Query(oldRequest);
    REQUIRE(stale.HasError());
    CHECK(stale.ErrorValue().code.Value() == "platform.request.stale");
    CHECK(replacement.RecordCount() == 0);
}

TEST_CASE("Admission and observer capacities reject without partial records or callbacks", "[platform-services][request][capacity]") {
    PlatformRequestStore store({.activeCapacity = 1, .terminalCapacity = 1, .observerCapacity = 1, .generation = {2}});
    auto request = Admit<int>(store);
    const auto rejected = store.Admit<int>();
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == "platform.request.capacity_exceeded");
    CHECK(store.RecordCount() == 1);

    auto firstObserver = store.OnComplete<int>(request, [](const PlatformRequestSnapshot<int> &) {
    });
    REQUIRE(firstObserver.HasValue());
    const auto rejectedObserver = store.OnComplete<int>(request, [](const PlatformRequestSnapshot<int> &) {
    });
    REQUIRE(rejectedObserver.HasError());
    CHECK(rejectedObserver.ErrorValue().code.Value() == "platform.request.capacity_exceeded");
    CHECK(store.ObserverCount() == 1);
}

TEST_CASE("Invalid store limits fail admission without creating identity", "[platform-services][request][capacity]") {
    PlatformRequestStore store({.activeCapacity = 0, .terminalCapacity = 1, .observerCapacity = 1, .generation = {4}});
    const auto rejected = store.Admit<int>();
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == "platform.request.invalid_configuration");
    CHECK(store.RecordCount() == 0);
}

TEST_CASE("Void requests retain typed success and callback exceptions cannot change the result", "[platform-services][request][callback]") {
    PlatformRequestStore store;
    auto request = Admit<void>(store);
    auto subscription = store.OnComplete<void>(request, [](const PlatformRequestSnapshot<void> &) {
        throw 4;
    });
    REQUIRE(subscription.HasValue());
    auto token = std::move(subscription).Value();
    REQUIRE(store.MarkRunning(request).HasValue());
    REQUIRE(store.CompleteSuccess(request).HasValue());
    CHECK(store.DispatchCompletions() == 1);
    CHECK(store.CallbackFailureCount() == 1);
    const auto snapshot = store.Query(request);
    REQUIRE(snapshot.HasValue());
    REQUIRE(snapshot.Value().terminal.has_value());
    CHECK(snapshot.Value().terminal->HasValue());
    CHECK(snapshot.Value().state == PlatformRequestState::Succeeded);
    CHECK_FALSE(token.IsActive());
}

TEST_CASE("Shutdown is idempotent terminalizes all accepted records and suppresses observers", "[platform-services][request][shutdown]") {
    PlatformRequestStore store({.activeCapacity = 3, .terminalCapacity = 3, .observerCapacity = 3, .generation = {3}});
    auto queued = Admit<int>(store);
    auto running = Admit<void>(store);
    REQUIRE(store.MarkRunning(running).HasValue());
    int callbacks = 0;
    auto subscription = store.OnComplete<int>(queued, [&](const PlatformRequestSnapshot<int> &) {
        ++callbacks;
    });
    REQUIRE(subscription.HasValue());
    auto token = std::move(subscription).Value();

    store.Shutdown();
    store.Shutdown();
    CHECK(store.Query(queued).Value().state == PlatformRequestState::Failed);
    CHECK(store.Query(running).Value().state == PlatformRequestState::Failed);
    CHECK(store.Query(queued).Value().terminal->HasError());
    CHECK(store.RecordCount() == 2);
    CHECK(store.ObserverCount() == 0);
    CHECK_FALSE(token.IsActive());
    CHECK(store.DispatchCompletions() == 0);
    CHECK(callbacks == 0);
    const auto rejected = store.Admit<int>();
    REQUIRE(rejected.HasError());
    CHECK(rejected.ErrorValue().code.Value() == "platform.frontend.unavailable");
    const auto rejectedObserver = store.OnComplete<int>(queued, [](const PlatformRequestSnapshot<int> &) {
    });
    REQUIRE(rejectedObserver.HasError());
    CHECK(rejectedObserver.ErrorValue().code.Value() == "platform.frontend.unavailable");
}
