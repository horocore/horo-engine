#include "ShaderReflectionCook.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
    using namespace Horo::Render;

    /** @brief Scripted parser evidence only; these sources do not establish native compiler or GPU qualification. */
    struct Evidence final {
        ShaderTargetRequirement target{ShaderTargetBackend::OpenGL, ShaderPayloadFormat::Glsl410, 1, 1, 8, 0};
        ShaderManifest manifest{.schemaVersion = 1,
                                .sourceIdentity = "tests/variable-registers.hlsl",
                                .sourceRevision = 7,
                                .entryPoints = {{ShaderStage::Vertex, "VertexEntry"}, {ShaderStage::Fragment, "PixelEntry"}},
                                .bindings = {{{1},
                                              ShaderResourceKind::UniformBuffer,
                                              ShaderResourceAccess::ReadOnly,
                                              1,
                                              ShaderStageVisibility::Vertex}},
                                .parameters = {{{1}, {1}, ShaderValueType::Float32, 4, 4, 1},
                                               {{2}, {1}, ShaderValueType::Float32, 1, 4, 2}},
                                .targets = {target}};
        std::vector<ShaderReflectionBindingName> names{{{1}, "type.Camera", {{{1}, "Projection"}, {{2}, "Planes"}}}};
        std::array<SpirvReflectionStage, 2> stages{
            {{ShaderStage::Vertex, "VertexEntry", R"({"entryPoints":[{"name":"VertexEntry","mode":"vert"}],
              "types":{"_block":{"members":[{"name":"Projection","type":"mat4","offset":0,"matrix_stride":16},
               {"name":"Planes","type":"vec4","offset":64,"array":[2],"array_size_is_literal":[true],"array_stride":16}]}},
              "ubos":[{"name":"type.Camera","type":"_block","set":0,"binding":7,"block_size":96}],
              "inputs":[{"name":"Position","type":"vec4","location":3}],
              "outputs":[{"name":"UV","type":"vec2","location":5}]})"},
             {ShaderStage::Fragment, "PixelEntry", R"({"entryPoints":[{"name":"PixelEntry","mode":"frag"}],
              "inputs":[{"name":"UV","type":"vec2","location":5}],
              "outputs":[{"name":"Color","type":"vec4","location":0}]})"}}};
        std::array<std::string, 2> translations{
            {"#version 410\nlayout(std140) uniform NativeCamera { mat4 Projection; vec4 Planes[2]; } camera;\n"
             "layout(location=3) in vec4 actualPosition; out vec2 horo_raster_varying_5; void main() {}",
             "#version 410\nin vec2 horo_raster_varying_5; layout(location=0) out vec4 actualColor; void main() {}"}};

        auto Normalize() const {
            return NormalizeRegisterRasterReflection(manifest, target, stages, translations, names);
        }

        void Metal() {
            target.backend = ShaderTargetBackend::Metal;
            target.payloadFormat = ShaderPayloadFormat::MetalLibrary24;
            manifest.targets = {target};
            translations = {{"struct Camera { float4x4 Projection; float4 Planes[2]; };"
                             "struct VertexIn { float4 actualPosition [[attribute(3)]]; };"
                             "struct VertexOut { float4 position [[position]]; float2 uv [[user(locn5)]]; };"
                             "vertex VertexOut VertexEntry(VertexIn input [[stage_in]], constant Camera& camera [[buffer(9)]]) {}",
                             "struct FragmentIn { float2 uv [[user(locn5)]]; };"
                             "struct FragmentOut { float4 color [[color(0)]]; };"
                             "fragment FragmentOut PixelEntry(FragmentIn input [[stage_in]]) {}"}};
        }
    };

    void Replace(std::string &text, std::string_view before, std::string_view after) {
        const auto found = text.find(before);
        REQUIRE(found != text.npos);
        text.replace(found, before.size(), after);
    }

    void Sampled(Evidence &input) {
        input.manifest.bindings.push_back(
            {{2}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment});
        input.manifest.bindings.push_back(
            {{3}, ShaderResourceKind::Sampler, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment});
        input.names.push_back({{2}, "Albedo", {}});
        input.names.push_back({{3}, "Filtering", {}});
        input.stages[1].json.pop_back();
        input.stages[1].json += R"(,"separate_images":[{"name":"Albedo","type":"texture2D","set":0,"binding":2}],
            "separate_samplers":[{"name":"Filtering","type":"sampler","set":0,"binding":3}]})";
        input.translations[1] += " uniform sampler2D actualCombined;";
    }
}  // namespace

TEST_CASE("Generic register reflection proves variable native packing and actual lookup names",
          "[renderer][shader-cook][register-raster]") {
    Evidence input;
    const bool metal = GENERATE(false, true);
    if (metal)
        input.Metal();
    const auto result = input.Normalize();
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().parameters.size() == 2);
    CHECK(result.Value().parameters[1].byteOffset == 64);
    CHECK(result.Value().parameters[1].arrayCount == 2);
    CHECK(result.Value().parameters[1].arrayStride == 16);
    CHECK(result.Value().targetBindings[0].nativeBinding == (metal ? 9 : 0));
    CHECK(result.Value().targetBindings[0].nativeName == (metal ? "camera" : "NativeCamera"));
    CHECK(result.Value().stageInterface.size() == 4);
}

TEST_CASE("Generic register reflection pairs the actual combined GL sampler", "[renderer][shader-cook][register-raster]") {
    Evidence input;
    Sampled(input);
    const auto result = input.Normalize();
    REQUIRE(result.HasValue());
    CHECK(result.Value().targetBindings[1].pairedSampler == ShaderBindingId{3});
    CHECK(result.Value().targetBindings[1].nativeName == "actualCombined");
    CHECK(result.Value().targetBindings[2].nativeName == "actualCombined");
    CHECK(result.Value().targetBindings[1].nativeBinding == 0);
    CHECK(result.Value().targetBindings[2].nativeBinding == 0);
}

TEST_CASE("Generic register reflection rejects mismatched actual member evidence", "[renderer][shader-cook][register-raster]") {
    using Mutation = void (*)(Evidence &);
    const std::array<Mutation, 7> mutations{{+[](Evidence &input) {
        Replace(input.stages[0].json, "\"offset\":64", "\"offset\":80");
    }, +[](Evidence &input) {
        Replace(input.stages[0].json, "\"matrix_stride\":16", "\"matrix_stride\":16,\"row_major\":true");
    }, +[](Evidence &input) {
        Replace(input.stages[0].json, "\"block_size\":96", "\"block_size\":112");
    }, +[](Evidence &input) {
        Replace(input.stages[0].json, "\"array_stride\":16", "\"array_stride\":32");
    }, +[](Evidence &input) {
        Replace(input.translations[0], "Planes[2]", "Planes[3]");
    }, +[](Evidence &input) {
        Replace(input.translations[0], "mat4 Projection", "vec4 Projection");
    }, +[](Evidence &input) {
        Replace(input.stages[0].json, "\"array_size_is_literal\":[true]", "\"array_size_is_literal\":[false]");
    }}};
    Evidence input;
    mutations[GENERATE(0U, 1U, 2U, 3U, 4U, 5U, 6U)](input);
    CHECK(input.Normalize().HasError());
}

TEST_CASE("Generic register reflection rejects commented duplicate and unrelated declarations",
          "[renderer][shader-cook][register-raster]") {
    Evidence input;
    const auto mutation = GENERATE(0, 1, 2, 3);
    if (mutation == 0)
        Replace(input.translations[0], "layout(std140) uniform NativeCamera { mat4 Projection; vec4 Planes[2]; } camera;",
                "/* layout(std140) uniform NativeCamera { mat4 Projection; vec4 Planes[2]; } camera; */");
    if (mutation == 1)
        input.translations[0] += " layout(std140) uniform Another { mat4 Projection; vec4 Planes[2]; };";
    if (mutation == 2)
        Replace(input.stages[0].json, "\"offset\":64", "\"offset\":64,\"offset\":64");
    if (mutation == 3) {
        input.Metal();
        Replace(input.translations[0], "VertexEntry(", "UnrelatedHelper(");
    }
    CHECK(input.Normalize().HasError());
}

TEST_CASE("Generic register reflection rejects stage entries interface mismatch and missing resource pair",
          "[renderer][shader-cook][register-raster]") {
    Evidence input;
    const auto mutation = GENERATE(0, 1, 2, 3);
    if (mutation == 0)
        input.stages[0].entryPoint = "OtherEntry";
    if (mutation == 1)
        Replace(input.stages[1].json, "\"mode\":\"frag\"", "\"mode\":\"vert\"");
    if (mutation == 2)
        Replace(input.translations[1], "horo_raster_varying_5", "horo_raster_varying_6");
    if (mutation == 3)
        Sampled(input);  // Cross claims a pair, but the actual translation no longer declares it.
    if (mutation == 3)
        Replace(input.translations[1], "uniform sampler2D actualCombined;", "");
    CHECK(input.Normalize().HasError());
}

TEST_CASE("Generic register reflection bounds translator documents before parsing", "[renderer][shader-cook][register-raster]") {
    Evidence input;
    input.stages[0].json.assign(1024U * 1024U + 1, ' ');
    CHECK(input.Normalize().HasError());
}

TEST_CASE("Generic raster renaming uses sorted actual compiler varying locations", "[renderer][shader-cook][register-raster]") {
    Evidence input;
    Replace(input.stages[0].json, "\"outputs\":[", "\"outputs\":[{\"name\":\"Extra\",\"type\":\"vec4\",\"location\":9},");
    const auto locations = RegisterRasterVaryingLocations(input.stages[0]);
    REQUIRE(locations.HasValue());
    CHECK(locations.Value() == std::vector<std::uint32_t>{5, 9});
    const auto fragment = RegisterRasterVaryingLocations(input.stages[1]);
    REQUIRE(fragment.HasValue());
    CHECK(fragment.Value() == std::vector<std::uint32_t>{5});
}

TEST_CASE("Generic raster renaming rejects duplicate foreign or unsupported varying evidence", "[renderer][shader-cook][register-raster]") {
    Evidence input;
    const auto mutation = GENERATE(0, 1, 2, 3, 4);
    if (mutation == 0)
        input.stages[0].entryPoint = "ForeignEntry";
    if (mutation == 1)
        Replace(input.stages[0].json, "\"outputs\":[", "\"outputs\":[{\"name\":\"Duplicate\",\"type\":\"vec2\",\"location\":5},");
    if (mutation == 2)
        Replace(input.stages[0].json, "\"name\":\"UV\"", "\"name\":\"UV\",\"array\":[1]");
    if (mutation == 3)
        Replace(input.stages[0].json, "\"name\":\"UV\",\"type\":\"vec2\"", "\"name\":\"UV\",\"type\":\"ivec2\"");
    if (mutation == 4)
        Replace(input.stages[0].json, "\"location\":5", "\"location\":256");
    CHECK(RegisterRasterVaryingLocations(input.stages[0]).HasError());
}
