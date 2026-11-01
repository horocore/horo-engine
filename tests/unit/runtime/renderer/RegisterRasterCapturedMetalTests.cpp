#include "EditorViewportShaderRecipe.h"
#include "ShaderReflectionCook.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    std::string Capture(const char *name) {
        const auto path =
            std::filesystem::path{HORO_PROJECT_SOURCE_DIR} / "tests/fixtures/shaders/register_raster_metal_lit_20261101" / name;
        std::ifstream input{path, std::ios::binary};
        REQUIRE(input.good());
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    void Replace(std::string &source, std::string_view before, std::string_view after) {
        const auto at = source.find(before);
        REQUIRE(at != source.npos);
        source.replace(at, before.size(), after);
    }

    /** @brief Exact real JSON/MSL capture; this parser regression does not execute a native compiler or the GPU. */
    struct CapturedMetal final {
        ShaderTargetRequirement target{.backend = ShaderTargetBackend::Metal,
                                       .payloadFormat = ShaderPayloadFormat::MetalLibrary24,
                                       .descriptorVersion = 1,
                                       .interfaceSchemaVersion = 1,
                                       .maximumBindings = 32,
                                       .maximumInlineConstantBytes = 128,
                                       .supportsCompute = true,
                                       .supportsStorageResources = true};
        ShaderManifest manifest;
        std::vector<ShaderReflectionBindingName> names;
        std::array<SpirvReflectionStage, 2> stages{
            {{ShaderStage::Vertex, "VSMain", Capture("vertex.json")}, {ShaderStage::Fragment, "PSMain", Capture("fragment.json")}}};
        std::array<std::string, 2> translations{Capture("vertex.metal"), Capture("fragment.metal")};

        CapturedMetal() {
            const auto canonical = Editor::MakeEditorViewportClusteredShaderManifest(Editor::EditorViewportShaderProgram::Lit, target);
            REQUIRE(canonical.HasValue());
            manifest = canonical.Value();
            names = Editor::EditorViewportClusteredShaderReflectionNames(Editor::EditorViewportShaderProgram::Lit);
        }

        auto Normalize() const {
            return NormalizeRegisterRasterReflection(manifest, target, stages, translations, names);
        }
    };
}  // namespace

TEST_CASE("Real captured Metal clustered lit reflection admits the native fragment position builtin",
          "[renderer][shader-cook][register-raster][actual-capture]") {
    const CapturedMetal capture;
    const auto normalized = capture.Normalize();
    REQUIRE(normalized.HasValue());
    CHECK(normalized.Value().bindings.size() == 6);
    CHECK(normalized.Value().parameters.size() == 27);
    CHECK(normalized.Value().stageInterface.size() == 7);
    const auto &targets = normalized.Value().targetBindings;
    REQUIRE(targets.size() == 6);
    CHECK(targets[0].nativeName == "ViewportParameters");
    CHECK(targets[3].nativeName == "ClusterLights");
    CHECK(targets[3].nativeBinding == 3);
    CHECK(targets[4].nativeBinding == 4);
    CHECK(targets[5].nativeBinding == 5);
}

TEST_CASE("Captured Metal fragment position requires its exact float4 argument shape stage and uniqueness",
          "[renderer][shader-cook][register-raster][actual-capture]") {
    CapturedMetal capture;
    const auto mutation = GENERATE(0, 1, 2, 3, 4, 5);
    if (mutation == 0)
        Replace(capture.translations[1], "float4 gl_FragCoord [[position]]", "float3 gl_FragCoord [[position]]");
    if (mutation == 1)
        Replace(capture.translations[1], "float4 gl_FragCoord [[position]]", "float4& gl_FragCoord [[position]]");
    if (mutation == 2)
        Replace(capture.translations[1], "float4 gl_FragCoord [[position]]",
                "float4 gl_FragCoord [[position]], float4 duplicate [[position]]");
    if (mutation == 3)
        Replace(capture.translations[0], "VSMain(VSMain_in in [[stage_in]],",
                "VSMain(float4 unexpected [[position]], VSMain_in in [[stage_in]],");
    if (mutation == 4)
        Replace(capture.translations[1], "float4 gl_FragCoord [[position]]", "float4 gl_FragCoord [[position(0)]]");
    if (mutation == 5)
        Replace(capture.translations[1], "float4 gl_FragCoord [[position]]", "float4 gl_FragCoord [[unknown_builtin]]");
    CHECK(capture.Normalize().HasError());
}

TEST_CASE("Captured Metal raster keeps strict readonly storage layout and active entry evidence",
          "[renderer][shader-cook][register-raster][actual-capture]") {
    CapturedMetal capture;
    const auto mutation = GENERATE(0, 1, 2, 3);
    if (mutation == 0)
        Replace(capture.translations[1], "packed_float3 direction;", "float3 direction;");
    if (mutation == 1)
        Replace(capture.translations[1], "const device type_StructuredBuffer_PackedLight&", "device type_StructuredBuffer_PackedLight&");
    if (mutation == 2)
        Replace(capture.translations[1], "ClusterMembership [[buffer(4)]]", "ClusterMembership [[buffer(3)]]");
    if (mutation == 3)
        Replace(capture.translations[1], "fragment PSMain_out PSMain(", "fragment PSMain_out OtherEntry(");
    CHECK(capture.Normalize().HasError());
}
