#include "ShaderReflectionCook.h"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

namespace {
    using namespace Horo::Render;

    struct RegisterInterface final {
        ShaderTargetRequirement target{ShaderTargetBackend::OpenGL, ShaderPayloadFormat::Glsl410, 1, 1, 8, 0};
        ShaderManifest manifest{.schemaVersion = 1,
                                .sourceIdentity = "tests/register-raster.hlsl",
                                .sourceRevision = 1,
                                .entryPoints = {{ShaderStage::Vertex, "VSMain"}, {ShaderStage::Fragment, "PSMain"}},
                                .bindings = {{{1},
                                              ShaderResourceKind::UniformBuffer,
                                              ShaderResourceAccess::ReadOnly,
                                              1,
                                              ShaderStageVisibility::Vertex | ShaderStageVisibility::Fragment}},
                                .parameters = {{{1}, {1}, ShaderValueType::Float32, 4, 4, 1},
                                               {{2}, {1}, ShaderValueType::Float32, 1, 4, 16}},
                                .targets = {target}};
        std::vector<ShaderReflectionBindingName> names{{{1}, "type.FrameValues", {{{1}, "Transform"}, {{2}, "Values"}}}};

        void WithSampledImage() {
            manifest.bindings.push_back(
                {{2}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment});
            manifest.bindings.push_back(
                {{3}, ShaderResourceKind::Sampler, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment});
            names.push_back({{2}, "InputImage", {}});
            names.push_back({{3}, "InputSampler", {}});
        }
    };
}  // namespace

TEST_CASE("Register raster admission accepts explicit logical interfaces without claiming native packing",
          "[renderer][shader-cook][register-raster]") {
    RegisterInterface input;
    if (GENERATE(false, true)) {
        input.target.backend = ShaderTargetBackend::Metal;
        input.target.payloadFormat = ShaderPayloadFormat::MetalLibrary24;
        input.manifest.targets = {input.target};
    }
    if (GENERATE(false, true))
        input.WithSampledImage();
    REQUIRE(ValidateShaderManifest(input.manifest).HasValue());
    CHECK(ValidateRegisterRasterReflection(input.manifest, input.target, input.names).HasValue());
}

TEST_CASE("Register raster admission rejects ambiguous or foreign authored name evidence", "[renderer][shader-cook][register-raster]") {
    using Mutation = void (*)(RegisterInterface &);
    constexpr std::array<Mutation, 6> corruptions{+[](RegisterInterface &input) {
        input.names[0].parameters[1].name = "Transform";
    }, +[](RegisterInterface &input) {
        input.names[0].parameters[1].id = {1};
    }, +[](RegisterInterface &input) {
        input.names[0].parameters[1].id = {99};
    }, +[](RegisterInterface &input) {
        input.names[0].parameters.pop_back();
    }, +[](RegisterInterface &input) {
        input.names[0].name = "FrameValues; injected()";
    }, +[](RegisterInterface &input) {
        input.names[0].parameters[0].name.assign(257, 'x');
    }};
    const auto index = GENERATE(0U, 1U, 2U, 3U, 4U, 5U);
    RegisterInterface input;
    corruptions[index](input);
    CHECK(ValidateRegisterRasterReflection(input.manifest, input.target, input.names).HasError());
}

TEST_CASE("Register raster admission rejects resource and layout families without implemented native proof",
          "[renderer][shader-cook][register-raster]") {
    using Mutation = void (*)(RegisterInterface &);
    constexpr std::array<Mutation, 6> unsupported{+[](RegisterInterface &input) {
        input.manifest.parameters[0].rows = 3;
    }, +[](RegisterInterface &input) {
        input.manifest.parameters[1].arrayCount = 513;
    }, +[](RegisterInterface &input) {
        input.manifest.bindings[0].kind = ShaderResourceKind::StorageBuffer;
    }, +[](RegisterInterface &input) {
        input.manifest.bindings[0].arrayCount = 2;
    }, +[](RegisterInterface &input) {
        input.WithSampledImage();
        input.manifest.bindings.pop_back();
        input.names.pop_back();
    }, +[](RegisterInterface &input) {
        input.manifest.entryPoints.pop_back();
    }};
    const auto index = GENERATE(0U, 1U, 2U, 3U, 4U, 5U);
    RegisterInterface input;
    unsupported[index](input);
    CHECK(ValidateRegisterRasterReflection(input.manifest, input.target, input.names).HasError());
}
