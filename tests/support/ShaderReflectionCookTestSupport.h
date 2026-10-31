#pragma once
/** @file ShaderReflectionCookTestSupport.h
 * @brief Canonical scripted reflection evidence and production cook pipeline shared by renderer/application tests.
 */
#include "EditorVulkanViewportShader.h"
#include "Horo/Runtime/Render/ShaderArtifact.h"

#include <catch2/catch_test_macros.hpp>

namespace Horo::Tests::ShaderCook {
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

    inline std::array<SpirvReflectionStage, 2> Evidence() {
        return {{{ShaderStage::Vertex, "VSMain",
                  "{\"entryPoints\":[{\"name\":\"VSMain\",\"mode\":\"vert\"}],"
                  "\"inputs\":[{\"name\":\"position\",\"type\":\"vec3\",\"location\":0},"
                  "{\"name\":\"normal\",\"type\":\"vec3\",\"location\":1},"
                  "{\"name\":\"uv\",\"type\":\"vec2\",\"location\":2}]," +
                      std::string{Members} + "}"},
                 {ShaderStage::Fragment, "PSMain",
                  "{\"entryPoints\":[{\"name\":\"PSMain\",\"mode\":\"frag\"}]," + std::string{Members} + "}"}}};
    }

    /** @brief Builds scripted final multiview reflection with the exact canonical matrix/layer uniform ABI. */
    inline std::array<SpirvReflectionStage, 2> MultiviewEvidence() {
        return {{{ShaderStage::Vertex, "VSMain", R"({"entryPoints":[{"name":"VSMain","mode":"vert"}],
          "inputs":[{"name":"position","type":"vec3","location":0}],
          "types":{"_1":{"members":[
            {"name":"WorldToClip","type":"mat4","offset":0,"matrix_stride":16,"array":[32],"array_size_is_literal":[true],"array_stride":64},
            {"name":"LocalToWorld","type":"mat4","offset":2048,"matrix_stride":16},
            {"name":"ViewLayers","type":"uint","offset":2112,"array":[32],"array_size_is_literal":[true],"array_stride":16}]}},
          "ubos":[{"type":"_1","name":"type.MultiviewParameters","block_size":2624,"set":0,"binding":0}]})"},
                 {ShaderStage::Fragment, "PSMain", R"({"entryPoints":[{"name":"PSMain","mode":"frag"}],
          "outputs":[{"name":"color","type":"vec4","location":0}]})"}}};
    }

    inline void ReplaceEvidence(std::string &json, const std::string_view before, const std::string_view after) {
        const auto position = json.find(before);
        REQUIRE(position != std::string::npos);
        json.replace(position, before.size(), after);
    }

    struct CookFixture {
        CompiledShaderArtifact compiled;
        ReflectedShaderArtifact reflected;

        explicit CookFixture(const std::array<SpirvReflectionStage, 2> &stages = Evidence(),
                             const std::span<const ShaderArtifactStage> nativeStages = {}, const bool multiview = false) {
            const auto normalized =
                NormalizeVulkanReflection(multiview ? EditorVulkanMultiviewShaderManifest() : EditorVulkanViewportShaderManifest(),
                                          EditorVulkanViewportShaderTarget(), stages,
                                          multiview ? EditorVulkanMultiviewReflectionNames() : EditorVulkanViewportReflectionNames());
            REQUIRE(normalized.HasValue());
            const std::array packageStages{ShaderArtifactStage{ShaderStage::Vertex, "VSMain", {1}},
                                           ShaderArtifactStage{ShaderStage::Fragment, "PSMain", {2}}};
            const auto payload =
                PackageShaderArtifactStages(ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16,
                                            nativeStages.empty() ? std::span<const ShaderArtifactStage>{packageStages} : nativeStages,
                                            1024);
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
}  // namespace Horo::Tests::ShaderCook
