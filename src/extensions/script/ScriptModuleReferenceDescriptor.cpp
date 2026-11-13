#include "ScriptModuleReferenceService.h"

#include <array>

namespace Horo::Extensions {
    namespace {
        constexpr ScriptExportVersion Introduced{1, 0, 0};

        ScriptExportTypeReference Primitive(ScriptExportPrimitiveKind kind) {
            return {.primitive = kind};
        }

        ScriptExportTypeReference Named(std::string name) {
            return {.namedType = std::move(name)};
        }

        ScriptExportTypeDescriptor Mode() {
            return {.id = "horo.reference.mode",
                    .kind = ScriptExportNamedTypeKind::Enum,
                    .introducedVersion = Introduced,
                    .enumValues = {{.id = "off", .value = 0, .introducedVersion = Introduced},
                                   {.id = "on", .value = 1, .introducedVersion = Introduced}}};
        }

        ScriptExportTypeDescriptor Names() {
            return {.id = "horo.reference.names",
                    .kind = ScriptExportNamedTypeKind::Array,
                    .introducedVersion = Introduced,
                    .elementType = Primitive(ScriptExportPrimitiveKind::String),
                    .maximumElements = 64};
        }

        ScriptExportTypeDescriptor Scores() {
            return {.id = "horo.reference.scores",
                    .kind = ScriptExportNamedTypeKind::Map,
                    .introducedVersion = Introduced,
                    .keyType = Primitive(ScriptExportPrimitiveKind::String),
                    .valueType = Primitive(ScriptExportPrimitiveKind::Number),
                    .maximumElements = 64};
        }

        ScriptExportFieldDescriptor Field(std::string id, ScriptExportTypeReference type) {
            return {.id = std::move(id), .type = std::move(type), .introducedVersion = Introduced};
        }

        ScriptExportTypeDescriptor Record() {
            using enum Horo::Extensions::ScriptExportPrimitiveKind;
            auto handle = Primitive(Handle);
            handle.nullability = ScriptExportNullability::Nullable;
            return {.id = "horo.reference.record",
                    .kind = ScriptExportNamedTypeKind::Struct,
                    .introducedVersion = Introduced,
                    .fields = {Field("id", Primitive(UnsignedInteger)), Field("name", Primitive(String)),
                               Field("payload", Primitive(Bytes)), Field("active", Primitive(Boolean)), Field("ratio", Primitive(Number)),
                               Field("names", Named("horo.reference.names")), Field("scores", Named("horo.reference.scores")),
                               Field("mode", Named("horo.reference.mode")), Field("token", std::move(handle))}};
        }

        ScriptExportParameterDescriptor Parameter(std::string id, ScriptExportTypeReference type) {
            return {.id = std::move(id), .type = std::move(type), .introducedVersion = Introduced};
        }

        ScriptExportResultDescriptor Output(std::string id, ScriptExportTypeReference type) {
            return {.id = std::move(id), .type = std::move(type), .introducedVersion = Introduced};
        }

        std::vector<ScriptExportFunctionDescriptor> Methods() {
            using P = ScriptExportPrimitiveKind;
            using M = ScriptExportInvocationMode;
            return {{.id = "echo",
                     .introducedVersion = Introduced,
                     .invocation = M::Synchronous,
                     .parameters = {Parameter("record", Named("horo.reference.record"))},
                     .results = {Output("echoed", Named("horo.reference.record"))}},
                    {.id = "digest",
                     .introducedVersion = Introduced,
                     .invocation = M::Asynchronous,
                     .parameters = {Parameter("bytes", Primitive(P::Bytes))},
                     .results = {Output("hash", Primitive(P::UnsignedInteger))}},
                    {.id = "sum",
                     .introducedVersion = Introduced,
                     .invocation = M::Synchronous,
                     .parameters = {Parameter("a", Primitive(P::SignedInteger)), Parameter("b", Primitive(P::SignedInteger))},
                     .results = {Output("sum", Primitive(P::SignedInteger))},
                     .errorIds = {"horo.reference.range"}},
                    {.id = "reject",
                     .introducedVersion = Introduced,
                     .invocation = M::Synchronous,
                     .parameters = {Parameter("message", Primitive(P::String))},
                     .errorIds = {"horo.reference.rejected"}}};
        }
    }  // namespace

    Result<ScriptExportDescriptorSnapshotPtr> BuildScriptModuleReferenceApi(std::string moduleId, ScriptExportVersion version) {
        ScriptExportDescriptor api{.moduleId = std::move(moduleId),
                                   .id = "horo.reference.api",
                                   .nameSpace = "horo.reference",
                                   .version = version,
                                   .compatibility = {{1, 0, 0}, version},
                                   .service = {{"horo.script.reference"}, "horo.script.reference", "horo.script.reference.v1", {1, 0, 0}},
                                   .types = {Mode(), Names(), Scores(), Record()},
                                   .errors = {{.id = "horo.reference.range", .introducedVersion = Introduced},
                                              {.id = "horo.reference.rejected", .introducedVersion = Introduced}},
                                   .functions = Methods()};
        auto defaultValue = EncodeScriptValue(ScriptValue{std::int64_t{1}});
        if (defaultValue.HasError())
            return Result<ScriptExportDescriptorSnapshotPtr>::Failure(defaultValue.ErrorValue());
        auto &parameter = api.functions[2].parameters[1];
        parameter.requirement = ScriptExportParameterRequirement::Optional;
        parameter.canonicalDefault = std::move(defaultValue).Value();
        auto constant = EncodeScriptValue(ScriptValue{ScriptEnumValue{"horo.reference.mode", 1}});
        if (constant.HasError())
            return Result<ScriptExportDescriptorSnapshotPtr>::Failure(constant.ErrorValue());
        api.constants.push_back({.id = "on",
                                 .type = Named("horo.reference.mode"),
                                 .introducedVersion = Introduced,
                                 .canonicalValue = std::move(constant).Value()});
        if (version == ScriptExportVersion{1, 1, 0})
            api.functions.push_back(
                {.id = "apirevision",
                 .introducedVersion = {1, 1, 0},
                 .invocation = ScriptExportInvocationMode::Synchronous,
                 .results = {{.id = "revision", .type = Primitive(ScriptExportPrimitiveKind::String), .introducedVersion = {1, 1, 0}}}});
        return BuildScriptExportDescriptorSnapshot(std::array{api});
    }
}  // namespace Horo::Extensions
