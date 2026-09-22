#include "ScriptInvocationTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace Horo::Extensions::Tests {
    using namespace Support;

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

    TEST_CASE("Script value validation accepts and rejects named collection types", "[Extensions][ScriptValue]") {
        const auto descriptor = MakeTypedDescriptor();
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
    }

    TEST_CASE("Script value validation enforces arguments and error detail bounds", "[Extensions][ScriptValue]") {
        const auto descriptor = MakeDescriptor();
        const auto structValue = ScriptValue::Struct("com.example.request", {{"name", ScriptValue::String("horo")}});
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

        const ScriptError validError{
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
        const auto wrapped =
            WithCause(Error{.code = ErrorCode{"outer"}, .domain = ErrorDomainId{"com.example"}, .message = "outer"}, cause);
        const auto converted = MakeScriptError(wrapped, true);
        REQUIRE(converted.HasValue());
        CHECK(converted.Value().retryable);
        REQUIRE(converted.Value().details.size() == 1);
        CHECK(converted.Value().details.front().id == "cause.com.example");
        smallErrorLimits.maximumErrorDetails = 0;
        RequireErrorCode(MakeScriptError(wrapped, false, smallErrorLimits), "script_value_capacity_exceeded");
    }

    TEST_CASE("Script value validation rejects unsupported and oversized payloads", "[Extensions][ScriptValue]") {
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
        const ScriptValue malformedHandle{ScriptHandle{.context = {1}, .providerGeneration = 2, .value = 3, .generation = 0, .type = "x"}};
        RequireErrorCode(ValidateScriptValue(malformedHandle), "script_value_invalid");
    }

    TEST_CASE("Script value validation enforces finite work, element, and byte bounds", "[Extensions][ScriptValue]") {
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

    TEST_CASE("Script value codecs round-trip values and results", "[Extensions][ScriptValue]") {
        const auto structure =
            ScriptValue::Struct("com.example.request", {{"name", ScriptValue::String("horo")}, {"note", ScriptValue::String("optional")}});
        const auto encodedStructure = EncodeScriptValue(structure);
        REQUIRE(encodedStructure.HasValue());
        const auto decodedStructure = DecodeScriptValue(encodedStructure.Value());
        REQUIRE(decodedStructure.HasValue());
        CHECK(decodedStructure.Value() == structure);

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
    }

    TEST_CASE("Script value decoders reject malformed scalar and result wire data", "[Extensions][ScriptValue]") {
        const auto encoded = EncodeScriptValue(ScriptValue::String("horo"));
        REQUIRE(encoded.HasValue());
        auto invalidHeader = encoded.Value();
        invalidHeader[0] = std::byte{0};
        RequireErrorCode(DecodeScriptValue(invalidHeader), "script_value_encoding_invalid");
        auto trailing = encoded.Value();
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
        RequireErrorCode(DecodeScriptValue(encoded.Value(), smallEncoded), "script_value_capacity_exceeded");
        auto smallRaw = ScriptValueLimits{};
        smallRaw.maximumBytes = 1;
        const auto encodedStructure =
            EncodeScriptValue(ScriptValue::Struct("com.example.request", {{"name", ScriptValue::String("horo")}}));
        REQUIRE(encodedStructure.HasValue());
        RequireErrorCode(DecodeScriptValue(encodedStructure.Value(), smallRaw), "script_value_capacity_exceeded");
        auto shallow = ScriptValueLimits{};
        shallow.maximumDepth = 0;
        const auto encodedArray = EncodeScriptValue(ScriptValue::Array({ScriptValue::Null()}));
        REQUIRE(encodedArray.HasValue());
        RequireErrorCode(DecodeScriptValue(encodedArray.Value(), shallow), "script_value_capacity_exceeded");

        const auto encodedResult = EncodeScriptCallResult(ScriptCallResult::Success({ScriptValue::String("done")}));
        REQUIRE(encodedResult.HasValue());
        auto invalidResultHeader = encodedResult.Value();
        invalidResultHeader[3] = std::byte{2};
        RequireErrorCode(DecodeScriptCallResult(invalidResultHeader), "script_value_encoding_invalid");
        auto resultTrailing = encodedResult.Value();
        resultTrailing.push_back(std::byte{0});
        RequireErrorCode(DecodeScriptCallResult(resultTrailing), "script_value_encoding_invalid");
        std::vector<std::byte> truncatedResult{std::byte{0x53}, std::byte{0x52}, std::byte{0x01}, std::byte{0}};
        AppendU64(truncatedResult, 1);
        RequireErrorCode(DecodeScriptCallResult(truncatedResult), "script_value_encoding_invalid");
    }

    TEST_CASE("Script value decoders reject non-canonical collection wire data", "[Extensions][ScriptValue]") {
        std::vector<std::byte> nonScalarMapKey{std::byte{0x53}, std::byte{0x56}, std::byte{0x01}, std::byte{11}};
        AppendU64(nonScalarMapKey, 1);
        nonScalarMapKey.push_back(std::byte{10});
        AppendU64(nonScalarMapKey, 0);
        nonScalarMapKey.push_back(std::byte{0});
        RequireErrorCode(DecodeScriptValue(nonScalarMapKey), "script_value_encoding_invalid");

        std::vector<std::byte> outOfOrderMap{std::byte{0x53}, std::byte{0x56}, std::byte{0x01}, std::byte{11}};
        AppendU64(outOfOrderMap, 2);
        AppendWireString(outOfOrderMap, "b");
        outOfOrderMap.push_back(std::byte{0});
        AppendWireString(outOfOrderMap, "a");
        outOfOrderMap.push_back(std::byte{0});
        RequireErrorCode(DecodeScriptValue(outOfOrderMap), "script_value_encoding_invalid");

        std::vector<std::byte> outOfOrderStruct{std::byte{0x53}, std::byte{0x56}, std::byte{0x01}, std::byte{12}};
        AppendWireStringPayload(outOfOrderStruct, "com.example.request");
        AppendU64(outOfOrderStruct, 2);
        AppendWireStringPayload(outOfOrderStruct, "b");
        outOfOrderStruct.push_back(std::byte{0});
        AppendWireStringPayload(outOfOrderStruct, "a");
        outOfOrderStruct.push_back(std::byte{0});
        RequireErrorCode(DecodeScriptValue(outOfOrderStruct), "script_value_encoding_invalid");
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

    TEST_CASE("Script value type validation preserves nullability and result arity", "[Extensions][ScriptValue]") {
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
}  // namespace Horo::Extensions::Tests
