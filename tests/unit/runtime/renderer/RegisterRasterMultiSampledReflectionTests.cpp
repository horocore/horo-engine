#include "ColorOutputShaderRecipe.h"
#include "Horo/Runtime/Render/ShaderReflectionErrors.h"
#include "ShaderReflectionCook.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <utility>

#ifndef HORO_COLOR_OUTPUT_REFLECTION_FIXTURE_PATH
#error "Supply the portable original color-output VS/PS reflection fixture directory."
#endif
namespace {
    using namespace Horo;
    using namespace Horo::Render;
    constexpr auto SceneStage = ColorOutputShaderStage::SceneToDisplayLinear;

    /** @brief Reads bounded actual captured parser text; this suite runs no compiler or GPU. */
    std::string Read(std::string_view name) {
        std::ifstream file(std::filesystem::path{HORO_COLOR_OUTPUT_REFLECTION_FIXTURE_PATH} / name, std::ios::binary | std::ios::ate);
        REQUIRE(file.good());
        const auto size = file.tellg();
        REQUIRE(size > 0);
        REQUIRE(size <= 1024 * 1024);
        std::string text(static_cast<std::size_t>(size), '\0');
        file.seekg(0);
        file.read(text.data(), static_cast<std::streamsize>(text.size()));
        REQUIRE(file.good());
        return text;
    }

    /** @brief Uses the canonical admitted Metal raster target. */
    ShaderTargetRequirement Target() {
        return {ShaderTargetBackend::Metal, ShaderPayloadFormat::MetalLibrary24, 1, 1, 32, 128, true, true};
    }

    /** @brief Builds the actual product recipe without duplicating its IDs or parameter registry. */
    ShaderManifest Manifest() {
        auto result = MakeColorOutputShaderManifest(SceneStage, Target());
        REQUIRE(result.HasValue());
        return std::move(result).Value();
    }

    /** @brief Introduces one explicit contradiction into a copy of original compiler evidence. */
    void Replace(std::string &text, std::string_view from, std::string_view to) {
        const auto at = text.find(from);
        REQUIRE(at != std::string::npos);
        text.replace(at, from.size(), to);
    }

    /** @brief Owns canonical recipe and actual paired-stage JSON/native evidence. */
    struct Capture {
        ShaderManifest manifest{Manifest()};
        std::vector<ShaderReflectionBindingName> names{ColorOutputShaderReflectionNames(SceneStage)};
        std::array<SpirvReflectionStage, 2> stages{{{ShaderStage::Vertex, "VSMain", Read("color_output-vert.json")},
                                                    {ShaderStage::Fragment, "PSMain", Read("color_output-frag.json")}}};
        std::array<std::string, 2> native{{Read("color_output-vert.metal"), Read("color_output-frag.metal")}};

        /** @brief Exercises the production bounded register-raster normalization. */
        Result<NormalizedShaderReflection> Normalize() const {
            return NormalizeRegisterRasterReflection(manifest, Target(), stages, native, names);
        }
    };

    /** @brief Requires a typed malformed-evidence result rather than a tool or unsupported-backend failure. */
    void Rejected(const Capture &capture) {
        const auto result = capture.Normalize();
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == ShaderReflectionErrors::InvalidReflection.code.Value());
    }
}  // namespace

TEST_CASE("Metal register reflection reconciles genuine three-image two-sampler color output", "[renderer][shader-cook][multi-image]") {
    const Capture capture;
    const auto result = capture.Normalize();
    INFO((result.HasError() ? result.ErrorValue().message : std::string{"actual color output reflection"}));
    REQUIRE(result.HasValue());
    const std::array<std::uint32_t, 7> slots{4, 0, 1, 5, 2, 6, 3};
    REQUIRE(result.Value().targetBindings.size() == slots.size());
    for (std::size_t i = 0; i < slots.size(); ++i) {
        CHECK(result.Value().targetBindings[i].nativeBinding == slots[i]);
        CHECK_FALSE(result.Value().targetBindings[i].pairedSampler.IsValid());
    }
    REQUIRE(result.Value().parameters.size() == 4);
    CHECK(result.Value().parameters[0].byteOffset == 0);
    CHECK(result.Value().parameters[1].byteOffset == 16);
    CHECK(result.Value().parameters[2].byteOffset == 32);
    CHECK(result.Value().parameters[3].byteOffset == 0);
    REQUIRE(result.Value().stageInterface.size() == 1);
    CHECK(result.Value().stageInterface[0].stage == ShaderStage::Fragment);
}

TEST_CASE("Metal sampled images loaded without a sampler reconcile the genuine canonical encode capture",
          "[renderer][shader-cook][multi-image]") {
    const auto stage = ColorOutputShaderStage::EncodeSdr;
    auto manifest = MakeColorOutputShaderManifest(stage, Target());
    REQUIRE(manifest.HasValue());
    const std::array<SpirvReflectionStage, 2> reflection{{{ShaderStage::Vertex, "VSMain", Read("color_encode_sdr-vert.json")},
                                                          {ShaderStage::Fragment, "PSMain", Read("color_encode_sdr-frag.json")}}};
    const std::array<std::string, 2> native{{Read("color_encode_sdr-vert.metal"), Read("color_encode_sdr-frag.metal")}};
    const auto result =
        NormalizeRegisterRasterReflection(manifest.Value(), Target(), reflection, native, ColorOutputShaderReflectionNames(stage));
    INFO((result.HasError() ? result.ErrorValue().message : std::string{"actual encode reflection"}));
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().targetBindings.size() == 2);
    CHECK_FALSE(result.Value().targetBindings[1].pairedSampler.IsValid());
}

TEST_CASE("Metal independent resources reject duplicate native texture or sampler namespace slots",
          "[renderer][shader-cook][multi-image]") {
    for (const auto &[from, to] :
         std::array{std::pair{"[[texture(5)]]", "[[texture(1)]]"}, std::pair{"[[sampler(6)]]", "[[sampler(2)]]"}}) {
        Capture capture;
        Replace(capture.native[1], from, to);
        Rejected(capture);
    }
}

TEST_CASE("Metal independent resources reject duplicate Cross registers and foreign names", "[renderer][shader-cook][multi-image]") {
    for (const auto &[from, to] :
         std::array{std::pair{"\"binding\" : 5", "\"binding\" : 1"}, std::pair{"\"binding\" : 6", "\"binding\" : 2"},
                    std::pair{"\"name\" : \"SceneColor\"", "\"name\" : \"UnknownScene\""}}) {
        Capture capture;
        Replace(capture.stages[1].json, from, to);
        Rejected(capture);
    }
}

TEST_CASE("Metal independent evidence rejects missing or silently renamed selected-entry resources",
          "[renderer][shader-cook][multi-image]") {
    Capture missing;
    Replace(missing.native[1], "texture2d<float> SceneColor [[texture(0)]], ", "");
    Rejected(missing);
    Capture renamed;
    Replace(renamed.native[1], "texture2d<float> SceneColor", "texture2d<float> UnknownScene");
    Rejected(renamed);
    Capture sampler;
    Replace(sampler.native[1], "sampler horo_color_gamut_cusp_table_0Sampler [[sampler(6)]], ", "");
    Rejected(sampler);
}

TEST_CASE("Metal independent sampled image declarations remain nondepth readonly float2D and nonarray",
          "[renderer][shader-cook][multi-image]") {
    for (const auto replacement : std::array{"depth2d<float> SceneColor", "texture2d_array<float> SceneColor", "texture2d<uint> SceneColor",
                                             "texture2d<float, access::write> SceneColor"}) {
        Capture capture;
        Replace(capture.native[1], "texture2d<float> SceneColor", replacement);
        Rejected(capture);
    }
    Capture array;
    Replace(array.stages[1].json, "\"name\" : \"SceneColor\"", "\"name\" : \"SceneColor\", \"array\": [1]");
    Rejected(array);
    Capture writable;
    Replace(writable.stages[1].json, "\"name\" : \"SceneColor\"", "\"name\" : \"SceneColor\", \"writeonly\": true");
    Rejected(writable);
}

TEST_CASE("Metal independent samples retain unchanged readonly uint storage proof", "[renderer][shader-cook][multi-image]") {
    Capture native;
    Replace(native.native[1], "const device type_StructuredBuffer_uint& ExposureWords", "device type_StructuredBuffer_uint& ExposureWords");
    Rejected(native);
    Capture access;
    Replace(access.stages[1].json, "\"readonly\" : true", "\"readonly\" : false");
    Rejected(access);
    Capture stride;
    Replace(stride.stages[1].json, "\"array_stride\" : 4", "\"array_stride\" : 8");
    Rejected(stride);
}

TEST_CASE("Fullscreen vertex index is exact once and cannot fabricate a logical attribute", "[renderer][shader-cook][multi-image]") {
    Capture wrong;
    Replace(wrong.native[0], "uint gl_VertexIndex [[vertex_id]]", "int gl_VertexIndex [[vertex_id]]");
    Rejected(wrong);
    Capture duplicate;
    Replace(duplicate.native[0], "uint gl_VertexIndex [[vertex_id]]", "uint gl_VertexIndex [[vertex_id]], uint otherIndex [[vertex_id]]");
    Rejected(duplicate);
}

TEST_CASE("Metal independent sampling stays bounded and OpenGL retains the original sole-pair profile",
          "[renderer][shader-cook][multi-image]") {
    Capture excess;
    for (std::uint32_t i = 0; i < 6; ++i) {
        excess.manifest.bindings.push_back(
            {{20 + i}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment});
        excess.names.push_back({{20 + i}, "Unused" + std::to_string(i), {}});
    }
    CHECK(ValidateRegisterRasterReflection(excess.manifest, Target(), excess.names).HasError());
    Capture source;
    auto gl = Target();
    gl.backend = ShaderTargetBackend::OpenGL;
    gl.payloadFormat = ShaderPayloadFormat::Glsl410;
    source.manifest.targets = {gl};
    CHECK(ValidateRegisterRasterReflection(source.manifest, gl, source.names).HasError());
}
