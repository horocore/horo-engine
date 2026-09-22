#include "ScriptInvocationTestSupport.h"

#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

namespace Horo::Extensions::Tests {
    using namespace Support;
    using namespace std::chrono_literals;

    TEST_CASE("Script invocation reserves terminal delivery and publishes exactly one completion", "[Extensions][ScriptInvocation]") {
        ScriptInvocationRegistry registry;
        auto provider = registry.RegisterProvider({.generation = 1, .maximumInvocations = 4});
        REQUIRE(provider.HasValue());
        auto providerRegistration = std::move(provider).Value();
        auto context = registry.RegisterContext({.maximumInvocations = 4, .maximumHandles = 4});
        REQUIRE(context.HasValue());
        auto contextRegistration = std::move(context).Value();
        auto controller = registry.Begin(contextRegistration, providerRegistration, Request(DescriptorSnapshot()));
        REQUIRE(controller.HasValue());
        const auto handle = controller.Value().Handle();
        CHECK(handle.IsValid());
        REQUIRE(controller.Value().PublishProgress({.phase = "work", .completedUnits = 1, .totalUnits = 2}).HasValue());
        RequireErrorCode(controller.Value().Complete(ScriptCallResult::Failure({.domain = "com.example", .code = "failed"})),
                         "script_call_result_invalid");
        REQUIRE(controller.Value().Complete(ScriptCallResult::Success({ScriptValue::String("done")})).HasValue());
        CHECK(controller.Value().Complete(ScriptCallResult::Success({ScriptValue::String("again")})).Value() ==
              ScriptInvocationTransitionResult::AlreadyTerminal);

        const auto events = registry.Drain(contextRegistration);
        REQUIRE(events.HasValue());
        REQUIRE(events.Value().size() == 2);
        CHECK(events.Value()[0].kind == ScriptInvocationEventKind::Progress);
        CHECK(events.Value()[1].kind == ScriptInvocationEventKind::Completed);
        REQUIRE(events.Value()[1].terminalResult.has_value());
        CHECK(events.Value()[1].terminalResult->values.front().AsString() == "done");
        const auto final = handle.Snapshot();
        REQUIRE(final.has_value());
        CHECK(final->state == ScriptInvocationStateKind::Completed);
        CHECK(final->IsTerminal());
    }

    TEST_CASE("Script invocation cancellation, timeout, provider revocation, and context teardown are terminal",
              "[Extensions][ScriptInvocation]") {
        ScriptInvocationRegistry registry;
        auto provider = registry.RegisterProvider({.generation = 2, .maximumInvocations = 8});
        REQUIRE(provider.HasValue());
        auto providerRegistration = std::move(provider).Value();
        auto context = registry.RegisterContext({.maximumInvocations = 8, .maximumHandles = 8});
        REQUIRE(context.HasValue());
        auto contextRegistration = std::move(context).Value();
        const auto snapshot = DescriptorSnapshot();

        auto callerCancelled = registry.Begin(contextRegistration, providerRegistration, Request(snapshot));
        REQUIRE(callerCancelled.HasValue());
        const auto callerHandle = callerCancelled.Value().Handle();
        CHECK(callerHandle.RequestCancellation() == ScriptInvocationCancellationRequestResult::Requested);
        CHECK(callerHandle.Snapshot()->state == ScriptInvocationStateKind::Cancelled);
        CHECK(callerHandle.Snapshot()->cancellationReason == ScriptInvocationCancellationReason::Caller);
        CHECK(callerCancelled.Value().Complete(ScriptCallResult::Success({ScriptValue::String("late")})).Value() ==
              ScriptInvocationTransitionResult::AlreadyTerminal);

        auto timed = Request(snapshot);
        timed.timeout = 0ms;
        auto timeoutController = registry.Begin(contextRegistration, providerRegistration, std::move(timed));
        REQUIRE(timeoutController.HasValue());
        const auto timeoutHandle = timeoutController.Value().Handle();
        REQUIRE(timeoutController.Value().ObserveCancellation().HasValue());
        CHECK(timeoutHandle.Snapshot()->cancellationReason == ScriptInvocationCancellationReason::Timeout);

        auto providerCancelled = registry.Begin(contextRegistration, providerRegistration, Request(snapshot));
        REQUIRE(providerCancelled.HasValue());
        const auto providerHandle = providerCancelled.Value().Handle();
        providerRegistration.Reset();
        CHECK(providerHandle.Snapshot()->cancellationReason == ScriptInvocationCancellationReason::Provider);
        CHECK_FALSE(providerRegistration.IsRegistered());

        auto nextProvider = registry.RegisterProvider({.generation = 3, .maximumInvocations = 8});
        REQUIRE(nextProvider.HasValue());
        auto nextProviderRegistration = std::move(nextProvider).Value();
        auto contextCancelled = registry.Begin(contextRegistration, nextProviderRegistration, Request(snapshot));
        REQUIRE(contextCancelled.HasValue());
        const auto contextHandle = contextCancelled.Value().Handle();
        contextRegistration.Reset();
        CHECK(contextHandle.Snapshot()->cancellationReason == ScriptInvocationCancellationReason::Context);
        CHECK_FALSE(contextHandle.Snapshot()->terminalResult->error->code == "");
        CHECK(registry.Drain(contextRegistration).ErrorValue().code.Value() == "script_invocation_context_revoked");
    }

    TEST_CASE("Script invocation backpressure coalesces progress without losing terminal completion", "[Extensions][ScriptInvocation]") {
        ScriptInvocationRegistryLimits limits;
        limits.maximumQueuedEvents = 1;
        limits.maximumProgressEventsPerInvocation = 4;
        ScriptInvocationRegistry registry(limits);
        auto provider = registry.RegisterProvider({.generation = 4, .maximumInvocations = 2});
        REQUIRE(provider.HasValue());
        auto providerRegistration = std::move(provider).Value();
        auto context = registry.RegisterContext({.maximumInvocations = 2, .maximumHandles = 2});
        REQUIRE(context.HasValue());
        auto contextRegistration = std::move(context).Value();
        auto controller = registry.Begin(contextRegistration, providerRegistration, Request(DescriptorSnapshot()));
        REQUIRE(controller.HasValue());
        CHECK(controller.Value().PublishProgress({.phase = "work", .completedUnits = 1, .totalUnits = 3}).Value() ==
              ScriptInvocationProgressDisposition::Coalesced);
        REQUIRE(controller.Value().Complete(ScriptCallResult::Success({ScriptValue::String("done")})).HasValue());
        const auto events = registry.Drain(contextRegistration);
        REQUIRE(events.HasValue());
        REQUIRE(events.Value().size() == 1);
        CHECK(events.Value().front().kind == ScriptInvocationEventKind::Completed);
    }

    TEST_CASE("Script invocation rejects malformed lifecycle admission inputs", "[Extensions][ScriptInvocation]") {
        ScriptInvocationRegistry registry;
        RequireErrorCode(registry.RegisterProvider({.generation = 0}), "script_invocation_invalid");
        auto provider = registry.RegisterProvider({.generation = 7, .maximumInvocations = 8});
        REQUIRE(provider.HasValue());
        auto providerRegistration = std::move(provider).Value();
        RequireErrorCode(registry.RegisterProvider({.generation = 7}), "script_invocation_provider_duplicate");
        RequireErrorCode(registry.RegisterContext({.maximumInvocations = 0, .maximumHandles = 1}), "script_invocation_invalid");
        auto context = registry.RegisterContext({.maximumInvocations = 8, .maximumHandles = 4});
        REQUIRE(context.HasValue());
        auto contextRegistration = std::move(context).Value();
        const auto snapshot = DescriptorSnapshot();

        auto incomplete = Request(snapshot);
        incomplete.apiId.clear();
        RequireErrorCode(registry.Begin(contextRegistration, providerRegistration, std::move(incomplete)), "script_invocation_invalid");
        auto missingFunction = Request(snapshot);
        missingFunction.functionId = "missing";
        RequireErrorCode(registry.Begin(contextRegistration, providerRegistration, std::move(missingFunction)),
                         "script_invocation_invalid");
        auto unsupportedAffinity = Request(snapshot);
        unsupportedAffinity.completionAffinity = ScriptInvocationCompletionAffinity::Count;
        RequireErrorCode(registry.Begin(contextRegistration, providerRegistration, std::move(unsupportedAffinity)),
                         "script_invocation_invalid");
        auto negativeTimeout = Request(snapshot);
        negativeTimeout.timeout = -1ms;
        RequireErrorCode(registry.Begin(contextRegistration, providerRegistration, std::move(negativeTimeout)),
                         "script_invocation_invalid");
        auto largeTimeout = Request(snapshot);
        largeTimeout.timeout = std::chrono::hours{25};
        RequireErrorCode(registry.Begin(contextRegistration, providerRegistration, std::move(largeTimeout)), "script_invocation_invalid");
    }

    TEST_CASE("Script invocation terminal failure preserves errors and terminalizes abandoned work", "[Extensions][ScriptInvocation]") {
        ScriptInvocationRegistry registry;
        auto provider = registry.RegisterProvider({.generation = 7, .maximumInvocations = 8});
        REQUIRE(provider.HasValue());
        auto providerRegistration = std::move(provider).Value();
        auto context = registry.RegisterContext({.maximumInvocations = 8, .maximumHandles = 4});
        REQUIRE(context.HasValue());
        auto contextRegistration = std::move(context).Value();
        const auto snapshot = DescriptorSnapshot();
        auto controller = registry.Begin(contextRegistration, providerRegistration, Request(snapshot));
        REQUIRE(controller.HasValue());
        CHECK(controller.Value().ObserveCancellation().Value() == ScriptInvocationCancellationObservation::NotRequested);
        RequireErrorCode(controller.Value().PublishProgress({.phase = "", .completedUnits = 0, .totalUnits = 1}),
                         "script_invocation_invalid");
        RequireErrorCode(controller.Value().PublishProgress({.phase = "work", .completedUnits = 2, .totalUnits = 1}),
                         "script_invocation_invalid");
        REQUIRE(controller.Value().PublishProgress({.phase = "work", .completedUnits = 2, .totalUnits = 3}).HasValue());
        RequireErrorCode(controller.Value().PublishProgress({.phase = "work", .completedUnits = 1, .totalUnits = 3}),
                         "script_invocation_invalid");

        const Error foundationError{
            .code = ErrorCode{"backend.failed"},
            .domain = ErrorDomainId{"com.example"},
            .message = "backend failed",
        };
        const auto failedHandle = controller.Value().Handle();
        CHECK(controller.Value().Fail(foundationError, true).Value() == ScriptInvocationTransitionResult::Applied);
        CHECK(failedHandle.Snapshot()->state == ScriptInvocationStateKind::Failed);
        CHECK(failedHandle.Snapshot()->terminalResult->error->retryable);
        CHECK(controller.Value().Fail(ScriptError{.domain = "com.example", .code = "again"}).Value() ==
              ScriptInvocationTransitionResult::AlreadyTerminal);
        CHECK(controller.Value().ObserveCancellation().Value() == ScriptInvocationCancellationObservation::AlreadyTerminal);

        ScriptInvocationHandle abandonedHandle;
        {
            auto abandoned = registry.Begin(contextRegistration, providerRegistration, Request(snapshot));
            REQUIRE(abandoned.HasValue());
            abandonedHandle = abandoned.Value().Handle();
        }
        REQUIRE(abandonedHandle.Snapshot().has_value());
        CHECK(abandonedHandle.Snapshot()->state == ScriptInvocationStateKind::Failed);
        CHECK(abandonedHandle.Snapshot()->terminalResult->error->code == "script_invocation_abandoned");
        CHECK(ScriptInvocationProgress{.completedUnits = 1, .totalUnits = 2}.Fraction() == 0.5);
        CHECK(ScriptInvocationProgress{.completedUnits = 0, .totalUnits = 0}.Fraction() == 0.0);
        CHECK_FALSE(ScriptInvocationSnapshot{.state = ScriptInvocationStateKind::Running}.IsTerminal());
        CHECK(ScriptInvocationSnapshot{.state = ScriptInvocationStateKind::Failed}.IsTerminal());
        const auto drained = registry.Drain(contextRegistration, 0);
        REQUIRE(drained.HasValue());
        CHECK(drained.Value().empty());
    }

    TEST_CASE("Script invocation admission applies bounds and shutdown fail closed", "[Extensions][ScriptInvocation]") {
        ScriptInvocationRegistryLimits limits;
        limits.maximumContexts = 1;
        limits.maximumProviders = 1;
        limits.maximumInvocations = 1;
        limits.maximumQueuedEvents = 2;
        limits.maximumHandlesPerContext = 1;
        ScriptInvocationRegistry registry(limits);
        auto provider = registry.RegisterProvider({.generation = 8, .maximumInvocations = 1});
        REQUIRE(provider.HasValue());
        auto providerRegistration = std::move(provider).Value();
        auto context = registry.RegisterContext({.maximumInvocations = 1, .maximumHandles = 1});
        REQUIRE(context.HasValue());
        auto contextRegistration = std::move(context).Value();
        RequireErrorCode(registry.RegisterProvider({.generation = 9}), "script_invocation_capacity_exceeded");
        RequireErrorCode(registry.RegisterContext({.maximumInvocations = 1, .maximumHandles = 1}), "script_invocation_capacity_exceeded");
        RequireErrorCode(registry.ValidateHandle(ScriptHandle{}), "script_handle_invalid");
        RequireErrorCode(registry.IssueHandle(contextRegistration, providerRegistration, ""), "script_handle_invalid");
        const auto handle = registry.IssueHandle(contextRegistration, providerRegistration, "com.example.resource");
        REQUIRE(handle.HasValue());
        RequireErrorCode(registry.IssueHandle(contextRegistration, providerRegistration, "second"), "script_invocation_capacity_exceeded");
        REQUIRE(registry.ReleaseHandle(handle.Value()).HasValue());
    }

    TEST_CASE("Script invocation rejects foreign registry and shutdown admission", "[Extensions][ScriptInvocation]") {
        ScriptInvocationRegistry registry;
        ScriptInvocationRegistry otherRegistry;
        auto otherProvider = otherRegistry.RegisterProvider({.generation = 9});
        REQUIRE(otherProvider.HasValue());
        auto otherContext = otherRegistry.RegisterContext();
        REQUIRE(otherContext.HasValue());
        RequireErrorCode(registry.Begin(otherContext.Value(), otherProvider.Value(), Request(DescriptorSnapshot())),
                         "script_invocation_unavailable");
        RequireErrorCode(registry.IssueHandle(otherContext.Value(), otherProvider.Value(), "resource"), "script_invocation_unavailable");

        registry.BeginShutdown();
        CHECK(registry.IsShutdown());
        RequireErrorCode(registry.RegisterProvider({.generation = 10}), "script_invocation_shutdown");
        RequireErrorCode(registry.RegisterContext(), "script_invocation_shutdown");
    }

    TEST_CASE("Script opaque handles fail closed after disposal and provider revocation", "[Extensions][ScriptInvocation]") {
        ScriptInvocationRegistry registry;
        auto provider = registry.RegisterProvider({.generation = 5, .maximumInvocations = 2});
        REQUIRE(provider.HasValue());
        auto providerRegistration = std::move(provider).Value();
        auto context = registry.RegisterContext({.maximumInvocations = 2, .maximumHandles = 1});
        REQUIRE(context.HasValue());
        auto contextRegistration = std::move(context).Value();

        auto handle = registry.IssueHandle(contextRegistration, providerRegistration, "com.example.resource");
        REQUIRE(handle.HasValue());
        REQUIRE(registry.ValidateHandle(handle.Value()).HasValue());
        REQUIRE(registry.ReleaseHandle(handle.Value()).HasValue());
        RequireErrorCode(registry.ValidateHandle(handle.Value()), "script_handle_revoked");

        auto second = registry.IssueHandle(contextRegistration, providerRegistration, "com.example.resource");
        REQUIRE(second.HasValue());
        providerRegistration.Reset();
        RequireErrorCode(registry.ValidateHandle(second.Value()), "script_handle_revoked");
    }

    TEST_CASE("Script invocation owner-thread checks reject foreign submission, drain, and handles", "[Extensions][ScriptInvocation]") {
        ScriptInvocationRegistry registry;
        auto provider = registry.RegisterProvider({.generation = 6, .maximumInvocations = 2});
        REQUIRE(provider.HasValue());
        auto providerRegistration = std::move(provider).Value();
        auto context = registry.RegisterContext({.maximumInvocations = 2, .maximumHandles = 2});
        REQUIRE(context.HasValue());
        auto contextRegistration = std::move(context).Value();
        const auto snapshot = DescriptorSnapshot();
        std::string beginError;
        std::string drainError;
        std::string handleError;
        std::thread foreign([&] {
            const auto began = registry.Begin(contextRegistration, providerRegistration, Request(snapshot));
            beginError = began.HasError() ? began.ErrorValue().code.Value() : "unexpected_success";
            const auto drained = registry.Drain(contextRegistration);
            drainError = drained.HasError() ? drained.ErrorValue().code.Value() : "unexpected_success";
            const auto issued = registry.IssueHandle(contextRegistration, providerRegistration, "resource");
            handleError = issued.HasError() ? issued.ErrorValue().code.Value() : "unexpected_success";
        });
        foreign.join();
        CHECK(beginError == "script_invocation_thread_violation");
        CHECK(drainError == "script_invocation_thread_violation");
        CHECK(handleError == "script_invocation_thread_violation");
    }

    TEST_CASE("Script invocation terminalization remains exactly once under cancellation and completion races",
              "[Extensions][ScriptInvocation]") {
        ScriptInvocationRegistry registry;
        auto provider = registry.RegisterProvider({.generation = 10, .maximumInvocations = 2});
        REQUIRE(provider.HasValue());
        auto providerRegistration = std::move(provider).Value();
        auto context = registry.RegisterContext({.maximumInvocations = 2, .maximumHandles = 2});
        REQUIRE(context.HasValue());
        auto contextRegistration = std::move(context).Value();
        auto controller = registry.Begin(contextRegistration, providerRegistration, Request(DescriptorSnapshot()));
        REQUIRE(controller.HasValue());
        const auto handle = controller.Value().Handle();
        std::atomic<int> ready{0};
        std::atomic<bool> start{false};
        const auto waitForStart = [&] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
        };
        std::thread completion([&] {
            waitForStart();
            (void)controller.Value().Complete(ScriptCallResult::Success({ScriptValue::String("done")}));
        });
        std::thread cancellation([&] {
            waitForStart();
            (void)handle.RequestCancellation();
        });
        while (ready.load(std::memory_order_acquire) != 2)
            std::this_thread::yield();
        start.store(true, std::memory_order_release);
        completion.join();
        cancellation.join();

        const auto snapshot = handle.Snapshot();
        REQUIRE(snapshot.has_value());
        CHECK(snapshot->IsTerminal());
        const auto events = registry.Drain(contextRegistration);
        REQUIRE(events.HasValue());
        REQUIRE(events.Value().size() == 1);
        CHECK(events.Value().front().kind == ScriptInvocationEventKind::Completed);
    }
}  // namespace Horo::Extensions::Tests
