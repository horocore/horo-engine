#include "AllocationProbe.h"
#include "Horo/Foundation/Logging/LogContext.h"
#include "Horo/PlatformServices/PlatformRequest.h"
#include "Horo/PlatformServices/PlatformRequestErrors.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <thread>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::PlatformServices;

    template <typename T> PlatformRequestHandle<T> Running(PlatformRequestStore &store) {
        auto request = store.Admit<T>();
        REQUIRE(request.HasValue());
        auto handle = std::move(request).Value();
        REQUIRE(store.MarkRunning(handle).HasValue());
        return handle;
    }

    template <typename T> PlatformRequestCompletionSink<T> Sink(PlatformRequestStore &store, const PlatformRequestHandle<T> &request) {
        auto sink = store.CompletionSink(request);
        REQUIRE(sink.HasValue());
        return sink.Value();
    }

    template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &expected) {
        INFO("Expected platform completion error: " << expected.code.Value());
        REQUIRE_FALSE(result.HasValue());
        const auto &actual = result.ErrorValue();
        CHECK(std::pair(actual.code.Value(), actual.domain.Value()) == std::pair(expected.code.Value(), expected.domain.Value()));
    }
}  // namespace

TEST_CASE("SDK completion is copied and published only on later engine turns", "[platform-services][completion-queue]") {
    PlatformRequestStore store;
    auto request = Running<std::vector<int>>(store);
    auto sink = Sink(store, request);
    const auto owner = std::this_thread::get_id();
    int calls{};
    auto subscription = store.OnComplete<std::vector<int>>(request, [&](const auto &snapshot) {
        CHECK(std::this_thread::get_id() == owner);
        REQUIRE(snapshot.terminal.has_value());
        CHECK(*snapshot.terminal->Value() == std::vector<int>{1, 2, 3});
        ++calls;
    });
    REQUIRE(subscription.HasValue());
    bool queued{};
    std::thread sdk([&] {
        std::vector<int> payload{1, 2, 3};
        queued = sink.Complete(Result<std::vector<int>>::Success(payload)).HasValue();
        payload.clear();
    });
    sdk.join();
    CHECK(queued);
    CHECK(store.Query(request).Value().state == PlatformRequestState::Running);
    CHECK(store.DispatchCompletions() == 0);
    CHECK(store.DrainProviderCompletions(0).Value().processed == 0);
    const auto report = store.DrainProviderCompletions();
    REQUIRE(report.HasValue());
    CHECK(report.Value().processed == 1);
    CHECK(report.Value().applied == 1);
    CHECK(report.Value().discarded == 0);
    CHECK(calls == 0);
    CHECK(store.Query(request).Value().state == PlatformRequestState::Succeeded);
    CHECK(store.DispatchCompletions() == 1);
    CHECK(calls == 1);
}

TEST_CASE("SDK ingress deduplicates concurrent evidence before it consumes capacity", "[platform-services][completion-queue]") {
    PlatformRequestStore store({.completionCapacity = 1});
    auto request = Running<int>(store);
    auto sink = Sink(store, request);
    std::atomic_int accepted{};
    std::atomic_int duplicate{};
    std::atomic_int errors{};
    std::vector<std::thread> workers;
    for (int index = 0; index < 16; ++index) {
        workers.emplace_back([&, index, sink] {
            const auto outcome = sink.Complete(Result<int>::Success(index));
            if (outcome.HasError())
                ++errors;
            else if (outcome.Value() == PlatformRequestMutation::Applied)
                ++accepted;
            else
                ++duplicate;
        });
    }
    for (auto &worker : workers)
        worker.join();
    CHECK(accepted == 1);
    CHECK(duplicate == 15);
    CHECK(errors == 0);
    CHECK(store.DrainProviderCompletions().Value().applied == 1);
    CHECK(sink.Complete(Result<int>::Success(99)).Value() == PlatformRequestMutation::Unchanged);
    CHECK(store.DrainProviderCompletions().Value().processed == 0);
}

TEST_CASE("Ingress pressure rejects retryably and bounded drain preserves FIFO", "[platform-services][completion-queue]") {
    PlatformRequestStore store({.completionCapacity = 2});
    auto first = Running<int>(store);
    auto second = Running<int>(store);
    auto third = Running<int>(store);
    auto a = Sink(store, first);
    auto b = Sink(store, second);
    auto c = Sink(store, third);
    REQUIRE(a.Complete(Result<int>::Success(1)).HasValue());
    REQUIRE(b.Complete(Result<int>::Success(2)).HasValue());
    RequireError(c.Complete(Result<int>::Success(3)), RequestErrors::CapacityExceeded);
    CHECK(store.DrainProviderCompletions(1).Value().applied == 1);
    CHECK(store.Query(first).Value().state == PlatformRequestState::Succeeded);
    CHECK(store.Query(second).Value().state == PlatformRequestState::Running);
    REQUIRE(c.Complete(Result<int>::Success(3)).Value() == PlatformRequestMutation::Applied);
    CHECK(store.DrainProviderCompletions(1).Value().applied == 1);
    CHECK(store.Query(second).Value().state == PlatformRequestState::Succeeded);
    CHECK(store.Query(third).Value().state == PlatformRequestState::Running);
    CHECK(store.DrainProviderCompletions(1).Value().applied == 1);
    CHECK(*store.Query(third).Value().terminal->Value() == 3);
}

TEST_CASE("Wrong-thread and callback-reentrant drains preserve pending evidence", "[platform-services][completion-queue]") {
    PlatformRequestStore store;
    auto first = Running<void>(store);
    auto second = Running<void>(store);
    auto a = Sink(store, first);
    auto b = Sink(store, second);
    REQUIRE(a.Complete(Result<void>::Success()).HasValue());
    auto subscription = store.OnComplete<void>(first, [&](const auto &) {
        REQUIRE(b.Complete(Result<void>::Success()).HasValue());
        RequireError(store.DrainProviderCompletions(), RequestErrors::ReentrantDrain);
        CHECK(store.DispatchCompletions() == 0);
    });
    REQUIRE(subscription.HasValue());
    REQUIRE(store.DrainProviderCompletions().Value().applied == 1);
    bool wrongThread{};
    std::size_t delivered{1};
    std::thread sdk([&] {
        auto result = store.DrainProviderCompletions();
        wrongThread = result.HasError() && result.ErrorValue().code.Value() == RequestErrors::WrongThread.code.Value();
        delivered = store.DispatchCompletions();
    });
    sdk.join();
    CHECK(wrongThread);
    CHECK(delivered == 0);
    CHECK(store.DispatchCompletions() == 1);
    CHECK(store.Query(second).Value().state == PlatformRequestState::Running);
    CHECK(store.DrainProviderCompletions().Value().applied == 1);
}

TEST_CASE("Provider failure and cancellation enforce typed terminal shapes", "[platform-services][completion-queue]") {
    PlatformRequestStore store;
    auto failed = Running<int>(store);
    auto cancelled = Running<void>(store);
    auto a = Sink(store, failed);
    auto b = Sink(store, cancelled);
    RequireError(a.Complete(Result<int>::Failure(MakeError(RequestErrors::TimedOut))), RequestErrors::InvalidTransition);
    RequireError(b.AcknowledgeCancellation(MakeError(RequestErrors::Cancelled)), RequestErrors::InvalidTransition);
    REQUIRE(store.RequestCancel(cancelled).HasValue());
    RequireError(b.AcknowledgeCancellation(MakeError(RequestErrors::TimedOut)), RequestErrors::InvalidTransition);
    REQUIRE(a.Complete(Result<int>::Failure(MakeError(RequestErrors::CapacityExceeded))).HasValue());
    REQUIRE(b.AcknowledgeCancellation(MakeError(RequestErrors::Cancelled)).HasValue());
    CHECK(store.DrainProviderCompletions().Value().applied == 2);
    CHECK(store.Query(failed).Value().state == PlatformRequestState::Failed);
    CHECK(store.Query(failed).Value().terminal->ErrorValue()->code.Value() == RequestErrors::CapacityExceeded.code.Value());
    CHECK(store.Query(cancelled).Value().state == PlatformRequestState::Cancelled);
}

TEST_CASE("Late evidence cannot replace timeout or a retained immutable terminal result", "[platform-services][completion-queue]") {
    PlatformRequestStore store({.terminalCapacity = 1});
    auto late = Running<int>(store);
    auto sink = Sink(store, late);
    REQUIRE(sink.Complete(Result<int>::Success(7)).HasValue());
    REQUIRE(store.CompleteTimedOut(late, MakeError(RequestErrors::TimedOut)).HasValue());
    const auto report = store.DrainProviderCompletions();
    REQUIRE(report.HasValue());
    CHECK(report.Value().processed == 1);
    CHECK(report.Value().discarded == 1);
    CHECK(report.Value().applied == 0);
    CHECK(store.Query(late).Value().state == PlatformRequestState::TimedOut);
    auto next = Running<void>(store);
    REQUIRE(store.CompleteSuccess(next).HasValue());
    RequireError(sink.Complete(Result<int>::Success(8)), RequestErrors::Stale);
}

TEST_CASE("Session replacement and store destruction revoke retained SDK sinks", "[platform-services][completion-queue]") {
    auto oldStore = std::make_unique<PlatformRequestStore>(PlatformRequestStoreConfig{.generation = {11}});
    auto oldRequest = Running<void>(*oldStore);
    auto sink = Sink(*oldStore, oldRequest);
    PlatformRequestStore replacement({.generation = {12}});
    RequireError(replacement.CompletionSink(oldRequest), RequestErrors::Stale);
    REQUIRE(sink.Complete(Result<void>::Success()).HasValue());
    int calls{};
    auto subscription = oldStore->OnComplete<void>(oldRequest, [&](const auto &) {
        ++calls;
    });
    REQUIRE(subscription.HasValue());
    oldStore->Shutdown();
    oldStore->Shutdown();
    RequireError(sink.Complete(Result<void>::Success()), RequestErrors::FrontendUnavailable);
    RequireError(oldStore->CompletionSink(oldRequest), RequestErrors::FrontendUnavailable);
    RequireError(oldStore->DrainProviderCompletions(0), RequestErrors::FrontendUnavailable);
    CHECK(oldStore->DispatchCompletions() == 0);
    CHECK(calls == 0);
    oldStore.reset();
    RequireError(sink.Complete(Result<void>::Success()), RequestErrors::FrontendUnavailable);
    CHECK(replacement.RecordCount() == 0);
}

TEST_CASE("SDK shutdown race retires ingress without touching destroyed store", "[platform-services][completion-queue]") {
    for (int iteration = 0; iteration < 64; ++iteration) {
        auto store = std::make_unique<PlatformRequestStore>();
        auto request = Running<void>(*store);
        auto sink = Sink(*store, request);
        std::atomic_bool start{};
        std::atomic_bool valid{true};
        std::thread sdk([&] {
            while (!start.load())
                std::this_thread::yield();
            for (int index = 0; index < 32; ++index) {
                const auto result = sink.Complete(Result<void>::Success());
                if (result.HasError() && result.ErrorValue().code.Value() != RequestErrors::FrontendUnavailable.code.Value())
                    valid = false;
            }
        });
        start = true;
        store.reset();
        sdk.join();
        CHECK(valid);
    }
}

TEST_CASE("Request admission context reaches deferred observer and restores the engine context", "[platform-services][completion-queue]") {
    PlatformRequestStore store;
    auto request = [&] {
        const Log::LogContext admission("operation", "admitted");
        return Running<void>(store);
    }();
    auto sink = Sink(store, request);
    bool captured{};
    auto subscription = store.OnComplete<void>(request, [&](const auto &) {
        const auto fields = Log::GetMdcFields();
        captured = fields == std::vector<Log::MdcField>{{"operation", "admitted"}};
    });
    REQUIRE(subscription.HasValue());
    std::thread sdk([&] {
        const Log::LogContext native("operation", "sdk");
        static_cast<void>(sink.Complete(Result<void>::Success()));
    });
    sdk.join();
    const Log::LogContext owner("operation", "engine");
    REQUIRE(store.DrainProviderCompletions().Value().applied == 1);
    CHECK(store.DispatchCompletions() == 1);
    CHECK(captured);
    CHECK(Log::GetMdcFields() == std::vector<Log::MdcField>{{"operation", "engine"}});
}

TEST_CASE("Zero-capacity completion ingress and invalid handles fail before provider admission", "[platform-services][completion-queue]") {
    PlatformRequestStore invalid({.completionCapacity = 0});
    RequireError(invalid.Admit<void>(), RequestErrors::InvalidConfiguration);
    PlatformRequestStore store;
    RequireError(store.CompletionSink(PlatformRequestHandle<void>{}), RequestErrors::Stale);
    auto queued = store.Admit<void>();
    REQUIRE(queued.HasValue());
    auto sink = Sink(store, queued.Value());
    RequireError(sink.Complete(Result<void>::Success()), RequestErrors::InvalidTransition);
    REQUIRE(sink.Complete(Result<void>::Failure(MakeError(RequestErrors::CapacityExceeded))).HasValue());
    CHECK(store.DrainProviderCompletions().Value().applied == 1);
}

TEST_CASE("SDK payload allocation failure is typed and leaves the request retryable", "[platform-services][completion-queue]") {
    PlatformRequestStore store;
    auto request = Running<int>(store);
    auto sink = Sink(store, request);
    auto rejected = [&] {
        const Tests::AllocationProbe::ScopedFailure allocationFailure;
        return sink.Complete(Result<int>::Success(4));
    }();
    RequireError(rejected, RequestErrors::CapacityExceeded);
    CHECK(store.DrainProviderCompletions().Value().processed == 0);
    REQUIRE(sink.Complete(Result<int>::Success(5)).Value() == PlatformRequestMutation::Applied);
    CHECK(store.DrainProviderCompletions().Value().applied == 1);
    CHECK(*store.Query(request).Value().terminal->Value() == 5);
}

TEST_CASE("Shutdown releases queued SDK payloads outside the lifecycle lock", "[platform-services][completion-queue]") {
    PlatformRequestStore store;
    auto request = Running<std::shared_ptr<int>>(store);
    auto sink = Sink(store, request);
    int releases{};
    auto payload = std::shared_ptr<int>(new int{1}, [&](int *value) {
        CHECK(store.RecordCount() == 1);
        ++releases;
        delete value;
    });
    REQUIRE(sink.Complete(Result<std::shared_ptr<int>>::Success(std::move(payload))).HasValue());
    CHECK(releases == 0);
    store.Shutdown();
    CHECK(releases == 1);
}
