#include "ShaderReflectionCook.h"

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
