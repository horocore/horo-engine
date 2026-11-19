#include "EditorVulkanViewportShader.h"
#include "Horo/Runtime/Render/ShaderArtifact.h"
#include "support/ShaderReflectionCookTestSupport.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Editor;

    using namespace Horo::Tests::ShaderCook;

}  // namespace

TEST_CASE("Viewport runtime consumes canonical reflection identity and rejects altered payload", "[renderer][shader-cook]") {
    CookFixture fixture;
    const auto encoded = EncodeReflectedShaderArtifact(fixture.reflected);
    REQUIRE(encoded.HasValue());
    const auto loaded = LoadEditorVulkanViewportCook(fixture.compiled.payload, encoded.Value());
    REQUIRE(loaded.HasValue());
    CHECK(loaded.Value().metadata.cookedArtifactKey == fixture.reflected.cookedArtifactKey);
    fixture.compiled.payload.back() ^= 1;
    CHECK(LoadEditorVulkanViewportCook(fixture.compiled.payload, encoded.Value()).HasError());
}

TEST_CASE("Cook identity rejects mutable layout tool and compilation provenance", "[renderer][shader-cook]") {
    CookFixture fixture;
    SECTION("Layout version") {
        fixture.reflected.layoutVersion = 2;
    }
    SECTION("Tool release") {
        fixture.reflected.reflectionTool.release += "altered";
    }
    SECTION("Tool build") {
        fixture.reflected.reflectionTool.buildDigest = {};
    }
    SECTION("Compilation key") {
        fixture.reflected.compiledArtifactKey = {};
    }
    SECTION("Layout offset") {
        fixture.reflected.reflection.parameters[0].byteOffset = 16;
    }
    CHECK(EncodeReflectedShaderArtifact(fixture.reflected).HasError());
}

TEST_CASE("Official reflection evidence rejects inconsistent final-stage layout and entry identity", "[renderer][shader-cook]") {
    auto stages = Evidence();
    SECTION("Offset mismatch") {
        const auto offset = stages[1].json.find("\"offset\":64");
        REQUIRE(offset != std::string::npos);
        stages[1].json.replace(offset, 11, "\"offset\":80");
    }
    SECTION("Foreign entry") {
        stages[1].entryPoint = "other";
    }
    SECTION("Duplicate stage") {
        stages[1] = stages[0];
    }
    SECTION("Duplicate JSON key") {
        stages[1].json.insert(1, "\"entryPoints\":[],");
    }
    SECTION("Oversized evidence") {
        stages[1].json.assign(1024U * 1024U + 1, ' ');
    }
    CHECK(NormalizeVulkanReflection(EditorVulkanViewportShaderManifest(), EditorVulkanViewportShaderTarget(), stages,
                                    EditorVulkanViewportReflectionNames())
              .HasError());
}

TEST_CASE("Cook metadata rejects noncanonical duplicate oversized and truncated values", "[renderer][shader-cook]") {
    CookFixture fixture;
    auto encoded = EncodeReflectedShaderArtifact(fixture.reflected);
    REQUIRE(encoded.HasValue());
    auto metadata = std::move(encoded).Value();
    SECTION("Trailing whitespace") {
        metadata.push_back(' ');
    }
    SECTION("Truncated") {
        metadata.pop_back();
    }
    SECTION("Duplicate key") {
        constexpr std::string_view duplicate = "\"layoutVersion\":1,";
        metadata.insert(metadata.begin() + 1, duplicate.begin(), duplicate.end());
    }
    SECTION("Oversized") {
        metadata.resize(1024U * 1024U + 1);
    }
    CHECK(LoadEditorVulkanViewportCook(fixture.compiled.payload, metadata).HasError());
}

TEST_CASE("Viewport cooked attributes require canonical MeshVertex position normal and UV shapes", "[renderer][shader-cook]") {
    auto stages = Evidence();
    SECTION("Foreign UV width") {
        ReplaceEvidence(stages[0].json, "\"type\":\"vec2\"", "\"type\":\"vec3\"");
    }
    SECTION("Missing normal") {
        ReplaceEvidence(stages[0].json, "\"location\":1", "\"location\":3");
    }
    CookFixture fixture{stages};
    const auto encoded = EncodeReflectedShaderArtifact(fixture.reflected);
    REQUIRE(encoded.HasValue());
    CHECK(LoadEditorVulkanViewportCook(fixture.compiled.payload, encoded.Value()).HasError());
}

TEST_CASE("Viewport light reflection preserves fixed array bounds and rejects unprovable packing", "[renderer][shader-cook]") {
    auto stages = Evidence();
    SECTION("Nonliteral array") {
        ReplaceEvidence(stages[1].json, "\"array_size_is_literal\":[true]", "\"array_size_is_literal\":[false]");
    }
    SECTION("Wrong admitted light count") {
        ReplaceEvidence(stages[1].json, "\"array\":[16]", "\"array\":[17]");
    }
    SECTION("Overlapping elements") {
        ReplaceEvidence(stages[1].json, "\"array_stride\":16", "\"array_stride\":4");
    }
    CHECK(NormalizeVulkanReflection(EditorVulkanViewportShaderManifest(), EditorVulkanViewportShaderTarget(), stages,
                                    EditorVulkanViewportReflectionNames())
              .HasError());
}

TEST_CASE("Viewport reflection requires the exact admitted DXC block type identity", "[renderer][shader-cook]") {
    auto stages = Evidence();
    ReplaceEvidence(stages[0].json, "type.ViewportParameters", "ViewportParameters");
    const auto normalized = NormalizeVulkanReflection(EditorVulkanViewportShaderManifest(), EditorVulkanViewportShaderTarget(), stages,
                                                      EditorVulkanViewportReflectionNames());
    REQUIRE(normalized.HasError());
    CHECK(normalized.ErrorValue().message.find("uniform block identity") != std::string::npos);
}

TEST_CASE("Canonical multiview consumer requires actual bound reflection and closed position/uniform ABI", "[renderer][shader-cook][xr]") {
    // Scripted Cross schema tests the production admission/codec, not actual compiler/native execution.
    auto stages = MultiviewEvidence();
    enum class Evidence {
        Exact,
        ForeignAttributeWidth,
        AlteredPayload
    };
    const auto evidence = GENERATE(Evidence::Exact, Evidence::ForeignAttributeWidth, Evidence::AlteredPayload);
    const bool invalidAttributes = evidence == Evidence::ForeignAttributeWidth;
    const bool alteredPayload = evidence == Evidence::AlteredPayload;
    if (invalidAttributes) {
        ReplaceEvidence(stages[0].json, "\"type\":\"vec3\"", "\"type\":\"vec2\"");
    }
    const auto normalized = NormalizeVulkanReflection(EditorVulkanMultiviewShaderManifest(), EditorVulkanViewportShaderTarget(), stages,
                                                      EditorVulkanMultiviewReflectionNames());
    REQUIRE(normalized.HasValue());
    const auto layer = std::ranges::find(normalized.Value().parameters, ShaderParameterId{3}, &ShaderReflectedParameter::id);
    REQUIRE(layer != normalized.Value().parameters.end());
    CHECK(layer->type == ShaderValueType::Uint32);
    CHECK(layer->rows == 1);
    CHECK(layer->columns == 1);
    CHECK(layer->arrayCount == 32);
    CHECK(layer->arrayStride == 16);
    const std::array packageStages{ShaderArtifactStage{ShaderStage::Vertex, "VSMain", {1}},
                                   ShaderArtifactStage{ShaderStage::Fragment, "PSMain", {2}}};
    const auto payload = PackageShaderArtifactStages(ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16, packageStages, 1024);
    REQUIRE(payload.HasValue());
    CompiledShaderArtifact compiled{.backend = ShaderTargetBackend::Vulkan,
                                    .payloadFormat = ShaderPayloadFormat::SpirV16,
                                    .artifactKey = ComputeSha256(std::as_bytes(std::span{payload.Value()})),
                                    .payload = payload.Value()};
    const auto cooked =
        CookReflectedShaderArtifact(compiled, {ShaderCompilerTool::SpirvCross, "scripted-official-schema", compiled.artifactKey},
                                    normalized.Value());
    REQUIRE(cooked.HasValue());
    const auto encoded = EncodeReflectedShaderArtifact(cooked.Value());
    REQUIRE(encoded.HasValue());
    if (alteredPayload)
        compiled.payload.back() ^= 1;
    const auto loaded = LoadEditorVulkanMultiviewCook(compiled.payload, encoded.Value());
    if (invalidAttributes || alteredPayload)
        REQUIRE(loaded.HasError());
    else {
        REQUIRE(loaded.HasValue());
        REQUIRE(loaded.Value().metadata.cookedArtifactKey == cooked.Value().cookedArtifactKey);
        REQUIRE(LoadEditorVulkanViewportCook(compiled.payload, encoded.Value()).HasError());
    }
}

TEST_CASE("Motion shader cooking requires signed XYZ transport and both exact predecessor fields", "[renderer][shader-cook][xr]") {
    const auto stages = MultiviewMotionEvidence();
    const auto manifest = EditorVulkanMultiviewMotionShaderManifest();
    const auto target = EditorVulkanViewportShaderTarget();
    const auto names = EditorVulkanMultiviewMotionReflectionNames();
    auto incomplete = stages;
    ReplaceEvidence(incomplete[0].json, "\"PreviousLocalToClip\"", "\"UnrelatedLocalToClip\"");
    CHECK(NormalizeVulkanReflection(manifest, target, incomplete, names).HasError());
    auto evidence = stages;
    const bool wrongOutput = GENERATE(false, true);
    if (wrongOutput)
        ReplaceEvidence(evidence[1].json, "\"motion\",\"type\":\"vec4\"", "\"motion\",\"type\":\"vec3\"");
    const auto normalized = NormalizeVulkanReflection(manifest, target, evidence, names);
    REQUIRE(normalized.HasValue());
    const CookFixture carrier;
    const auto cooked =
        CookReflectedShaderArtifact(carrier.compiled,
                                    {ShaderCompilerTool::SpirvCross, "scripted-official-schema", carrier.compiled.artifactKey},
                                    normalized.Value());
    REQUIRE(cooked.HasValue());
    const auto encoded = EncodeReflectedShaderArtifact(cooked.Value());
    REQUIRE(encoded.HasValue());
    CHECK(LoadEditorVulkanMultiviewMotionCook(carrier.compiled.payload, encoded.Value()).HasError() == wrongOutput);
    CHECK(LoadEditorVulkanMultiviewCook(carrier.compiled.payload, encoded.Value()).HasError());
}

TEST_CASE("XR coverage cook admits only the exact predecessor sampled depth and scalar output", "[renderer][shader-cook][xr]") {
    auto evidence = MultiviewCoverageEvidence();
    const auto manifest = EditorVulkanMultiviewCoverageShaderManifest();
    const auto names = EditorVulkanMultiviewCoverageReflectionNames();
    const auto target = EditorVulkanViewportShaderTarget();
    {
        auto foreignDepth = evidence;
        ReplaceEvidence(foreignDepth[1].json, "texture2D", "texture2DArray");
        CHECK(NormalizeVulkanReflection(manifest, target, foreignDepth, names).HasError());
    }
    enum class Evidence {
        Exact,
        DepthOptimizedOut,
        ForeignOutput,
        ForeignStage,
    };
    const auto variant = GENERATE(Evidence::Exact, Evidence::DepthOptimizedOut, Evidence::ForeignOutput, Evidence::ForeignStage);
    if (variant == Evidence::DepthOptimizedOut)
        ReplaceEvidence(evidence[1].json, R"(,{"name":"PreviousRightDepth","type":"texture2D","set":0,"binding":2})", "");
    if (variant == Evidence::ForeignOutput)
        ReplaceEvidence(evidence[1].json, R"("coverage","type":"float")", R"("coverage","type":"vec4")");
    const auto reflected = NormalizeVulkanReflection(manifest, target, evidence, names);
    REQUIRE(reflected.HasValue());
    const std::array stages{ShaderArtifactStage{ShaderStage::Vertex, "VSCoverage", {1}},
                            ShaderArtifactStage{ShaderStage::Fragment, variant == Evidence::ForeignStage ? "PSMain" : "PSCoverage", {2}}};
    const auto payload = PackageShaderArtifactStages(target.backend, target.payloadFormat, stages, 1024);
    REQUIRE(payload.HasValue());
    const CompiledShaderArtifact compiled{.backend = target.backend,
                                          .payloadFormat = target.payloadFormat,
                                          .artifactKey = ComputeSha256(std::as_bytes(std::span{payload.Value()})),
                                          .payload = payload.Value()};
    const auto cooked =
        CookReflectedShaderArtifact(compiled, {ShaderCompilerTool::SpirvCross, "scripted-official-schema", compiled.artifactKey},
                                    reflected.Value());
    REQUIRE(cooked.HasValue());
    const auto metadata = EncodeReflectedShaderArtifact(cooked.Value());
    REQUIRE(metadata.HasValue());
    const auto loaded = LoadEditorVulkanMultiviewCoverageCook(compiled.payload, metadata.Value());
    if (loaded.HasError()) {
        INFO(loaded.ErrorValue().message);
        CHECK(variant != Evidence::Exact);
    } else {
        CHECK(variant == Evidence::Exact);
    }
    CHECK(LoadEditorVulkanMultiviewMotionCook(compiled.payload, metadata.Value()).HasError());
}
