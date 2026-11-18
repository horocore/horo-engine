#include "Horo/Runtime/Render/NativeReconstructionKernel.h"
#include "ShaderReflectionCook.h"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
    using namespace Horo::Render;

    /** @brief Scripted producer-schema evidence; no native shader compilation is claimed. */
    struct SampledEvidence final {
        ShaderTargetRequirement target{ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16, 1, 1, 8, 0};
        ShaderManifest
            manifest{.schemaVersion = 1,
                     .sourceIdentity = "tests/shadow-sampling.hlsl",
                     .sourceRevision = 1,
                     .entryPoints = {{ShaderStage::Vertex, "VSMain"}, {ShaderStage::Fragment, "PSMain"}},
                     .bindings =
                         {{{1},
                           ShaderResourceKind::UniformBuffer,
                           ShaderResourceAccess::ReadOnly,
                           1,
                           ShaderStageVisibility::Vertex | ShaderStageVisibility::Fragment},
                          {{2}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment},
                          {{3}, ShaderResourceKind::Sampler, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment}},
                     .parameters = {{{1}, {1}, ShaderValueType::Float32, 4, 4, 1}},
                     .targets = {target}};
        std::vector<ShaderReflectionBindingName> names{{{1}, "type.Frame", {{{1}, "Transform"}}},
                                                       {{2}, "ShadowAtlas", {}},
                                                       {{3}, "ShadowSampler", {}}};
        std::array<SpirvReflectionStage, 2> stages{{{ShaderStage::Vertex, "VSMain",
                                                     R"({"entryPoints":[{"name":"VSMain","mode":"vert"}],
               "types":{"frame":{"members":[{"name":"Transform","type":"mat4","offset":0,"matrix_stride":16}]}},
               "ubos":[{"name":"type.Frame","type":"frame","set":1,"binding":7,"block_size":64}]})"},
                                                    {ShaderStage::Fragment, "PSMain",
                                                     R"({"entryPoints":[{"name":"PSMain","mode":"frag"}],
               "separate_images":[{"name":"ShadowAtlas","type":"texture2D","set":2,"binding":9}],
               "separate_samplers":[{"name":"ShadowSampler","type":"sampler","set":2,"binding":11}]})"}}};

        auto Normalize() const {
            return NormalizeVulkanReflection(manifest, target, stages, names);
        }
    };

    void Replace(std::string &text, const std::string_view before, const std::string_view after) {
        const auto at = text.find(before);
        REQUIRE(at != std::string::npos);
        text.replace(at, before.size(), after);
    }
}  // namespace

TEST_CASE("Vulkan sampled reflection retains actual placements and observed stage masks", "[renderer][shader-cook][shadows]") {
    SampledEvidence evidence;
    const auto result = evidence.Normalize();
    REQUIRE(result.HasValue());
    const auto &reflection = result.Value();
    REQUIRE(reflection.bindings.size() == 3);
    CHECK(reflection.bindings[0].stages == ShaderStageVisibility::Vertex);
    CHECK(reflection.bindings[1].stages == ShaderStageVisibility::Fragment);
    CHECK(reflection.targetBindings[0].nativeSpace == 1);
    CHECK(reflection.targetBindings[0].nativeBinding == 7);
    CHECK(reflection.targetBindings[1].nativeSpace == 2);
    CHECK(reflection.targetBindings[1].nativeBinding == 9);
    CHECK(reflection.targetBindings[2].nativeBinding == 11);
    CHECK_FALSE(reflection.targetBindings[1].pairedSampler.IsValid());
}

TEST_CASE("Vulkan sampled reflection preserves inactive authored resources without inventing slots", "[renderer][shader-cook][shadows]") {
    SampledEvidence evidence;
    evidence.stages[1].json = R"({"entryPoints":[{"name":"PSMain","mode":"frag"}]})";
    const auto result = evidence.Normalize();
    REQUIRE(result.HasValue());
    CHECK_FALSE(result.Value().bindings[1].active);
    CHECK_FALSE(result.Value().bindings[2].active);
    CHECK_FALSE(result.Value().targetBindings[1].active);
    CHECK(result.Value().targetBindings[1].nativeName.empty());
    CHECK(result.Value().targetBindings[1].nativeBinding == 0);
}

TEST_CASE("Vulkan sampled reflection rejects ambiguous foreign and unsupported resource evidence", "[renderer][shader-cook][shadows]") {
    using Mutation = void (*)(SampledEvidence &);
    const std::array<Mutation, 8> mutations{{+[](SampledEvidence &v) {
        Replace(v.stages[1].json, "ShadowAtlas", "UnpublishedAtlas");
    }, +[](SampledEvidence &v) {
        Replace(v.stages[1].json, "texture2D", "textureCube");
    }, +[](SampledEvidence &v) {
        Replace(v.stages[1].json, "\"binding\":9", "\"binding\":9,\"array\":[1]");
    }, +[](SampledEvidence &v) {
        Replace(v.stages[1].json, "\"binding\":9", "\"binding\":-1");
    }, +[](SampledEvidence &v) {
        Replace(v.stages[1].json, "\"set\":2,\"binding\":9", "\"set\":1,\"binding\":7");
    }, +[](SampledEvidence &v) {
        Replace(v.stages[1].json, "ShadowAtlas", "ShadowSampler");
    }, +[](SampledEvidence &v) {
        v.manifest.bindings[1].stages = ShaderStageVisibility::Vertex;
    }, +[](SampledEvidence &v) {
        Replace(v.stages[1].json, "\"separate_images\":[",
                "\"separate_images\":[{\"name\":\"ShadowAtlas\",\"type\":\"texture2D\",\"set\":2,\"binding\":9},");
    }}};
    SampledEvidence evidence;
    mutations[GENERATE(0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U)](evidence);
    CHECK(evidence.Normalize().HasError());
}

TEST_CASE("Vulkan reconstruction reflection proves the closed storage image ABI", "[renderer][shader-cook][reconstruction]") {
    const ShaderTargetRequirement target{ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16, 1, 1, 8, 128, true, true};
    const auto contract = DescribeNativeReconstructionShader(NativeReconstructionEntry::TemporalHistory, target);
    REQUIRE(contract.HasValue());
    auto manifest = contract.Value().manifest;
    auto names = contract.Value().reflectionNames;
    std::array stages{SpirvReflectionStage{ShaderStage::Compute, "ReconstructTemporal", R"({
        "entryPoints":[{"name":"ReconstructTemporal","mode":"comp","workgroup_size":[8,8,1],"workgroup_size_is_spec_constant_id":[false,false,false]}],
        "separate_images":[
            {"name":"CurrentColor","type":"texture2D","set":0,"binding":0},
            {"name":"CurrentDepth","type":"texture2D","set":0,"binding":2},
            {"name":"Motion","type":"texture2D","set":0,"binding":3},
            {"name":"PreviousColor","type":"texture2D","set":0,"binding":4},
            {"name":"PreviousDepth","type":"texture2D","set":0,"binding":5}],
        "images":[
            {"name":"ReconstructedColor","type":"image2D","format":"rgba32f","set":0,"binding":1},
            {"name":"ReconstructedDepth","type":"image2D","format":"r32f","set":0,"binding":6}],
        "push_constants":[{"name":"Dispatch","type":"dispatch","push_constant":true}],
        "types":{"dispatch":{"name":"type.ConstantBuffer.ReconstructionDispatch","members":[{"name":"Extents","type":"uvec4","offset":0},
                                       {"name":"SampleParameters","type":"vec4","offset":16},
                                       {"name":"DepthParameters","type":"vec4","offset":32}]}}})"}};

    SECTION("Canonical reflection is insensitive to authored mapping order") {
        std::ranges::reverse(names);
        const auto result = NormalizeVulkanReflection(manifest, target, stages, names);
        REQUIRE(result.HasValue());
        REQUIRE(result.Value().bindings.size() == 7);
        CHECK(result.Value().bindings[1].kind == ShaderResourceKind::StorageTexture);
        CHECK(result.Value().bindings[6].stages == ShaderStageVisibility::Compute);
        CHECK(result.Value().targetBindings[6].nativeBinding == 6);
        CHECK(result.Value().parameters.empty());
        REQUIRE(manifest.inlineConstants.size() == 1);
        CHECK(manifest.inlineConstants.front().byteSize == sizeof(NativeReconstructionDispatch));
    }
    SECTION("A borrowed name cannot convert a foreign manifest to the storage profile") {
        manifest.sourceIdentity = "tests/other-compute.hlsl";
        CHECK(NormalizeVulkanReflection(manifest, target, stages, names).HasError());
    }
    SECTION("Descriptor, format, member and group tampering fail closed") {
        const auto [before, after] =
            GENERATE(table<std::string_view, std::string_view>({{"rgba32f", "rgba16f"},
                                                                {"\"binding\":6", "\"binding\":8"},
                                                                {"[8,8,1]", "[4,8,1]"},
                                                                {"[false,false,false]", "[true,false,false]"},
                                                                {"\"offset\":32", "\"offset\":24"},
                                                                {"uvec4", "vec4"},
                                                                {"\"push_constant\":true", "\"push_constant\":false"},
                                                                {"PreviousDepth", "PreviousColor"}}));
        Replace(stages[0].json, before, after);
        CHECK(NormalizeVulkanReflection(manifest, target, stages, names).HasError());
    }
}
