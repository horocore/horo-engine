#include "Horo/Runtime/Render/VfxShaderParameters.h"
#include "LockedVfxShaderFixture.h"
#include "ShaderReflectionCook.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <string>

namespace {
    using namespace Horo::Render;
    constexpr std::string_view Types = R"("types":{
        "_instances":{"members":[
            {"name":"ClipFromLocal","type":"mat4","offset":0,"array":[128],"array_size_is_literal":[true],"array_stride":64,"matrix_stride":16},
            {"name":"InstanceColor","type":"vec4","offset":8192,"array":[128],"array_size_is_literal":[true],"array_stride":16}]},
        "_material":{"members":[{"name":"Tint","type":"vec4","offset":0},{"name":"AlphaCutoff","type":"float","offset":16}]}})";
    constexpr std::string_view Instances = R"({"type":"_instances","name":"type.VfxInstances","block_size":10240,"set":0,"binding":0})";
    constexpr std::string_view Material = R"({"type":"_material","name":"type.VfxMaterial","block_size":20,"set":0,"binding":1})";
    constexpr std::string_view Structs = R"(struct type_VfxInstances { float4x4 ClipFromLocal[128]; float4 InstanceColor[128]; };
        struct type_VfxMaterial { float4 Tint; float AlphaCutoff; };)";
    constexpr std::string_view Varyings = R"([{"name":"uv","type":"vec2","location":0},{"name":"color","type":"vec4","location":1}])";
    // Exact helper shape retained from the locked qualifier-only translation diagnostic, never native/GPU evidence.
    constexpr std::string_view MatrixHelper = R"(
        template<typename T, int Cols, int Rows>
        [[clang::optnone]] vec<T, Rows> spvFMulMatrixVector(matrix<T, Cols, Rows> m, vec<T, Cols> v) {
            vec<T, Rows> res = vec<T, Rows>(0);
            for (uint i = Cols; i > 0; --i) { res = fma(m[i - 1], vec<T, Rows>(v[i - 1]), res); }
            return res;
        })";

    /** @brief Scripted official-schema parser regressions only; these bytes are never cooked native/GPU qualification evidence. */
    struct Evidence {
        ShaderTargetRequirement target{ShaderTargetBackend::Metal, ShaderPayloadFormat::MetalLibrary24, 1, 1, 4, 0, false, false};
        ShaderManifest manifest{MakeVfxShaderManifest(target)};
        std::array<SpirvReflectionStage, 2> stages{
            {{ShaderStage::Vertex, "VSMain",
              std::string{R"({"entryPoints":[{"name":"VSMain","mode":"vert"}],)"} + std::string{Types} + ",\"ubos\":[" +
                  std::string{Instances} + "," + std::string{Material} +
                  R"(],"inputs":[{"name":"position","type":"vec3","location":0},{"name":"uv","type":"vec2","location":2}],"outputs":)" +
                  std::string{Varyings} + "}"},
             {ShaderStage::Fragment, "PSMain",
              std::string{R"({"entryPoints":[{"name":"PSMain","mode":"frag"}],)"} + std::string{Types} +
                  ",\"inputs\":" + std::string{Varyings} +
                  R"(,"outputs":[{"name":"color","type":"vec4","location":0}],"separate_images":[{"name":"BaseColor","type":"texture2D","set":0,"binding":2}],"separate_samplers":[{"name":"BaseColorSampler","type":"sampler","set":0,"binding":3}]})"}}};
        std::array<std::string, 2>
            translations{std::string{Structs} + std::string{MatrixHelper} +
                             " vertex void VSMain(constant type_VfxInstances& VfxInstances [[buffer(7)]], constant "
                             "type_VfxMaterial& m [[buffer(9)]]) { out.gl_Position = spvFMulMatrixVector("
                             "VfxInstances.ClipFromLocal[gl_InstanceIndex], float4(in.in_var_POSITION, 1.0)); }",
                         "fragment void PSMain(texture2d<float> BaseColor [[texture(4)]], sampler BaseColorSampler [[sampler(2)]]) {}"};
    };

    void Replace(std::string &text, const std::string_view before, const std::string_view after) {
        const auto at = text.find(before);
        REQUIRE(at != text.npos);
        text.replace(at, before.size(), after);
    }

    /** @brief Scripted GL parser evidence only; never native shader qualification. */
    Evidence OpenGLEvidence() {
        Evidence evidence;
        evidence.target.backend = ShaderTargetBackend::OpenGL;
        evidence.target.payloadFormat = ShaderPayloadFormat::Glsl410;
        evidence.manifest = MakeVfxShaderManifest(evidence.target);
        evidence.translations =
            {"#version 410\nlayout(std140) uniform ActualInstances { mat4 ClipFromLocal[128]; vec4 InstanceColor[128]; } i;\n"
             "layout(std140) uniform ActualMaterial { vec4 Tint; float AlphaCutoff; } m;\n"
             "layout(location=0) in vec3 NativePosition; layout(location=2) in vec2 NativeUv;"
             "layout(location=0) out vec2 VertexUv; layout(location=1) out vec4 VertexColor; void main() {"
             "precise vec4 clip = i.ClipFromLocal[uint(gl_InstanceID)] * vec4(NativePosition, 1.0); gl_Position = clip; }",
             "#version 410\nuniform sampler2D ActualCombinedPair;\n"
             "layout(location=0) in vec2 VertexUv; layout(location=1) in vec4 VertexColor;"
             "layout(location=0) out vec4 NativeTarget0; void main() {}"};
        return evidence;
    }
}  // namespace

TEST_CASE("VFX cook preserves canonical semantics and actual nondefault native placements", "[renderer][shader-cook][vfx]") {
    Evidence evidence;
    REQUIRE(ValidateShaderManifest(evidence.manifest).HasValue());
    const auto result = NormalizeVfxMetalReflection(evidence.manifest, evidence.target, evidence.stages, evidence.translations);
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().targetBindings.size() == 4);
    CHECK(result.Value().targetBindings[0].nativeBinding == 7);
    CHECK(result.Value().targetBindings[1].nativeBinding == 9);
    CHECK(result.Value().targetBindings[2].nativeBinding == 4);
    CHECK(result.Value().targetBindings[3].nativeBinding == 2);
    CHECK(result.Value().targetBindings[2].pairedSampler == VfxShaderParameterIds::BaseColorSampler);
    REQUIRE(result.Value().parameters.size() == 4);
    CHECK(result.Value().parameters[0].arrayStride == 64);
    CHECK(result.Value().parameters[0].columnMajor);
    CHECK(result.Value().parameters[1].byteOffset == 8192);
    CHECK(result.Value().parameters[3].id == VfxShaderParameterIds::AlphaCutoff);
}

TEST_CASE("VFX cook rejects unproven intermediate or native layout rather than publishing guessed offsets",
          "[renderer][shader-cook][vfx]") {
    Evidence evidence;
    SECTION("Prior cook contract revision") {
        evidence.manifest.sourceRevision = 2;
    }
    SECTION("Array stride") {
        Replace(evidence.stages[0].json, "\"array_stride\":64", "\"array_stride\":80");
    }
    SECTION("Runtime array") {
        Replace(evidence.stages[0].json, "\"array_size_is_literal\":[true]", "\"array_size_is_literal\":[false]");
    }
    SECTION("Wrong major order has no admitted native lowering proof") {
        Replace(evidence.stages[0].json, "\"matrix_stride\":16", "\"matrix_stride\":16,\"row_major\":true");
    }
    SECTION("Matrix stride") {
        Replace(evidence.stages[0].json, "\"matrix_stride\":16", "\"matrix_stride\":32");
    }
    SECTION("Offset") {
        Replace(evidence.stages[0].json, "\"offset\":8192", "\"offset\":8208");
    }
    SECTION("Native array shape") {
        Replace(evidence.translations[0], "ClipFromLocal[128]", "ClipFromLocal[127]");
    }
    SECTION("Native matrix shape") {
        Replace(evidence.translations[0], "float4x4 ClipFromLocal", "float3x4 ClipFromLocal");
    }
    SECTION("Duplicate native argument") {
        evidence.translations[0] += " constant type_VfxInstances& j [[buffer(8)]]";
    }
    SECTION("Native uniform collision") {
        Replace(evidence.translations[0], "buffer(9)", "buffer(7)");
    }
    CHECK(NormalizeVfxMetalReflection(evidence.manifest, evidence.target, evidence.stages, evidence.translations).HasError());
}

TEST_CASE("VFX cook rejects competing native semantics and foreign logical evidence", "[renderer][shader-cook][vfx]") {
    Evidence evidence;
    SECTION("Helper spelling cannot hide different matrix semantics") {
        Replace(evidence.translations[0], "m[i - 1]", "m[0]");
    }
    SECTION("Position call cannot reverse vector order") {
        Replace(evidence.translations[0], "out.gl_Position = spvFMulMatrixVector", "out.gl_Position = spvFMulVectorMatrix");
    }
    SECTION("An overload cannot compete with the proved helper") {
        evidence.translations[0] += "float4 spvFMulMatrixVector(float4 value) { return value; }";
    }
    SECTION("Comment cannot provide slot") {
        Replace(evidence.translations[0], "[[buffer(7)]]", "/* [[buffer(7)]] */");
    }
    SECTION("Duplicate JSON key") {
        evidence.stages[0].json.insert(1, "\"ubos\":[],");
    }
    SECTION("Unknown active resource") {
        Replace(evidence.stages[1].json, "BaseColorSampler", "ForeignSampler");
    }
    SECTION("Foreign manifest") {
        evidence.manifest.parameters[0].arrayCount = 127;
    }
    SECTION("Oversized source") {
        evidence.translations[0].resize(1024U * 1024U + 1, ' ');
    }
    CHECK(NormalizeVfxMetalReflection(evidence.manifest, evidence.target, evidence.stages, evidence.translations).HasError());
}

TEST_CASE("VFX cook handles optimized fragment uniform removal without renumbering logical identities", "[renderer][shader-cook][vfx]") {
    Evidence evidence;
    SECTION("Unmasked optimizer removes fragment material argument") {
        // The default evidence intentionally has no fragment UBO. Vertex Tint still owns its exact logical identity.
    }
    SECTION("Masked fragment retains the same material ABI") {
        evidence.stages[1].json.insert(1, "\"ubos\":[" + std::string{Material} + "],");
        evidence.translations[1] = std::string{Structs} + evidence.translations[1];
        Replace(evidence.translations[1], "PSMain(", "PSMain(constant type_VfxMaterial& m [[buffer(9)]], ");
    }
    const auto result = NormalizeVfxMetalReflection(evidence.manifest, evidence.target, evidence.stages, evidence.translations);
    REQUIRE(result.HasValue());
    CHECK(result.Value().bindings[0].id == VfxShaderParameterIds::Instances);
    CHECK(result.Value().bindings[1].id == VfxShaderParameterIds::Material);
    CHECK(result.Value().parameters[3].id == VfxShaderParameterIds::AlphaCutoff);
}

TEST_CASE("VFX GL410 cook takes actual block and combined names and assigns finite link bindings", "[renderer][shader-cook][vfx][opengl]") {
    auto evidence = OpenGLEvidence();
    const auto result = NormalizeVfxOpenGLReflection(evidence.manifest, evidence.target, evidence.stages, evidence.translations);
    REQUIRE(result.HasValue());
    REQUIRE(result.Value().targetBindings.size() == 4);
    CHECK(result.Value().targetBindings[0].nativeName == "ActualInstances");
    CHECK(result.Value().targetBindings[0].nativeBinding == 0);
    CHECK(result.Value().targetBindings[1].nativeName == "ActualMaterial");
    CHECK(result.Value().targetBindings[1].nativeBinding == 1);
    CHECK(result.Value().targetBindings[2].nativeName == "ActualCombinedPair");
    CHECK(result.Value().targetBindings[2].pairedSampler == VfxShaderParameterIds::BaseColorSampler);
    CHECK(result.Value().targetBindings[2].nativeBinding == result.Value().targetBindings[3].nativeBinding);
    CHECK(result.Value().parameters[0].arrayCount == 128);
    CHECK(result.Value().parameters[0].matrixStride == 16);
    const auto output = std::ranges::find_if(result.Value().stageInterface, [](const auto &variable) {
        return variable.stage == ShaderStage::Fragment && variable.direction == ShaderInterfaceDirection::Output;
    });
    REQUIRE(output != result.Value().stageInterface.end());
    CHECK(output->nativeName == "NativeTarget0");
}

TEST_CASE("VFX GL cook rejects missing unsupported or ambiguous final-target evidence", "[renderer][shader-cook][vfx][opengl]") {
    auto evidence = OpenGLEvidence();
    SECTION("Wrong native language version") {
        Replace(evidence.translations[0], "version 410", "version 430");
    }
    SECTION("Storage rather than uniform lowering") {
        Replace(evidence.translations[0], "std140", "std430");
    }
    SECTION("Unproved native matrix order") {
        Replace(evidence.translations[0], "mat4 ClipFromLocal", "layout(row_major) mat4 ClipFromLocal");
    }
    SECTION("Wrong native array extent") {
        Replace(evidence.translations[0], "ClipFromLocal[128]", "ClipFromLocal[127]");
    }
    SECTION("Unknown uniform") {
        evidence.translations[1] += " uniform float Foreign;";
    }
    SECTION("Ambiguous combined pair") {
        evidence.translations[1] += " uniform sampler2D OtherPair;";
    }
    SECTION("Sampler declaration hidden in comment") {
        Replace(evidence.translations[1], "uniform sampler2D ActualCombinedPair;", "/* uniform sampler2D ActualCombinedPair; */");
    }
    SECTION("Comparison sampler changes the material contract") {
        Replace(evidence.translations[1], "sampler2D", "sampler2DShadow");
    }
    SECTION("Intermediate member offset differs") {
        Replace(evidence.stages[0].json, "\"offset\":8192", "\"offset\":8208");
    }
    SECTION("Native layout cannot come from a Metal map") {
        evidence.translations[0] = std::string{Structs};
    }
    SECTION("Actual GL output location must agree with intermediate evidence") {
        Replace(evidence.translations[1], "layout(location=0) out vec4", "layout(location=1) out vec4");
    }
    SECTION("Duplicate GL output is not an alternate name authority") {
        evidence.translations[1] += "layout(location=0) out vec4 OtherTarget;";
    }
    SECTION("Monolithic GL410 requires matching stage varying names despite equal locations") {
        Replace(evidence.translations[1], "in vec2 VertexUv", "in vec2 ForeignUv");
    }
    SECTION("A matrix declaration cannot hide reversed position evaluation") {
        Replace(evidence.translations[0], "i.ClipFromLocal[uint(gl_InstanceID)] * vec4(NativePosition, 1.0)",
                "vec4(NativePosition, 1.0) * i.ClipFromLocal[uint(gl_InstanceID)]");
    }
    SECTION("A different final temporary cannot replace the proved position") {
        Replace(evidence.translations[0], "gl_Position = clip;", "gl_Position = other;");
    }
    CHECK(NormalizeVfxOpenGLReflection(evidence.manifest, evidence.target, evidence.stages, evidence.translations).HasError());
}

TEST_CASE("VFX GL cook preserves optimizer stage removal and rejects different cross-stage block identities",
          "[renderer][shader-cook][vfx][opengl]") {
    auto evidence = OpenGLEvidence();
    bool accepted = true;
    SECTION("Unused fragment material block removed") {}
    SECTION("Same material block retained by masked fragment") {
        evidence.stages[1].json.insert(1, "\"ubos\":[" + std::string{Material} + "],");
        evidence.translations[1] += " layout(std140) uniform ActualMaterial { vec4 Tint; float AlphaCutoff; } m;";
    }
    SECTION("Different active native block cannot share one logical map") {
        evidence.stages[1].json.insert(1, "\"ubos\":[" + std::string{Material} + "],");
        evidence.translations[1] += " layout(std140) uniform OtherMaterial { vec4 Tint; float AlphaCutoff; } m;";
        accepted = false;
    }
    CHECK(NormalizeVfxOpenGLReflection(evidence.manifest, evidence.target, evidence.stages, evidence.translations).HasValue() == accepted);
}

TEST_CASE("Locked VFX final profiles normalize authentic ordinary and masked stage evidence", "[renderer][shader-cook][vfx][locked]") {
    const auto backend = GENERATE(ShaderTargetBackend::OpenGL, ShaderTargetBackend::Metal);
    const bool masked = GENERATE(false, true);
    const auto evidence = Horo::Tests::LoadLockedVfxShaderEvidence(backend, masked);
    const auto normalized = backend == ShaderTargetBackend::OpenGL
                                ? NormalizeVfxOpenGLReflection(evidence.manifest, evidence.target, evidence.stages, evidence.translations)
                                : NormalizeVfxMetalReflection(evidence.manifest, evidence.target, evidence.stages, evidence.translations);
    INFO("Actual captured backend=" << static_cast<unsigned>(backend) << " masked=" << masked);
    INFO((normalized.HasError() ? normalized.ErrorValue().message : "Actual captured reflection admitted"));
    REQUIRE(normalized.HasValue());
    REQUIRE(normalized.Value().parameters.size() == 4);
    const auto &matrix = normalized.Value().parameters.front();
    CHECK(matrix.arrayCount == 128);
    CHECK(matrix.arrayStride == 64);
    CHECK(matrix.matrixStride == 16);
    CHECK(matrix.columnMajor);
    REQUIRE(normalized.Value().targetBindings.size() == 4);
    CHECK(normalized.Value().targetBindings[0].nativeBinding == 0);
    CHECK(normalized.Value().targetBindings[1].nativeBinding == 1);
    REQUIRE(normalized.Value().bindings.size() == 4);
    CHECK(normalized.Value().bindings[0].stages == ShaderStageVisibility::Vertex);
    CHECK(normalized.Value().bindings[1].stages ==
          (masked ? ShaderStageVisibility::Vertex | ShaderStageVisibility::Fragment : ShaderStageVisibility::Vertex));
    CHECK(normalized.Value().bindings[2].stages == ShaderStageVisibility::Fragment);
    CHECK(normalized.Value().bindings[3].stages == ShaderStageVisibility::Fragment);
}

TEST_CASE("Locked GL VFX text cooks a final-target package with its actual reflection identity", "[renderer][shader-cook][vfx][locked]") {
    const bool masked = GENERATE(false, true);
    const auto evidence = Horo::Tests::LoadLockedVfxShaderEvidence(ShaderTargetBackend::OpenGL, masked);
    const auto artifact = Horo::Tests::CookLockedVfxOpenGlFixture(evidence);
    REQUIRE(artifact.reflection);
    CHECK(ValidateReflectedShaderArtifact(artifact, *artifact.reflection).HasValue());
    CHECK(artifact.reflection->reflection.backend == ShaderTargetBackend::OpenGL);
    const auto output = std::ranges::find_if(artifact.reflection->reflection.stageInterface, [](const auto &variable) {
        return variable.stage == ShaderStage::Fragment && variable.direction == ShaderInterfaceDirection::Output;
    });
    REQUIRE(output != artifact.reflection->reflection.stageInterface.end());
    CHECK(output->nativeName == "out_var_SV_Target0");
    // No production compiler adapter, native Metal library, link, device or GPU execution occurs in this CPU case.
}

TEST_CASE("Locked GL VFX cooked metadata roundtrips its exact package and rejects tampering", "[renderer][shader-cook][vfx][locked]") {
    const auto evidence = Horo::Tests::LoadLockedVfxShaderEvidence(ShaderTargetBackend::OpenGL, GENERATE(false, true));
    const auto artifact = Horo::Tests::CookLockedVfxOpenGlFixture(evidence);
    REQUIRE(artifact.reflection);
    const auto encoded = EncodeReflectedShaderArtifact(*artifact.reflection);
    REQUIRE(encoded.HasValue());
    const auto decoded = DecodeReflectedShaderArtifact(evidence.manifest, evidence.target, artifact.payload, encoded.Value());
    REQUIRE(decoded.HasValue());
    CHECK(decoded.Value().cookedArtifactKey == artifact.reflection->cookedArtifactKey);
    const auto reencoded = EncodeReflectedShaderArtifact(decoded.Value());
    REQUIRE(reencoded.HasValue());
    CHECK(reencoded.Value() == encoded.Value());

    SECTION("Changed GLSL package bytes") {
        auto payload = artifact.payload;
        payload.back() ^= 1;
        CHECK(DecodeReflectedShaderArtifact(evidence.manifest, evidence.target, payload, encoded.Value()).HasError());
    }
    SECTION("Changed canonical metadata bytes") {
        auto metadata = encoded.Value();
        metadata.push_back(' ');
        CHECK(DecodeReflectedShaderArtifact(evidence.manifest, evidence.target, artifact.payload, metadata).HasError());
    }
}

TEST_CASE("GL cooked reflection rejects foreign backends and mismatched payload formats", "[renderer][shader-cook][vfx][locked]") {
    const auto evidence = Horo::Tests::LoadLockedVfxShaderEvidence(ShaderTargetBackend::OpenGL, false);
    const auto artifact = Horo::Tests::CookLockedVfxOpenGlFixture(evidence);
    REQUIRE(artifact.reflection);
    const auto encoded = EncodeReflectedShaderArtifact(*artifact.reflection);
    REQUIRE(encoded.HasValue());
    for (const auto backend :
         {ShaderTargetBackend::Vulkan, ShaderTargetBackend::Metal, ShaderTargetBackend::D3D12, ShaderTargetBackend::Null}) {
        auto compiled = artifact;
        compiled.backend = backend;
        CHECK(ValidateReflectedShaderArtifact(compiled, *artifact.reflection).HasError());
        auto target = evidence.target;
        target.backend = backend;
        CHECK(DecodeReflectedShaderArtifact(evidence.manifest, target, artifact.payload, encoded.Value()).HasError());
    }
    for (const auto format : {ShaderPayloadFormat::SpirV16, ShaderPayloadFormat::MetalLibrary24, ShaderPayloadFormat::Dxil60}) {
        auto compiled = artifact;
        compiled.payloadFormat = format;
        CHECK(ValidateReflectedShaderArtifact(compiled, *artifact.reflection).HasError());
        auto target = evidence.target;
        target.payloadFormat = format;
        CHECK(DecodeReflectedShaderArtifact(evidence.manifest, target, artifact.payload, encoded.Value()).HasError());
    }
    for (const auto backend : {ShaderTargetBackend::Null, ShaderTargetBackend::D3D12}) {
        auto reflection = *artifact.reflection;
        reflection.reflection.backend = backend;
        CHECK(EncodeReflectedShaderArtifact(reflection).HasError());
    }
}
