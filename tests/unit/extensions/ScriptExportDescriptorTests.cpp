#include "Horo/Extensions/ScriptExportDescriptor.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace Horo::Extensions::Tests {
    namespace {
        ScriptExportTypeReference Primitive(const ScriptExportPrimitiveKind kind,
                                            const ScriptExportNullability nullability = ScriptExportNullability::NonNull) {
            return ScriptExportTypeReference{.primitive = kind, .nullability = nullability};
        }

        ScriptExportTypeReference Named(const std::string_view id,
                                        const ScriptExportNullability nullability = ScriptExportNullability::NonNull) {
            return ScriptExportTypeReference{.namedType = std::string{id}, .nullability = nullability};
        }

        ScriptExportDocumentation Documentation(const std::string_view summary) {
            return ScriptExportDocumentation{.summary = std::string{summary}};
        }

        ScriptExportFieldDescriptor Field(const std::string_view id, ScriptExportTypeReference type,
                                          const ScriptExportParameterRequirement requirement = ScriptExportParameterRequirement::Required,
                                          std::optional<std::vector<std::byte>> canonicalDefault = std::nullopt) {
            return ScriptExportFieldDescriptor{
                .id = std::string{id},
                .type = std::move(type),
                .introducedVersion = {1, 0, 0},
                .requirement = requirement,
                .canonicalDefault = std::move(canonicalDefault),
            };
        }

        ScriptExportTypeDescriptor EnumType() {
            ScriptExportTypeDescriptor type{
                .id = "com.example.mode",
                .kind = ScriptExportNamedTypeKind::Enum,
                .introducedVersion = {1, 0, 0},
                .documentation = Documentation("Request mode"),
            };
            type.enumValues = {
                {.id = "read", .value = 0, .introducedVersion = {1, 0, 0}},
                {.id = "write", .value = 1, .introducedVersion = {1, 0, 0}},
            };
            return type;
        }

        ScriptExportTypeDescriptor RequestType() {
            ScriptExportTypeDescriptor type{
                .id = "com.example.request",
                .kind = ScriptExportNamedTypeKind::Struct,
                .introducedVersion = {1, 0, 0},
            };
            type.fields = {
                Field("path", Primitive(ScriptExportPrimitiveKind::String)),
                Field("mode", Named("com.example.mode")),
                Field("note", Primitive(ScriptExportPrimitiveKind::String, ScriptExportNullability::Nullable),
                      ScriptExportParameterRequirement::Optional, std::vector<std::byte>{std::byte{0}}),
            };
            return type;
        }

        ScriptExportTypeDescriptor PathsType() {
            return ScriptExportTypeDescriptor{
                .id = "com.example.paths",
                .kind = ScriptExportNamedTypeKind::Array,
                .introducedVersion = {1, 0, 0},
                .elementType = Primitive(ScriptExportPrimitiveKind::String),
                .maximumElements = 8,
            };
        }

        ScriptExportTypeDescriptor LabelsType() {
            return ScriptExportTypeDescriptor{
                .id = "com.example.labels",
                .kind = ScriptExportNamedTypeKind::Map,
                .introducedVersion = {1, 0, 0},
                .keyType = Primitive(ScriptExportPrimitiveKind::String),
                .valueType = Primitive(ScriptExportPrimitiveKind::String, ScriptExportNullability::Nullable),
                .maximumElements = 8,
            };
        }

        ScriptExportFunctionDescriptor FetchFunction() {
            ScriptExportFunctionDescriptor function{
                .id = "com.example.fetch",
                .introducedVersion = {1, 0, 0},
                .invocation = ScriptExportInvocationMode::Synchronous,
                .documentation = Documentation("Fetches one record"),
            };
            function.parameters = {{
                .id = "request",
                .type = Named("com.example.request"),
                .introducedVersion = {1, 0, 0},
            }};
            function.results = {{
                .id = "paths",
                .type = Named("com.example.paths"),
                .introducedVersion = {1, 0, 0},
            }};
            function.errorIds = {"com.example.not-found", "com.example.failed"};
            return function;
        }

        ScriptExportDescriptor ValidDescriptor() {
            ScriptExportDescriptor descriptor{
                .moduleId = "com.example.backend",
                .id = "com.example.records",
                .nameSpace = "com.example.records",
                .version = {1, 0, 0},
                .compatibility = {.minimum = {1, 0, 0}, .maximum = {1, 0, 0}},
                .service =
                    {
                        .capability = {"com.example.records"},
                        .serviceId = "com.example.records.service",
                        .contractId = "com.example.records.contract",
                        .minimumVersion = {1, 0, 0},
                    },
                .types = {EnumType(), RequestType(), PathsType(), LabelsType()},
                .constants = {{
                    .id = "com.example.default-path",
                    .type = Primitive(ScriptExportPrimitiveKind::String),
                    .introducedVersion = {1, 0, 0},
                    .canonicalValue = {std::byte{'/'}, std::byte{'\0'}},
                }},
                .errors =
                    {
                        {.id = "com.example.failed", .introducedVersion = {1, 0, 0}, .retryable = true},
                        {.id = "com.example.not-found", .introducedVersion = {1, 0, 0}},
                    },
                .functions = {FetchFunction()},
                .documentation = Documentation("Record service"),
            };
            return descriptor;
        }

        template <typename T> void RequireError(const Result<T> &result, const std::string_view code) {
            REQUIRE(result.HasError());
            CHECK(result.ErrorValue().code.Value() == code);
        }

        void AddOptionalFunction(ScriptExportDescriptor &descriptor) {
            descriptor.version = {1, 1, 0};
            descriptor.compatibility = {.minimum = {1, 0, 0}, .maximum = {1, 1, 0}};
            descriptor.functions.push_back(ScriptExportFunctionDescriptor{
                .id = "com.example.refresh",
                .introducedVersion = {1, 1, 0},
                .invocation = ScriptExportInvocationMode::Asynchronous,
            });
        }

        void ReverseDescriptorCollections(ScriptExportDescriptor &descriptor) {
            std::reverse(descriptor.types.begin(), descriptor.types.end());
            for (auto &type : descriptor.types) {
                std::reverse(type.fields.begin(), type.fields.end());
                std::reverse(type.enumValues.begin(), type.enumValues.end());
            }
            std::reverse(descriptor.constants.begin(), descriptor.constants.end());
            std::reverse(descriptor.errors.begin(), descriptor.errors.end());
            std::reverse(descriptor.functions.begin(), descriptor.functions.end());
            for (auto &function : descriptor.functions) {
                std::reverse(function.parameters.begin(), function.parameters.end());
                std::reverse(function.results.begin(), function.results.end());
                std::reverse(function.errorIds.begin(), function.errorIds.end());
            }
        }

        template <typename T>
        concept HasCallbackMember = requires(T value) { value.callback; };
    }  // namespace

    static_assert(!HasCallbackMember<ScriptExportDescriptor>);
    static_assert(std::is_same_v<decltype(ScriptExportDescriptor::functions), std::vector<ScriptExportFunctionDescriptor>>);

    TEST_CASE("Script export descriptors validate typed service APIs and nullability", "[unit][extensions][script-export]") {
        const ScriptExportDescriptor descriptor = ValidDescriptor();
        REQUIRE(ValidateScriptExportDescriptor(descriptor).HasValue());
        CHECK(descriptor.service.capability.value == "com.example.records");
        CHECK(descriptor.types[1].fields[2].type.nullability == ScriptExportNullability::Nullable);
        CHECK(descriptor.functions.front().invocation == ScriptExportInvocationMode::Synchronous);
    }

    TEST_CASE("Script export descriptors accept exact bounds and reject one-over values", "[unit][extensions][script-export]") {
        auto descriptor = ValidDescriptor();
        ScriptExportDescriptorLimits limits;
        limits.maximumFunctions = descriptor.functions.size();
        REQUIRE(ValidateScriptExportDescriptor(descriptor, limits).HasValue());

        AddOptionalFunction(descriptor);
        RequireError(ValidateScriptExportDescriptor(descriptor, limits), "script_export_descriptor_capacity_exceeded");

        descriptor = ValidDescriptor();
        limits = {};
        limits.maximumSymbols = 15;
        REQUIRE(ValidateScriptExportDescriptor(descriptor, limits).HasValue());
        --limits.maximumSymbols;
        RequireError(ValidateScriptExportDescriptor(descriptor, limits), "script_export_descriptor_capacity_exceeded");

        descriptor = ValidDescriptor();
        limits = {};
        for (auto &type : descriptor.types)
            type.documentation = {};
        descriptor.functions.front().documentation = {};
        limits.maximumDocumentationBytes = descriptor.documentation.summary.size();
        REQUIRE(ValidateScriptExportDescriptor(descriptor, limits).HasValue());
        --limits.maximumDocumentationBytes;
        RequireError(ValidateScriptExportDescriptor(descriptor, limits), "script_export_descriptor_capacity_exceeded");
    }

    TEST_CASE("Script export descriptors reject malformed references, duplicate identities, and cycles",
              "[unit][extensions][script-export]") {
        auto descriptor = ValidDescriptor();
        descriptor.functions.front().parameters.front().type =
            ScriptExportTypeReference{.primitive = ScriptExportPrimitiveKind::String, .namedType = "com.example.request"};
        RequireError(ValidateScriptExportDescriptor(descriptor), "script_export_descriptor_invalid");

        descriptor = ValidDescriptor();
        descriptor.functions.front().parameters.front().type = Named("com.example.missing");
        RequireError(ValidateScriptExportDescriptor(descriptor), "script_export_descriptor_invalid");

        descriptor = ValidDescriptor();
        descriptor.types[1].fields.push_back(descriptor.types[1].fields.front());
        RequireError(ValidateScriptExportDescriptor(descriptor), "script_export_descriptor_conflict");

        descriptor = ValidDescriptor();
        descriptor.types[1].fields.front().type = Named("com.example.request");
        RequireError(ValidateScriptExportDescriptor(descriptor), "script_export_descriptor_invalid");

        descriptor = ValidDescriptor();
        descriptor.types.back().keyType = Named("com.example.mode");
        RequireError(ValidateScriptExportDescriptor(descriptor), "script_export_descriptor_invalid");

        descriptor = ValidDescriptor();
        auto conflictingNamespace = ValidDescriptor();
        conflictingNamespace.id = "com.example.other";
        RequireError(BuildScriptExportDescriptorSnapshot(std::array{descriptor, conflictingNamespace}),
                     "script_export_descriptor_conflict");
    }

    TEST_CASE("Script export generations canonicalize order and preserve lifecycle generations", "[unit][extensions][script-export]") {
        auto descriptor = ValidDescriptor();
        const auto first = BuildScriptExportDescriptorSnapshot(std::array{descriptor});
        REQUIRE(first.HasValue());
        REQUIRE(first.Value()->Generation() == 1);
        REQUIRE(first.Value()->Find("com.example.records") != nullptr);

        auto reordered = descriptor;
        std::reverse(reordered.types.begin(), reordered.types.end());
        std::reverse(reordered.types[1].fields.begin(), reordered.types[1].fields.end());
        const auto equivalent = BuildScriptExportDescriptorSnapshot(std::array{reordered});
        REQUIRE(equivalent.HasValue());
        CHECK(first.Value()->Fingerprint() == equivalent.Value()->Fingerprint());

        AddOptionalFunction(descriptor);
        ReverseDescriptorCollections(descriptor);
        const auto replacement = BuildScriptExportDescriptorReplacement(first.Value(), std::array{descriptor});
        REQUIRE(replacement.HasValue());
        CHECK(replacement.Value()->Generation() == 2);
        CHECK(first.Value()->Generation() == 1);
        CHECK(replacement.Value()->Find("com.example.records")->functions.size() == 2);

        auto incompatible = descriptor;
        incompatible.nameSpace = "com.example.other";
        RequireError(BuildScriptExportDescriptorReplacement(first.Value(), std::array{incompatible}),
                     "script_export_descriptor_incompatible");
        CHECK(first.Value()->Generation() == 1);

        auto retired = ValidDescriptor();
        retired.version = {2, 0, 0};
        retired.compatibility = {.minimum = {2, 0, 0}, .maximum = {2, 0, 0}};
        retired.functions.clear();
        retired.tombstonedSymbols = {"com.example.fetch"};
        const auto majorReplacement = BuildScriptExportDescriptorReplacement(first.Value(), std::array{retired});
        REQUIRE(majorReplacement.HasValue());
        CHECK(majorReplacement.Value()->Generation() == 2);

        auto sameMajorRetirement = ValidDescriptor();
        sameMajorRetirement.version = {1, 1, 0};
        sameMajorRetirement.compatibility = {.minimum = {1, 0, 0}, .maximum = {1, 1, 0}};
        sameMajorRetirement.functions.clear();
        sameMajorRetirement.tombstonedSymbols = {"com.example.fetch"};
        RequireError(BuildScriptExportDescriptorReplacement(first.Value(), std::array{sameMajorRetirement}),
                     "script_export_descriptor_incompatible");
    }

    TEST_CASE("Script export optional member removal requires a major version", "[unit][extensions][script-export]") {
        auto previousDescriptor = ValidDescriptor();
        previousDescriptor.functions.front().parameters.push_back({
            .id = "options",
            .type = Named("com.example.labels"),
            .introducedVersion = {1, 0, 0},
            .requirement = ScriptExportParameterRequirement::Optional,
            .canonicalDefault = std::vector<std::byte>{std::byte{0}},
        });
        const auto previous = BuildScriptExportDescriptorSnapshot(std::array{previousDescriptor});
        REQUIRE(previous.HasValue());

        auto fieldRemoval = previousDescriptor;
        fieldRemoval.version = {1, 1, 0};
        fieldRemoval.compatibility = {.minimum = {1, 0, 0}, .maximum = {1, 1, 0}};
        fieldRemoval.types[1].fields.pop_back();
        fieldRemoval.tombstonedSymbols = {"com.example.request.note"};
        RequireError(BuildScriptExportDescriptorReplacement(previous.Value(), std::array{fieldRemoval}),
                     "script_export_descriptor_incompatible");

        auto parameterRemoval = previousDescriptor;
        parameterRemoval.version = {1, 1, 0};
        parameterRemoval.compatibility = {.minimum = {1, 0, 0}, .maximum = {1, 1, 0}};
        parameterRemoval.functions.front().parameters.pop_back();
        parameterRemoval.tombstonedSymbols = {"com.example.fetch.options"};
        RequireError(BuildScriptExportDescriptorReplacement(previous.Value(), std::array{parameterRemoval}),
                     "script_export_descriptor_incompatible");
    }

    TEST_CASE("Script export replacement rejects null and preserves the prior immutable generation", "[unit][extensions][script-export]") {
        const auto descriptor = ValidDescriptor();
        const auto candidate = std::array{descriptor};
        RequireError(BuildScriptExportDescriptorReplacement({}, candidate), "script_export_descriptor_invalid");

        const auto first = BuildScriptExportDescriptorSnapshot(candidate).Value();
        auto changed = descriptor;
        changed.version = {1, 1, 0};
        changed.compatibility = {.minimum = {1, 0, 0}, .maximum = {1, 1, 0}};
        changed.functions.front().invocation = ScriptExportInvocationMode::Asynchronous;
        RequireError(BuildScriptExportDescriptorReplacement(first, std::array{changed}), "script_export_descriptor_incompatible");
        CHECK(first->Generation() == 1);
        CHECK(first->Find("com.example.records")->functions.front().invocation == ScriptExportInvocationMode::Synchronous);
    }
}  // namespace Horo::Extensions::Tests
