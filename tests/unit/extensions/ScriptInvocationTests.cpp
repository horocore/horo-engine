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

        void AppendU64(std::vector<std::byte> &bytes, std::uint64_t value) {
            for (std::size_t index = 0; index < sizeof(value); ++index)
                bytes.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
        }

        ScriptExportDescriptor MakeTypedDescriptor() {
            auto descriptor = MakeDescriptor();
            descriptor.types.push_back({
                .id = "com.example.mode",
                .kind = ScriptExportNamedTypeKind::Enum,
                .introducedVersion = {1, 0, 0},
                .enumValues =
                    {
                        {.id = "off", .value = 0, .introducedVersion = {1, 0, 0}},
                        {.id = "on", .value = 1, .introducedVersion = {1, 0, 0}},
                    },
            });
            descriptor.types.push_back({
                .id = "com.example.names",
                .kind = ScriptExportNamedTypeKind::Array,
                .introducedVersion = {1, 0, 0},
                .elementType = Primitive(ScriptExportPrimitiveKind::String),
                .maximumElements = 2,
            });
            descriptor.types.push_back({
                .id = "com.example.weights",
                .kind = ScriptExportNamedTypeKind::Map,
                .introducedVersion = {1, 0, 0},
                .keyType = Primitive(ScriptExportPrimitiveKind::String),
                .valueType = Primitive(ScriptExportPrimitiveKind::Number),
                .maximumElements = 2,
            });
            descriptor.types.push_back({
                .id = "com.example.cycle",
                .kind = ScriptExportNamedTypeKind::Array,
                .introducedVersion = {1, 0, 0},
                .elementType = Named("com.example.cycle"),
                .maximumElements = 1,
            });
            descriptor.types.push_back({.id = "com.example.invalid", .kind = ScriptExportNamedTypeKind::Count});
            return descriptor;
        }
    }  // namespace

    TEST_CASE("Script values expose every owned alternative and value equality is structural", "[Extensions][ScriptValue]") {
        const ScriptHandle handle{
            .context = {7},
            .providerGeneration = 11,
            .value = 13,
            .generation = 17,
            .type = "com.example.resource",
        };
        const auto enumValue = ScriptValue{ScriptEnumValue{"com.example.mode", 1}};
        const auto bytes = ScriptValue{ScriptValue::Bytes{std::byte{0x01}, std::byte{0x02}}};
        const auto array = ScriptValue::Array({ScriptValue{std::int64_t{1}}, ScriptValue::String("two")});
        const auto map = ScriptValue::Map({
            {ScriptValue::String("a"), ScriptValue{std::int64_t{1}}},
            {ScriptValue::String("b"), ScriptValue{true}},
        });
        const auto reorderedMap = ScriptValue::Map({
            {ScriptValue::String("b"), ScriptValue{true}},
            {ScriptValue::String("a"), ScriptValue{std::int64_t{1}}},
        });
        const auto structure = ScriptValue::Struct("com.example.request", {{"name", ScriptValue::String("horo")}});
        const auto reorderedStructure = ScriptValue::Struct("com.example.request", {{"name", ScriptValue::String("horo")}});

        CHECK(ScriptValue::Null().IsNull());
        CHECK(ScriptValue{true}.GetKind() == ScriptValue::Kind::Boolean);
        CHECK(ScriptValue{std::int64_t{-1}}.AsSignedInteger() != nullptr);
        CHECK(ScriptValue{std::uint64_t{1}}.AsUnsignedInteger() != nullptr);
        CHECK(ScriptValue{1.5}.AsNumber() != nullptr);
        CHECK(ScriptValue::String("text").AsString() == "text");
        CHECK(bytes.AsBytes().size() == 2);
        CHECK(ScriptValue{handle}.AsHandle()->IsValid());
        CHECK(enumValue.AsEnum()->value == 1);
        CHECK(array.AsArray().size() == 2);
        CHECK(map.AsMap().size() == 2);
        CHECK(structure.StructType() == "com.example.request");
        CHECK(structure.AsStruct().size() == 1);
        CHECK(ScriptValue::String("text").AsArray().empty());
        CHECK(map == reorderedMap);
        CHECK(structure == reorderedStructure);
        CHECK(map != ScriptValue::Map({{ScriptValue::String("a"), ScriptValue{std::int64_t{2}}}}));

        const auto success = ScriptCallResult::Success({ScriptValue::String("ok")});
        CHECK(success.IsSuccess());
        CHECK_FALSE(success.IsFailure());
        const auto failure = ScriptCallResult::Failure({.domain = "com.example", .code = "failed"});
        CHECK(failure.IsFailure());
        CHECK_FALSE(failure.IsSuccess());
    }

    TEST_CASE("Script value validation covers named collections, cycles, errors, and finite bounds", "[Extensions][ScriptValue]") {
        const auto descriptor = MakeTypedDescriptor();
        const auto structValue = ScriptValue::Struct("com.example.request", {{"name", ScriptValue::String("horo")}});
        const auto enumValue = ScriptValue{ScriptEnumValue{"com.example.mode", 1}};
        const auto namesValue = ScriptValue::Array({ScriptValue::String("one"), ScriptValue::String("two")});
        const auto weightsValue = ScriptValue::Map({
            {ScriptValue::String("one"), ScriptValue{1.0}},
            {ScriptValue::String("two"), ScriptValue{2.0}},
        });

        REQUIRE(ValidateScriptValueForType(enumValue, Named("com.example.mode"), descriptor).HasValue());
        RequireErrorCode(ValidateScriptValueForType(ScriptValue{ScriptEnumValue{"com.example.mode", 9}}, Named("com.example.mode"),
                                                    descriptor),
                         "script_value_type_mismatch");
        REQUIRE(ValidateScriptValueForType(namesValue, Named("com.example.names"), descriptor).HasValue());
        RequireErrorCode(ValidateScriptValueForType(ScriptValue::Array({ScriptValue{std::int64_t{1}}}), Named("com.example.names"),
                                                    descriptor),
                         "script_value_type_mismatch");
        RequireErrorCode(ValidateScriptValueForType(ScriptValue::Array(
                                                        {ScriptValue::String("1"), ScriptValue::String("2"), ScriptValue::String("3")}),
                                                    Named("com.example.names"), descriptor),
                         "script_value_type_mismatch");
        REQUIRE(ValidateScriptValueForType(weightsValue, Named("com.example.weights"), descriptor).HasValue());
        RequireErrorCode(ValidateScriptValueForType(ScriptValue::Map({{ScriptValue::String("one"), ScriptValue{true}}}),
                                                    Named("com.example.weights"), descriptor),
                         "script_value_type_mismatch");
        RequireErrorCode(ValidateScriptValueForType(ScriptValue::Map({{ScriptValue{std::int64_t{1}}, ScriptValue{1.0}}}),
                                                    Named("com.example.weights"), descriptor),
                         "script_value_type_mismatch");

        RequireErrorCode(ValidateScriptValueForType(ScriptValue::Struct("com.example.request", {{"unknown", ScriptValue{true}}}),
                                                    Named("com.example.request"), descriptor),
                         "script_value_type_mismatch");
        RequireErrorCode(ValidateScriptValueForType(ScriptValue::Struct("com.example.request", {}), Named("com.example.request"),
                                                    descriptor),
                         "script_value_type_mismatch");
        RequireErrorCode(ValidateScriptValueForType(ScriptValue::Array({}), Named("com.example.missing"), descriptor),
                         "script_value_invalid");
        RequireErrorCode(ValidateScriptValueForType(ScriptValue::Array({ScriptValue::Array({})}), Named("com.example.cycle"), descriptor),
                         "script_value_invalid");
        RequireErrorCode(ValidateScriptValueForType(ScriptValue::String("x"), Named("com.example.invalid"), descriptor),
                         "script_value_invalid");
        RequireErrorCode(ValidateScriptValueForType(ScriptValue::String("x"),
                                                    {.primitive = ScriptExportPrimitiveKind::String, .namedType = "unexpected"},
                                                    descriptor),
                         "script_value_invalid");
        RequireErrorCode(ValidateScriptValueForType(ScriptValue::String("x"),
                                                    {.primitive = ScriptExportPrimitiveKind::String,
                                                     .nullability = ScriptExportNullability::Count},
                                                    descriptor),
                         "script_value_invalid");

        const auto &function = descriptor.functions.front();
        RequireErrorCode(ValidateScriptArguments(function, {}, descriptor), "script_value_type_mismatch");
        const std::vector<ScriptValue> tooManyArguments{structValue, ScriptValue{true}};
        RequireErrorCode(ValidateScriptArguments(function, tooManyArguments, descriptor), "script_value_type_mismatch");
        auto optionalFunction = function;
        optionalFunction.parameters.push_back({
            .id = "optional",
            .type = Primitive(ScriptExportPrimitiveKind::Boolean),
            .introducedVersion = {1, 0, 0},
            .requirement = ScriptExportParameterRequirement::Optional,
        });
        const std::vector<ScriptValue> requiredOnly{structValue};
        REQUIRE(ValidateScriptArguments(optionalFunction, requiredOnly, descriptor).HasValue());

        ScriptError validError{
            .domain = "com.example",
            .code = "failed",
            .message = "diagnostic",
            .retryable = true,
            .details = {{.id = "attempt", .value = ScriptValue{std::int64_t{2}}}},
        };
        REQUIRE(ValidateScriptError(validError).HasValue());
        auto duplicateDetails = validError;
        duplicateDetails.details.push_back(duplicateDetails.details.front());
        RequireErrorCode(ValidateScriptError(duplicateDetails), "script_value_invalid");
        RequireErrorCode(ValidateScriptError(ScriptError{.domain = "", .code = "failed"}), "script_value_invalid");
        auto smallErrorLimits = ScriptValueLimits{};
        smallErrorLimits.maximumErrorDetails = 1;
        auto tooManyDetails = validError;
        tooManyDetails.details.push_back({.id = "second", .value = ScriptValue::Null()});
        RequireErrorCode(ValidateScriptError(tooManyDetails, smallErrorLimits), "script_value_capacity_exceeded");

        const auto cause = Error{
            .code = ErrorCode{"cause"},
            .domain = ErrorDomainId{"com.example"},
            .message = "cause",
        };
        const auto wrapped = WithCause(
            Error{
                .code = ErrorCode{"outer"},
                .domain = ErrorDomainId{"com.example"},
                .message = "outer",
            },
            cause);
        const auto converted = MakeScriptError(wrapped, true);
        REQUIRE(converted.HasValue());
        CHECK(converted.Value().retryable);
        REQUIRE(converted.Value().details.size() == 1);
        CHECK(converted.Value().details.front().id == "cause.com.example");
        smallErrorLimits.maximumErrorDetails = 0;
        RequireErrorCode(MakeScriptError(wrapped, false, smallErrorLimits), "script_value_capacity_exceeded");

        auto zeroWork = ScriptValueLimits{};
        zeroWork.maximumWorkUnits = 0;
        RequireErrorCode(ValidateScriptValue(ScriptValue::Null(), zeroWork), "script_value_capacity_exceeded");
        auto zeroElements = ScriptValueLimits{};
        zeroElements.maximumElements = 0;
        RequireErrorCode(ValidateScriptValue(ScriptValue::Array({ScriptValue::Null()}), zeroElements), "script_value_capacity_exceeded");
        auto oneByte = ScriptValueLimits{};
        oneByte.maximumBytes = 1;
        RequireErrorCode(ValidateScriptValue(ScriptValue{ScriptValue::Bytes{std::byte{0x01}, std::byte{0x02}}}, oneByte),
                         "script_value_capacity_exceeded");
    }

    TEST_CASE("Script value codecs reject malformed wire data and round-trip results", "[Extensions][ScriptValue]") {
        const auto structure = ScriptValue::Struct("com.example.request", {
                                                                              {"name", ScriptValue::String("horo")},
                                                                              {"note", ScriptValue::String("optional")},
                                                                          });
        const auto encodedStructure = EncodeScriptValue(structure);
        REQUIRE(encodedStructure.HasValue());
        REQUIRE(DecodeScriptValue(encodedStructure.Value()).HasValue());
        CHECK(DecodeScriptValue(encodedStructure.Value()).Value() == structure);

        const ScriptError error{
            .domain = "com.example",
            .code = "failed",
            .message = "bad",
            .retryable = true,
            .cancelled = true,
            .details = {{.id = "reason", .value = ScriptValue::String("timeout")}},
        };
        const auto encodedSuccess = EncodeScriptCallResult(ScriptCallResult::Success({structure}));
        REQUIRE(encodedSuccess.HasValue());
        CHECK(DecodeScriptCallResult(encodedSuccess.Value()).Value() == ScriptCallResult::Success({structure}));
        const auto encodedFailure = EncodeScriptCallResult(ScriptCallResult::Failure(error));
        REQUIRE(encodedFailure.HasValue());
        CHECK(DecodeScriptCallResult(encodedFailure.Value()).Value() == ScriptCallResult::Failure(error));

        auto invalidHeader = encodedStructure.Value();
        invalidHeader[0] = std::byte{0};
        RequireErrorCode(DecodeScriptValue(invalidHeader), "script_value_encoding_invalid");
        auto trailing = encodedStructure.Value();
        trailing.push_back(std::byte{0});
        RequireErrorCode(DecodeScriptValue(trailing), "script_value_encoding_invalid");
        const std::vector<std::byte> truncatedValue{std::byte{0x53}, std::byte{0x56}, std::byte{0x01}};
        RequireErrorCode(DecodeScriptValue(truncatedValue), "script_value_encoding_invalid");
        const std::vector<std::byte> unknownTag{std::byte{0x53}, std::byte{0x56}, std::byte{0x01}, std::byte{0xff}};
        RequireErrorCode(DecodeScriptValue(unknownTag), "script_value_encoding_invalid");

        std::vector<std::byte> invalidUtf8{std::byte{0x53}, std::byte{0x56}, std::byte{0x01}, std::byte{6}};
        AppendU64(invalidUtf8, 1);
        invalidUtf8.push_back(std::byte{0xff});
        RequireErrorCode(DecodeScriptValue(invalidUtf8), "script_value_invalid");

        auto smallEncoded = ScriptValueLimits{};
        smallEncoded.maximumEncodedBytes = 3;
        RequireErrorCode(EncodeScriptValue(ScriptValue::Null(), smallEncoded), "script_value_capacity_exceeded");
        RequireErrorCode(DecodeScriptValue(encodedStructure.Value(), smallEncoded), "script_value_capacity_exceeded");
        auto smallRaw = ScriptValueLimits{};
        smallRaw.maximumBytes = 1;
        RequireErrorCode(DecodeScriptValue(encodedStructure.Value(), smallRaw), "script_value_capacity_exceeded");
        auto shallow = ScriptValueLimits{};
        shallow.maximumDepth = 0;
        RequireErrorCode(DecodeScriptValue(EncodeScriptValue(ScriptValue::Array({ScriptValue::Null()})).Value(), shallow),
                         "script_value_capacity_exceeded");

        auto invalidResultHeader = encodedSuccess.Value();
        invalidResultHeader[3] = std::byte{2};
        RequireErrorCode(DecodeScriptCallResult(invalidResultHeader), "script_value_encoding_invalid");
        auto resultTrailing = encodedSuccess.Value();
        resultTrailing.push_back(std::byte{0});
        RequireErrorCode(DecodeScriptCallResult(resultTrailing), "script_value_encoding_invalid");
        std::vector<std::byte> truncatedResult{std::byte{0x53}, std::byte{0x52}, std::byte{0x01}, std::byte{0}};
        AppendU64(truncatedResult, 1);
        RequireErrorCode(DecodeScriptCallResult(truncatedResult), "script_value_encoding_invalid");
        auto failedWithValues = ScriptCallResult::Failure(error);
        failedWithValues.values.push_back(ScriptValue::Null());
        RequireErrorCode(EncodeScriptCallResult(failedWithValues), "script_call_result_invalid");
    }

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

    TEST_CASE("Script invocation validates lifecycle inputs and terminal failure paths", "[Extensions][ScriptInvocation]") {
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

        auto controller = registry.Begin(contextRegistration, providerRegistration, Request(snapshot));
        REQUIRE(controller.HasValue());
        CHECK(controller.Value().ObserveCancellation().Value() == ScriptInvocationCancellationObservation::NotRequested);
        CHECK(controller.Value().PublishProgress({.phase = "", .completedUnits = 0, .totalUnits = 1}).ErrorValue().code.Value() ==
              "script_invocation_invalid");
        CHECK(controller.Value().PublishProgress({.phase = "work", .completedUnits = 2, .totalUnits = 1}).ErrorValue().code.Value() ==
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
        CHECK(ScriptInvocationSnapshot{.state = ScriptInvocationStateKind::Running}.IsTerminal() == false);
        CHECK(ScriptInvocationSnapshot{.state = ScriptInvocationStateKind::Failed}.IsTerminal());
        auto drained = registry.Drain(contextRegistration, 0);
        REQUIRE(drained.HasValue());
        CHECK(drained.Value().empty());
    }

    TEST_CASE("Script invocation admission, handle limits, and shutdown fail closed", "[Extensions][ScriptInvocation]") {
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

        ScriptInvocationRegistry otherRegistry;
        auto otherProvider = otherRegistry.RegisterProvider({.generation = 9});
        REQUIRE(otherProvider.HasValue());
        auto otherContext = otherRegistry.RegisterContext();
        REQUIRE(otherContext.HasValue());
        const auto snapshot = DescriptorSnapshot();
        RequireErrorCode(registry.Begin(otherContext.Value(), otherProvider.Value(), Request(snapshot)), "script_invocation_unavailable");
        RequireErrorCode(registry.IssueHandle(otherContext.Value(), otherProvider.Value(), "resource"), "script_invocation_unavailable");

        registry.BeginShutdown();
        CHECK(registry.IsShutdown());
        RequireErrorCode(registry.RegisterProvider({.generation = 10}), "script_invocation_shutdown");
        RequireErrorCode(registry.RegisterContext(), "script_invocation_shutdown");
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
