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
        {"name":"Tint","type":"vec4","offset":128},
        {"name":"LightDirection","type":"vec4","offset":144},
        {"name":"LightColor","type":"vec4","offset":160}]}},
        "ubos":[{"type":"_1","name":"ViewportParameters","block_size":176,"set":0,"binding":0}])";

    std::array<SpirvReflectionStage, 2> Evidence() {
        return {
            {{ShaderStage::Vertex, "VSMain", "{\"entryPoints\":[{\"name\":\"VSMain\",\"mode\":\"vert\"}]," + std::string{Members} + "}"},
             {ShaderStage::Fragment, "PSMain",
              "{\"entryPoints\":[{\"name\":\"PSMain\",\"mode\":\"frag\"}]," + std::string{Members} + "}"}}};
    }

    struct CookFixture {
        CompiledShaderArtifact compiled;
        ReflectedShaderArtifact reflected;

        CookFixture() {
            const auto stages = Evidence();
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
