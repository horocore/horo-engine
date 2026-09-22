#pragma once

#include "Horo/Extensions/ScriptInvocation.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Horo::Extensions::Tests::Support {
    inline ScriptExportTypeReference Primitive(ScriptExportPrimitiveKind kind,
                                               ScriptExportNullability nullability = ScriptExportNullability::NonNull) {
        return {.primitive = kind, .nullability = nullability};
    }

    inline ScriptExportTypeReference Named(std::string id, ScriptExportNullability nullability = ScriptExportNullability::NonNull) {
        return {.primitive = ScriptExportPrimitiveKind::Count, .namedType = std::move(id), .nullability = nullability};
    }

    inline ScriptExportTypeDescriptor MakeRequestType() {
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

    inline ScriptExportFunctionDescriptor MakeFetchFunction() {
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

    inline ScriptExportDescriptor MakeDescriptor() {
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

    inline ScriptExportDescriptorSnapshotPtr DescriptorSnapshot() {
        const auto descriptor = MakeDescriptor();
        auto snapshot = BuildScriptExportDescriptorSnapshot(std::array{descriptor});
        REQUIRE(snapshot.HasValue());
        return snapshot.Value();
    }

    inline ScriptInvocationRequest Request(const ScriptExportDescriptorSnapshotPtr &snapshot) {
        return ScriptInvocationRequest{
            .descriptors = snapshot,
            .apiId = "com.example.api",
            .functionId = "fetch",
            .arguments = {ScriptValue::Struct("com.example.request", {{"name", ScriptValue::String("horo")}})},
        };
    }

    inline void RequireErrorCode(const auto &result, std::string_view code) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == code);
    }

    inline void AppendU64(std::vector<std::byte> &bytes, std::uint64_t value) {
        for (std::size_t index = 0; index < sizeof(value); ++index)
            bytes.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
    }

    inline void AppendWireString(std::vector<std::byte> &bytes, std::string_view value) {
        bytes.push_back(std::byte{6});
        AppendU64(bytes, value.size());
        for (const auto character : value)
            bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }

    inline void AppendWireStringPayload(std::vector<std::byte> &bytes, std::string_view value) {
        AppendU64(bytes, value.size());
        for (const auto character : value)
            bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }

    inline ScriptExportDescriptor MakeTypedDescriptor() {
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
}  // namespace Horo::Extensions::Tests::Support
