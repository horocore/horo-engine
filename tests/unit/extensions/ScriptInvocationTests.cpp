#include "Horo/Extensions/ScriptInvocation.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>
#include <utility>

namespace Horo::Extensions::Tests {
    namespace {
        using namespace std::chrono_literals;

        ScriptExportTypeReference Primitive(ScriptExportPrimitiveKind kind,
                                            ScriptExportNullability nullability = ScriptExportNullability::NonNull) {
            return {.primitive = kind, .nullability = nullability};
        }

        ScriptExportTypeReference Named(std::string id, ScriptExportNullability nullability = ScriptExportNullability::NonNull) {
            return {.primitive = ScriptExportPrimitiveKind::Count, .namedType = std::move(id), .nullability = nullability};
        }

        ScriptExportTypeDescriptor MakeRequestType() {
            return {
                .id = "com.example.request",
                .kind = ScriptExportNamedTypeKind::Struct,
                .introducedVersion = {1, 0, 0},
                .fields =
                    {
                        {.id = "name", .type = Primitive(ScriptExportPrimitiveKind::String), .introducedVersion = {1, 0, 0}},
                        {.id = "note",
                         .type = Primitive(ScriptExportPrimitiveKind::String, ScriptExportNullability::Nullable),
                         .introducedVersion = {1, 0, 0},
                         .requirement = ScriptExportParameterRequirement::Optional,
                         .canonicalDefault = std::vector<std::byte>{std::byte{0}}},
                    },
            };
        }

        ScriptExportFunctionDescriptor MakeFetchFunction() {
            return {
                .id = "fetch",
                .introducedVersion = {1, 0, 0},
                .invocation = ScriptExportInvocationMode::Asynchronous,
                .parameters =
                    {
                        {.id = "request", .type = Named("com.example.request"), .introducedVersion = {1, 0, 0}},
                    },
                .results =
                    {
                        {.id = "answer", .type = Primitive(ScriptExportPrimitiveKind::String), .introducedVersion = {1, 0, 0}},
                    },
                .errorIds = {"com.example.failed"},
            };
        }

        ScriptExportDescriptor MakeDescriptor() {
            ScriptExportDescriptor descriptor{
                .moduleId = "com.example.module",
                .id = "com.example.api",
                .nameSpace = "com.example",
                .version = {1, 0, 0},
                .compatibility = {.minimum = {1, 0, 0}, .maximum = {1, 0, 0}},
                .service =
                    {
                        .capability = {"com.example.service"},
                        .serviceId = "com.example.service",
                        .contractId = "com.example.contract",
                        .minimumVersion = {1, 0, 0},
                    },
            };
            descriptor.types = {MakeRequestType()};
            descriptor.functions = {MakeFetchFunction()};
            descriptor.errors = {
                {.id = "com.example.failed", .introducedVersion = {1, 0, 0}},
            };
            return descriptor;
        }

        ScriptExportDescriptorSnapshotPtr DescriptorSnapshot() {
            const auto descriptor = MakeDescriptor();
            auto snapshot = BuildScriptExportDescriptorSnapshot(std::array{descriptor});
            REQUIRE(snapshot.HasValue());
            return snapshot.Value();
        }

        ScriptInvocationRequest Request(const ScriptExportDescriptorSnapshotPtr &snapshot) {
            return ScriptInvocationRequest{
                .descriptors = snapshot,
                .apiId = "com.example.api",
                .functionId = "fetch",
                .arguments = {ScriptValue::Struct("com.example.request", {{"name", ScriptValue::String("horo")}})},
            };
        }

        void RequireErrorCode(const auto &result, std::string_view code) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == code);
        }
    }  // namespace

    TEST_CASE("Script values round-trip owned recursive data through canonical bytes", "[Extensions][ScriptValue]") {
        const ScriptHandle handle{
            .context = {7},
            .providerGeneration = 11,
            .value = 13,
            .generation = 17,
            .type = "com.example.resource",
        };
        const auto value = ScriptValue::Map({
            {ScriptValue::String("bytes"), ScriptValue{ScriptValue::Bytes{std::byte{0x01}, std::byte{0x02}}}},
            {ScriptValue::String("enum"), ScriptValue{ScriptEnumValue{"com.example.mode", 2}}},
            {ScriptValue::String("handle"), ScriptValue{handle}},
            {ScriptValue::String("nested"), ScriptValue::Array({ScriptValue{true}, ScriptValue{std::int64_t{-3}}, ScriptValue{2.5}})},
            {ScriptValue::String("null"), ScriptValue::Null()},
        });

        REQUIRE(ValidateScriptValue(value).HasValue());
        const auto encoded = EncodeScriptValue(value);
        REQUIRE(encoded.HasValue());
        const auto decoded = DecodeScriptValue(encoded.Value());
        REQUIRE(decoded.HasValue());
        CHECK(decoded.Value() == value);
        CHECK(EncodeScriptValue(decoded.Value()).Value() == encoded.Value());
    }

    TEST_CASE("Script value validation rejects unsupported, oversized, and non-finite payloads", "[Extensions][ScriptValue]") {
        auto limits = ScriptValueLimits{};
        limits.maximumDepth = 1;
        const auto nested = ScriptValue::Array({ScriptValue::Array({ScriptValue{true}})});
        RequireErrorCode(ValidateScriptValue(nested, limits), "script_value_capacity_exceeded");

        limits = {};
        limits.maximumStringBytes = 2;
        RequireErrorCode(ValidateScriptValue(ScriptValue::String("long"), limits), "script_value_capacity_exceeded");

        RequireErrorCode(ValidateScriptValue(ScriptValue{std::numeric_limits<double>::infinity()}), "script_value_invalid");
        RequireErrorCode(ValidateScriptValue(ScriptValue::String(std::string{"\xff", 1})), "script_value_invalid");

        const auto duplicateMap = ScriptValue::Map({
            {ScriptValue::String("same"), ScriptValue{std::int64_t{1}}},
            {ScriptValue::String("same"), ScriptValue{std::int64_t{2}}},
        });
        RequireErrorCode(ValidateScriptValue(duplicateMap), "script_value_invalid");

        ScriptValue malformedHandle{ScriptHandle{.context = {1}, .providerGeneration = 2, .value = 3, .generation = 0, .type = "x"}};
        RequireErrorCode(ValidateScriptValue(malformedHandle), "script_value_invalid");
    }

    TEST_CASE("Script value type validation preserves descriptor nullability and result arity", "[Extensions][ScriptValue]") {
        const auto descriptor = MakeDescriptor();
        const auto structValue = ScriptValue::Struct("com.example.request", {{"name", ScriptValue::String("horo")}});
        REQUIRE(ValidateScriptValueForType(structValue, Named("com.example.request"), descriptor).HasValue());
        REQUIRE(ValidateScriptValueForType(ScriptValue::Null(),
                                           Primitive(ScriptExportPrimitiveKind::String, ScriptExportNullability::Nullable), descriptor)
                    .HasValue());
        RequireErrorCode(ValidateScriptValueForType(ScriptValue::Null(), Primitive(ScriptExportPrimitiveKind::String), descriptor),
                         "script_value_type_mismatch");

        const auto &function = descriptor.functions.front();
        const auto result = ScriptCallResult::Success({ScriptValue::String("ok")});
        REQUIRE(ValidateScriptResult(function, result, descriptor).HasValue());
        RequireErrorCode(ValidateScriptResult(function, ScriptCallResult::Success(), descriptor), "script_value_type_mismatch");
    }

    TEST_CASE("Script invocation reserves terminal delivery and publishes exactly one completion", "[Extensions][ScriptInvocation]") {
        ScriptInvocationRegistry registry;
        auto provider = registry.RegisterProvider({.generation = 1, .maximumInvocations = 4});
        REQUIRE(provider.HasValue());
        auto providerRegistration = std::move(provider).Value();
        auto context = registry.RegisterContext({.maximumInvocations = 4, .maximumHandles = 4});
        REQUIRE(context.HasValue());
        auto contextRegistration = std::move(context).Value();
        const auto snapshot = DescriptorSnapshot();
        auto controller = registry.Begin(contextRegistration, providerRegistration, Request(snapshot));
        REQUIRE(controller.HasValue());
        const auto handle = controller.Value().Handle();
        CHECK(handle.IsValid());
        REQUIRE(controller.Value().PublishProgress({.phase = "work", .completedUnits = 1, .totalUnits = 2}).HasValue());
        RequireErrorCode(controller.Value().Complete(ScriptCallResult::Failure({
                             .domain = "com.example",
                             .code = "failed",
                         })),
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

    TEST_CASE("Script opaque handles fail closed after explicit disposal and provider revocation", "[Extensions][ScriptInvocation]") {
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

    TEST_CASE("Script invocation owner-thread checks reject foreign submission and drain", "[Extensions][ScriptInvocation]") {
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
        std::thread foreign([&] {
            auto began = registry.Begin(contextRegistration, providerRegistration, Request(snapshot));
            REQUIRE(began.HasError());
            beginError = began.ErrorValue().code.Value();
            auto drained = registry.Drain(contextRegistration);
            REQUIRE(drained.HasError());
            drainError = drained.ErrorValue().code.Value();
        });
        foreign.join();
        CHECK(beginError == "script_invocation_thread_violation");
        CHECK(drainError == "script_invocation_thread_violation");
    }
}  // namespace Horo::Extensions::Tests
