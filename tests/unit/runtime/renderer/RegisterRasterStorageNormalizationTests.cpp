#include "RegisterRasterStorageTestSupport.h"

#include <catch2/generators/catch_generators.hpp>

using namespace Horo::Render;
using namespace Horo::Render::RegisterRasterStorageTests;

TEST_CASE("Readonly raster storage normalizes actual pointer or single-array-wrapper evidence against the packed light ABI",
          "[renderer][shader-cook][storage]") {
    Evidence evidence;
    if (GENERATE(false, true))
        evidence.Wrappers();
    const auto normalized = evidence.Normalize();
    REQUIRE(normalized.HasValue());
    REQUIRE(normalized.Value().targetBindings.size() == 4);
    REQUIRE(normalized.Value().parameters.size() == 13);
    CHECK(normalized.Value().targetBindings[1].nativeBinding == 3);
    CHECK(normalized.Value().targetBindings[2].nativeBinding == 4);
    CHECK(normalized.Value().targetBindings[3].nativeBinding == 5);
    CHECK(normalized.Value().parameters[2].byteOffset == 16);
    CHECK(normalized.Value().parameters[3].byteOffset == 28);
    CHECK(normalized.Value().parameters[7].byteOffset == 60);
    CHECK(normalized.Value().parameters.back().byteOffset == 0);
    CHECK(normalized.Value().parameters.back().arrayStride == 0);
}

TEST_CASE("Readonly raster storage rejects unknown or mutable actual element and access evidence", "[renderer][shader-cook][storage]") {
    using Mutation = void (*)(Evidence &);
    constexpr std::array<Mutation, 8> mutations{{+[](Evidence &e) {
        Replace(e.stages[1].json, "\"readonly\":true", "\"readonly\":false");
    }, +[](Evidence &e) {
        Replace(e.stages[1].json, "\"array_stride\":64", "\"array_stride\":80");
    }, +[](Evidence &e) {
        Replace(e.stages[1].json, "\"offset\":28", "\"offset\":32");
    }, +[](Evidence &e) {
        Replace(e.translations[1], "packed_float3 direction", "float3 direction");
    }, +[](Evidence &e) {
        Replace(e.translations[1], "const device PackedLight*", "device PackedLight*");
    }, +[](Evidence &e) {
        Replace(e.translations[1], "uint kind;", "uint kind[1];");
    }, +[](Evidence &e) {
        Replace(e.stages[1].json, "\"array\":[0]", "\"array\":[1]");
    }, +[](Evidence &e) {
        Replace(e.stages[1].json, "\"type\":\"vec3\"", "\"type\":\"ivec3\"");
    }}};
    Evidence evidence;
    mutations[GENERATE(0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U)](evidence);
    CHECK(evidence.Normalize().HasError());
}

TEST_CASE("Readonly raster storage rejects native and reflected aliases and foreign logical members", "[renderer][shader-cook][storage]") {
    using Mutation = void (*)(Evidence &);
    constexpr std::array<Mutation, 7> mutations{{+[](Evidence &e) {
        Replace(e.translations[1], "ClusterMembership [[buffer(4)]]", "ClusterMembership [[buffer(3)]]");
    }, +[](Evidence &e) {
        Replace(e.translations[1], "ClusterReferences [[buffer(5)]]", "ClusterReferences [[buffer(0)]]");
    }, +[](Evidence &e) {
        Replace(e.stages[1].json, "\"binding\":4", "\"binding\":3");
    }, +[](Evidence &e) {
        e.names[2].parameters[0].name = "count";
        e.names[2].parameters[1].name = "offset";
    }, +[](Evidence &e) {
        Replace(e.translations[1], "ClusterReferences [[buffer(5)]]", "OtherReferences [[buffer(5)]]");
    }, +[](Evidence &e) {
        e.Wrappers();
        Replace(e.translations[1], "uint _m0[1];", "uint _m0[1]; uint extra;");
    }, +[](Evidence &e) {
        e.names[3].parameters[0].name = "inventedValue";
    }}};
    Evidence evidence;
    mutations[GENERATE(0U, 1U, 2U, 3U, 4U, 5U, 6U)](evidence);
    CHECK(evidence.Normalize().HasError());
}

TEST_CASE("Readonly raster storage merges exact stage visibility and rejects mismatched cross-stage placement",
          "[renderer][shader-cook][storage]") {
    Evidence evidence;
    evidence.BothStages();
    const auto mutation = GENERATE(0, 1, 2);
    if (mutation == 1)
        Replace(evidence.translations[0], "ClusterLights [[buffer(3)]]", "ClusterLights [[buffer(7)]]");
    if (mutation == 2)
        evidence.manifest.bindings[1].stages = ShaderStageVisibility::Fragment;
    const auto normalized = evidence.Normalize();
    CHECK(normalized.HasValue() == (mutation == 0));
    if (normalized.HasValue())
        CHECK(normalized.Value().bindings[1].stages == (ShaderStageVisibility::Vertex | ShaderStageVisibility::Fragment));
}

TEST_CASE("Readonly raster storage is bounded to admitted shapes and Metal target support", "[renderer][shader-cook][storage]") {
    Evidence evidence;
    const auto mutation = GENERATE(0, 1, 2, 3);
    if (mutation == 0)
        evidence.manifest.bindings[1].access = ShaderResourceAccess::ReadWrite;
    if (mutation == 1)
        evidence.manifest.parameters[2].columns = 2;
    if (mutation == 2) {
        evidence.target.backend = ShaderTargetBackend::OpenGL;
        evidence.target.payloadFormat = ShaderPayloadFormat::Glsl410;
        evidence.manifest.targets = {evidence.target};
    }
    if (mutation == 3)
        evidence.manifest.bindings[1].arrayCount = 2;
    CHECK(evidence.Normalize().HasError());
}

TEST_CASE("Readonly raster storage requires selected native entry declarations rather than comments or helpers",
          "[renderer][shader-cook][storage]") {
    Evidence evidence;
    const auto mutation = GENERATE(0, 1);
    if (mutation == 0)
        Replace(evidence.translations[1], "fragment void PSMain(", "/* fragment void PSMain(");
    else
        Replace(evidence.translations[1], "fragment void PSMain(", "fragment void Unrelated(");
    CHECK(evidence.Normalize().HasError());
}

TEST_CASE("Readonly raster storage admits at most four actual buffer declarations", "[renderer][shader-cook][storage]") {
    Evidence evidence;
    evidence.AdditionalReference("AdditionalReferences", 7, 130, 6);
    const bool fifth = GENERATE(false, true);
    if (fifth)
        evidence.AdditionalReference("FifthReferences", 8, 140, 7);
    CHECK(evidence.Normalize().HasValue() == !fifth);
}

TEST_CASE("Readonly raster storage preserves uniform requirements and rejects contradictory runtime member access",
          "[renderer][shader-cook][storage]") {
    Evidence evidence;
    const auto mutation = GENERATE(0, 1, 2, 3);
    if (mutation == 0) {
        evidence.manifest.parameters.erase(evidence.manifest.parameters.begin());
        evidence.names[0].parameters.clear();
    }
    if (mutation == 1)
        Replace(evidence.stages[1].json, "\"name\":\"_m0\",\"type\":\"_light\"", "\"name\":\"injected expression\",\"type\":\"_light\"");
    if (mutation == 2)
        Replace(evidence.stages[1].json, "\"name\":\"_m0\",\"type\":\"_light\"", "\"writeonly\":true,\"name\":\"_m0\",\"type\":\"_light\"");
    if (mutation == 3)
        Replace(evidence.stages[1].json, "\"name\":\"_m0\",\"type\":\"_light\"", "\"readonly\":false,\"name\":\"_m0\",\"type\":\"_light\"");
    CHECK(evidence.Normalize().HasError());
}
