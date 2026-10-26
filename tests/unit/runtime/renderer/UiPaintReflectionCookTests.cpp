#include "Horo/Runtime/Render/UiPaintKernel.h"
#include "ShaderReflectionCook.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>

namespace {
    using namespace Horo::Render;
    constexpr std::string_view Uniform = R"("types":{"_1":{"members":[
        {"name":"Extent","type":"vec2","offset":0},{"name":"Opacity","type":"float","offset":8},
        {"name":"Sampling","type":"uint","offset":12},{"name":"Flags","type":"uint","offset":16},
        {"name":"Reserved0","type":"uint","offset":20},{"name":"Reserved1","type":"uint","offset":24},
        {"name":"Reserved2","type":"uint","offset":28},{"name":"MaskExtent","type":"vec2","offset":32},
        {"name":"MaskReserved","type":"uvec2","offset":40},{"name":"CornerRadii","type":"vec4","offset":48}]}},
        "ubos":[{"type":"_1","name":"type.PaintParameters","block_size":64,"set":0,"binding":0}])";
    constexpr std::string_view Struct = R"(struct type_PaintParameters {
        float2 Extent; float Opacity; uint Sampling; uint Flags; uint Reserved0; uint Reserved1; uint Reserved2;
        float2 MaskExtent; uint2 MaskReserved; float4 CornerRadii; };)";
    constexpr std::string_view Varyings = R"([{"name":"uv","type":"vec2","location":0},
        {"name":"color","type":"vec4","location":1},{"name":"coverageUv","type":"vec2","location":2}])";

    /** @brief Scripted official-schema evidence tests strict normalization only; it does not qualify native compiler output. */
    struct Evidence {
        ShaderTargetRequirement target{ShaderTargetBackend::Metal, ShaderPayloadFormat::MetalLibrary24, 1, 1, 4, 0, false, false};
        ShaderManifest manifest{MakeUiPaintShaderManifest(target)};
        std::array<SpirvReflectionStage, 2> stages{
            {{ShaderStage::Vertex, "PaintVertex",
              "{\"entryPoints\":[{\"name\":\"PaintVertex\",\"mode\":\"vert\"}]," + std::string{Uniform} +
                  R"(,"inputs":[{"name":"position","type":"vec2","location":0},{"name":"uv","type":"vec2","location":1},
                {"name":"color","type":"vec4","location":2}],"outputs":)" +
                  std::string{Varyings} + "}"},
             {ShaderStage::Fragment, "PaintFragment",
              "{\"entryPoints\":[{\"name\":\"PaintFragment\",\"mode\":\"frag\"}]," + std::string{Uniform} +
                  ",\"inputs\":" + std::string{Varyings} + R"(,"outputs":[{"name":"color","type":"vec4","location":0}],
              "separate_images":[{"name":"PaintTexture","type":"texture2D","set":0,"binding":1},
              {"name":"CoverageTexture","type":"texture2D","set":0,"binding":2}],
              "separate_samplers":[{"name":"PaintSampler","type":"sampler","set":0,"binding":3}]})"}}};
        std::array<std::string, 2>
            translations{std::string{Struct} + " vertex void PaintVertex(constant type_PaintParameters& p [[buffer(7)]]) {}",
                         std::string{Struct} +
                             " fragment void PaintFragment(constant type_PaintParameters& p [[buffer(7)]], "
                             "texture2d<float> PaintTexture [[texture(4)]], texture2d<float> CoverageTexture [[texture(8)]], "
                             "sampler PaintSampler [[sampler(2)]]) {}"};
    };

    void Replace(std::string &value, std::string_view before, std::string_view after) {
        const auto at = value.find(before);
        REQUIRE(at != value.npos);
        value.replace(at, before.size(), after);
    }
}  // namespace

TEST_CASE("UI cook takes nondefault native bindings from actual translated evidence", "[renderer][shader-cook][ui]") {
    Evidence evidence;
    const auto result = NormalizeUiPaintMetalReflection(evidence.manifest, evidence.target, evidence.stages, evidence.translations);
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().targetBindings.size() == 4);
    CHECK(result.Value().targetBindings[0].nativeBinding == 7);
    CHECK(result.Value().targetBindings[1].nativeBinding == 4);
    CHECK(result.Value().targetBindings[2].nativeBinding == 8);
    CHECK(result.Value().targetBindings[3].nativeBinding == 2);
    CHECK(result.Value().targetBindings[1].pairedSampler == ShaderBindingId{4});
}

TEST_CASE("UI cook rejects malformed ambiguous or mismatching intermediate and translated layout", "[renderer][shader-cook][ui]") {
    Evidence evidence;
    SECTION("Uniform member offset") {
        Replace(evidence.stages[1].json, "\"offset\":48", "\"offset\":64");
    }
    SECTION("Uniform member type") {
        Replace(evidence.stages[1].json, "\"type\":\"uvec2\"", "\"type\":\"vec2\"");
    }
    SECTION("Translated member packing") {
        Replace(evidence.translations[1], "uint2 MaskReserved", "uint MaskReserved");
    }
    SECTION("Different vertex buffer") {
        Replace(evidence.translations[0], "buffer(7)", "buffer(6)");
    }
    SECTION("Native texture collision") {
        Replace(evidence.translations[1], "texture(8)", "texture(4)");
    }
    SECTION("Duplicate native argument") {
        evidence.translations[1] += " sampler PaintSampler [[sampler(2)]]";
    }
    SECTION("Comments cannot supply evidence") {
        Replace(evidence.translations[1], "[[texture(8)]]", "/* [[texture(8)]] */");
    }
    SECTION("Signed binding") {
        Replace(evidence.stages[1].json, "\"binding\":2", "\"binding\":-2");
    }
    SECTION("Foreign stage") {
        evidence.stages[0].stage = ShaderStage::Compute;
    }
    SECTION("Wrong stage entry") {
        evidence.stages[1].entryPoint = "Other";
    }
    SECTION("Missing linked interface") {
        Replace(evidence.stages[1].json, "\"location\":2", "\"location\":3");
    }
    SECTION("Duplicate key") {
        evidence.stages[0].json.insert(1, "\"ubos\":[],");
    }
    SECTION("Trailing value") {
        evidence.stages[1].json += "{}";
    }
    SECTION("Oversized translation") {
        evidence.translations[0].resize(1024U * 1024U + 1, ' ');
    }
    CHECK(NormalizeUiPaintMetalReflection(evidence.manifest, evidence.target, evidence.stages, evidence.translations).HasError());
}
