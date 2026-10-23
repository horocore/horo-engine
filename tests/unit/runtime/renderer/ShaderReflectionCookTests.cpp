#include "EditorVulkanViewportShader.h"
#include "Horo/Runtime/Render/ShaderArtifact.h"

#include <array>
#include <catch2/catch_test_macros.hpp>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using namespace Horo::Editor;

    // Scripted official JSON schema evidence tests normalization, never native SPIR-V execution.
    constexpr std::string_view Members = R"("types":{"_1":{"members":[
        {"name":"Mvp","type":"mat4","offset":0,"matrix_stride":16},
        {"name":"NormalToWorld","type":"mat4","offset":64,"matrix_stride":16},
        {"name":"ModelToWorld","type":"mat4","offset":128,"matrix_stride":16},
        {"name":"Tint","type":"vec4","offset":192},
        {"name":"CameraAndLightCount","type":"vec4","offset":208},
        {"name":"LightPositionKind","type":"vec4","offset":224,"array":[16],"array_size_is_literal":[true],"array_stride":16},
        {"name":"LightDirectionRange","type":"vec4","offset":480,"array":[16],"array_size_is_literal":[true],"array_stride":16},
        {"name":"LightColorIntensity","type":"vec4","offset":736,"array":[16],"array_size_is_literal":[true],"array_stride":16},
        {"name":"LightCone","type":"vec4","offset":992,"array":[16],"array_size_is_literal":[true],"array_stride":16},
        {"name":"OverlayColors","type":"vec4","offset":1248,"array":[3],"array_size_is_literal":[true],"array_stride":16}]}},
        "ubos":[{"type":"_1","name":"type.ViewportParameters","block_size":1296,"set":0,"binding":0}])";

    std::array<SpirvReflectionStage, 2> Evidence() {
        return {{{ShaderStage::Vertex, "VSMain",
                  "{\"entryPoints\":[{\"name\":\"VSMain\",\"mode\":\"vert\"}],"
                  "\"inputs\":[{\"name\":\"position\",\"type\":\"vec3\",\"location\":0},"
                  "{\"name\":\"normal\",\"type\":\"vec3\",\"location\":1},"
                  "{\"name\":\"uv\",\"type\":\"vec2\",\"location\":2}]," +
                      std::string{Members} + "}"},
                 {ShaderStage::Fragment, "PSMain",
                  "{\"entryPoints\":[{\"name\":\"PSMain\",\"mode\":\"frag\"}]," + std::string{Members} + "}"}}};
    }

    void ReplaceEvidence(std::string &json, const std::string_view before, const std::string_view after) {
        const auto position = json.find(before);
        REQUIRE(position != std::string::npos);
        json.replace(position, before.size(), after);
    }

    struct CookFixture {
        CompiledShaderArtifact compiled;
        ReflectedShaderArtifact reflected;

        explicit CookFixture(const std::array<SpirvReflectionStage, 2> &stages = Evidence()) {
            const auto normalized = NormalizeVulkanReflection(EditorVulkanViewportShaderManifest(), EditorVulkanViewportShaderTarget(),
                                                              stages, EditorVulkanViewportReflectionNames());
            REQUIRE(normalized.HasValue());
            const std::array packageStages{ShaderArtifactStage{ShaderStage::Vertex, "VSMain", {1}},
                                           ShaderArtifactStage{ShaderStage::Fragment, "PSMain", {2}}};
            const auto payload =
                PackageShaderArtifactStages(ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16, packageStages, 1024);
            REQUIRE(payload.HasValue());
            compiled = {.backend = ShaderTargetBackend::Vulkan,
                        .payloadFormat = ShaderPayloadFormat::SpirV16,
                        .artifactKey = ComputeSha256(std::as_bytes(std::span{payload.Value()})),
                        .payload = payload.Value()};
            const auto cooked =
                CookReflectedShaderArtifact(compiled, {ShaderCompilerTool::SpirvCross, "scripted-official-schema", compiled.artifactKey},
                                            normalized.Value());
            REQUIRE(cooked.HasValue());
            reflected = cooked.Value();
        }
    };
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
