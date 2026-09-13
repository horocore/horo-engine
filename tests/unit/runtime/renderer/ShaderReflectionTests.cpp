#include "Horo/Runtime/Render/ShaderReflection.h"
#include "Horo/Runtime/Render/ShaderReflectionErrors.h"

#include <catch2/catch_test_macros.hpp>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    [[nodiscard]] ShaderTargetRequirement Target() {
        return {.backend = ShaderTargetBackend::OpenGL,
                .payloadFormat = ShaderPayloadFormat::Glsl410,
                .descriptorVersion = 1,
                .interfaceSchemaVersion = 1,
                .maximumBindings = 16,
                .maximumInlineConstantBytes = 128};
    }

    [[nodiscard]] ShaderManifest Manifest() {
        return {.schemaVersion = 1,
                .sourceIdentity = "shaders/material/surface.hlsl",
                .sourceRevision = 12,
                .entryPoints = {{ShaderStage::Vertex, "VertexMain"}, {ShaderStage::Fragment, "FragmentMain"}},
                .bindings = {{ShaderBindingId{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1,
                              ShaderStageVisibility::Vertex | ShaderStageVisibility::Fragment},
                             {ShaderBindingId{2}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1,
                              ShaderStageVisibility::Fragment},
                             {ShaderBindingId{3}, ShaderResourceKind::Sampler, ShaderResourceAccess::ReadOnly, 1,
                              ShaderStageVisibility::Fragment}},
                .parameters = {{ShaderParameterId{1}, ShaderBindingId{1}, ShaderValueType::Float32, 4, 4, 1},
                               {ShaderParameterId{2}, ShaderBindingId{1}, ShaderValueType::Float32, 1, 4, 2}},
                .targets = {Target()}};
    }

    [[nodiscard]] ShaderReflectionCandidate Candidate() {
        return {.backend = ShaderTargetBackend::OpenGL,
                .interfaceSchemaVersion = 1,
                .bindings = {{ShaderBindingId{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1,
                              ShaderStageVisibility::Vertex | ShaderStageVisibility::Fragment, true},
                             {ShaderBindingId{2}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1,
                              ShaderStageVisibility::Fragment, true},
                             {ShaderBindingId{3}, ShaderResourceKind::Sampler, ShaderResourceAccess::ReadOnly, 1,
                              ShaderStageVisibility::Fragment, true}},
                .parameters = {{ShaderParameterId{1}, ShaderBindingId{1}, ShaderValueType::Float32, 4, 4, 1, 0, 0, 16, true, true},
                               {ShaderParameterId{2}, ShaderBindingId{1}, ShaderValueType::Float32, 1, 4, 2, 64, 16, 0, true, true}},
                .stageInterface = {{ShaderStage::Vertex, ShaderInterfaceDirection::Input, 0, ShaderValueType::Float32, 1, 3, 1},
                                   {ShaderStage::Vertex, ShaderInterfaceDirection::Output, 0, ShaderValueType::Float32, 1, 2, 1},
                                   {ShaderStage::Fragment, ShaderInterfaceDirection::Input, 0, ShaderValueType::Float32, 1, 2, 1},
                                   {ShaderStage::Fragment, ShaderInterfaceDirection::Output, 0, ShaderValueType::Float32, 1, 4, 1}},
                .targetBindings = {{ShaderBindingId{1}, 0, 0, 0, "Material", {}, true},
                                   {ShaderBindingId{2}, 0, 0, 1, "Albedo", ShaderBindingId{3}, true},
                                   {ShaderBindingId{3}, 0, 0, 1, "AlbedoSampler", {}, true}},
                .sourceMap = {{"generated.surface", 1, 8, "shaders/material/surface.hlsl", 21, "node.albedo", "pin.color"},
                              {"generated.surface", 9, 12, "shaders/common.hlsli", 3, {}, {}}}};
    }

    template <typename ValueT> void RequireError(const Result<ValueT> &result, const ErrorCodeDescriptor &expected) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().domain.Value() == expected.domain.Value());
        CHECK(result.ErrorValue().code.Value() == expected.code.Value());
    }

    const std::vector<std::string> AdmittedIncludes{"shaders/common.hlsli"};
}  // namespace

TEST_CASE("Shader reflection normalizes final target evidence and preserves inactive logical identities",
          "[runtime][renderer][shader-reflection]") {
    const ShaderManifest manifest = Manifest();
    const ShaderTargetRequirement target = Target();
    ShaderReflectionCandidate candidate = Candidate();
    const auto normalized = NormalizeShaderReflection(manifest, target, candidate, AdmittedIncludes);
    REQUIRE(normalized.HasValue());
    CHECK(normalized.Value().backend == ShaderTargetBackend::OpenGL);
    CHECK(normalized.Value().bindings.size() == manifest.bindings.size());
    CHECK(normalized.Value().parameters.size() == manifest.parameters.size());
    CHECK(normalized.Value().targetBindings[1].pairedSampler == ShaderBindingId{3});

    ShaderReflectionCandidate withHelper = Candidate();
    withHelper.targetBindings.insert(withHelper.targetBindings.begin() + 2, {ShaderBindingId{2}, 1, 0, 2, "Albedo_helper", {}, true});
    const auto helper = NormalizeShaderReflection(manifest, target, withHelper, AdmittedIncludes);
    REQUIRE(helper.HasValue());
    CHECK(helper.Value().targetBindings[2].generatedHelperIndex == 1);

    candidate.bindings[1].active = false;
    candidate.targetBindings[1].active = false;
    candidate.targetBindings[1].nativeBinding = 0;
    candidate.targetBindings[1].nativeName.clear();
    candidate.targetBindings[1].pairedSampler = {};
    const auto inactive = NormalizeShaderReflection(manifest, target, candidate, AdmittedIncludes);
    REQUIRE(inactive.HasValue());
    CHECK_FALSE(inactive.Value().bindings[1].active);
    CHECK(inactive.Value().bindings[1].id == ShaderBindingId{2});
}

TEST_CASE("Shader reflection rejects missing declarations and target-specific binding failures", "[runtime][renderer][shader-reflection]") {
    const ShaderManifest manifest = Manifest();
    const ShaderTargetRequirement target = Target();

    ShaderReflectionCandidate missing = Candidate();
    missing.bindings.pop_back();
    RequireError(NormalizeShaderReflection(manifest, target, missing, AdmittedIncludes), ShaderReflectionErrors::ManifestMismatch);

    ShaderReflectionCandidate unnamed = Candidate();
    unnamed.targetBindings[1].nativeName.clear();
    RequireError(NormalizeShaderReflection(manifest, target, unnamed, AdmittedIncludes), ShaderReflectionErrors::TargetMappingInvalid);

    ShaderReflectionCandidate collision = Candidate();
    collision.targetBindings[1].pairedSampler = {};
    RequireError(NormalizeShaderReflection(manifest, target, collision, AdmittedIncludes), ShaderReflectionErrors::NativeBindingCollision);
}

TEST_CASE("Shader reflection rejects overlapping packing and incompatible stage links", "[runtime][renderer][shader-reflection]") {
    const ShaderManifest manifest = Manifest();
    const ShaderTargetRequirement target = Target();

    ShaderReflectionCandidate overlap = Candidate();
    overlap.parameters[1].byteOffset = 48;
    RequireError(NormalizeShaderReflection(manifest, target, overlap, AdmittedIncludes), ShaderReflectionErrors::LayoutMismatch);

    ShaderReflectionCandidate link = Candidate();
    link.stageInterface[2].columns = 3;
    RequireError(NormalizeShaderReflection(manifest, target, link, AdmittedIncludes), ShaderReflectionErrors::StageInterfaceMismatch);

    ShaderReflectionCandidate unordered = Candidate();
    std::swap(unordered.stageInterface[1], unordered.stageInterface[2]);
    RequireError(NormalizeShaderReflection(manifest, target, unordered, AdmittedIncludes), ShaderReflectionErrors::InvalidReflection);
}

TEST_CASE("Shader reflection accepts bounded row-major matrix layouts", "[runtime][renderer][shader-reflection]") {
    ShaderManifest manifest = Manifest();
    ShaderReflectionCandidate candidate = Candidate();
    manifest.parameters[0].rows = 3;
    manifest.parameters[0].columns = 4;
    candidate.parameters[0].rows = 3;
    candidate.parameters[0].columns = 4;
    candidate.parameters[0].columnMajor = false;
    candidate.parameters[0].matrixStride = 16;

    const auto normalized = NormalizeShaderReflection(manifest, Target(), candidate, AdmittedIncludes);
    REQUIRE(normalized.HasValue());
    CHECK_FALSE(normalized.Value().parameters[0].columnMajor);
}

TEST_CASE("Shader source mapping preserves authored graph provenance and explicit generated fallback",
          "[runtime][renderer][shader-reflection]") {
    const ShaderReflectionCandidate candidate = Candidate();
    const auto mapped = MapShaderSourceLocation({"generated.surface", 4, 9}, candidate.sourceMap);
    REQUIRE(mapped.HasValue());
    CHECK(mapped.Value().mapped);
    CHECK(mapped.Value().sourceIdentity == "shaders/material/surface.hlsl");
    CHECK(mapped.Value().line == 24);
    CHECK(mapped.Value().column == 9);
    CHECK(mapped.Value().graphNodeIdentity == "node.albedo");
    CHECK(mapped.Value().graphPinIdentity == "pin.color");

    const auto firstRangeEnd = MapShaderSourceLocation({"generated.surface", 8, 1}, candidate.sourceMap);
    REQUIRE(firstRangeEnd.HasValue());
    CHECK(firstRangeEnd.Value().mapped);
    CHECK(firstRangeEnd.Value().sourceIdentity == "shaders/material/surface.hlsl");

    const auto secondRangeBegin = MapShaderSourceLocation({"generated.surface", 9, 1}, candidate.sourceMap);
    REQUIRE(secondRangeBegin.HasValue());
    CHECK(secondRangeBegin.Value().mapped);
    CHECK(secondRangeBegin.Value().sourceIdentity == "shaders/common.hlsli");

    const auto generated = MapShaderSourceLocation({"generated.surface", 99, 2}, candidate.sourceMap);
    REQUIRE(generated.HasValue());
    CHECK_FALSE(generated.Value().mapped);
    CHECK(generated.Value().sourceIdentity == "generated.surface");
    CHECK(generated.Value().line == 99);
    CHECK(generated.Value().column == 2);
}

TEST_CASE("Shader reflection enforces bounded canonical source mappings", "[runtime][renderer][shader-reflection]") {
    const ShaderManifest manifest = Manifest();
    const ShaderTargetRequirement target = Target();
    ShaderReflectionCandidate candidate = Candidate();
    candidate.sourceMap[1].generatedLineBegin = 8;
    RequireError(NormalizeShaderReflection(manifest, target, candidate, AdmittedIncludes), ShaderReflectionErrors::SourceMapInvalid);

    candidate = Candidate();
    candidate.sourceMap[0].graphNodeIdentity.clear();
    RequireError(NormalizeShaderReflection(manifest, target, candidate, AdmittedIncludes), ShaderReflectionErrors::SourceMapInvalid);

    candidate = Candidate();
    candidate.sourceMap[1].sourceIdentity = "shaders/unadmitted.hlsli";
    RequireError(NormalizeShaderReflection(manifest, target, candidate, AdmittedIncludes), ShaderReflectionErrors::SourceMapInvalid);

    ShaderReflectionLimits limits;
    limits.maximumBindings = 0;
    RequireError(NormalizeShaderReflection(manifest, target, Candidate(), AdmittedIncludes, limits), ShaderReflectionErrors::InvalidLimits);
}
