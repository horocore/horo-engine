#include "Horo/Runtime/Render/ShaderPermutation.h"
#include "Horo/Runtime/Render/ShaderPermutationErrors.h"

#include <catch2/catch_test_macros.hpp>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    [[nodiscard]] ShaderManifest Manifest() {
        return {.schemaVersion = 1,
                .sourceIdentity = "shaders.standard.surface",
                .sourceRevision = 1,
                .entryPoints = {{ShaderStage::Vertex, "VertexMain"}, {ShaderStage::Fragment, "FragmentMain"}},
                .specializationInputs = {{ShaderSpecializationId{1}, ShaderValueType::Bool32, 0, ShaderStageVisibility::Fragment},
                                         {ShaderSpecializationId{2}, ShaderValueType::Uint32, 4, ShaderStageVisibility::Vertex}},
                .targets = {{ShaderTargetBackend::Vulkan, ShaderPayloadFormat::SpirV16, 1, 1, 16, 128}}};
    }

    [[nodiscard]] ShaderPermutationModel Model() {
        return {.schemaVersion = 1,
                .maximumVariantCount = 4,
                .features = {{0, "USE_NORMAL_MAP"}, {1, "USE_SKINNING"}},
                .admittedFeatureMasks = {0, 1, 3}};
    }

    [[nodiscard]] ShaderPermutationRequest Request() {
        ShaderPermutationRequest request;
        request.featureMask = 1;
        request.passId = RenderPassId{7};
        request.vertexLayoutCompatibility.bytes.front() = 9;
        auto target = AssetCookTargetId::Parse("linux-x86-64");
        REQUIRE(target.HasValue());
        request.target = std::move(target).Value();
        return request;
    }

    template <typename T> void RequireError(const Result<T> &result, const ErrorCodeDescriptor &expected) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().code.Value() == expected.code.Value());
    }
}  // namespace

TEST_CASE("Shader permutations resolve only an explicit finite variant", "[runtime][renderer][shader-permutation]") {
    ShaderPermutationRequest request = Request();
    request.specializationValues = {{ShaderSpecializationId{1}, ShaderValueType::Bool32, 1}};

    const auto result = ResolveShaderPermutation(Manifest(), Model(), request);

    REQUIRE(result.HasValue());
    CHECK(result.Value().key.shaderIdentity == "shaders.standard.surface");
    CHECK(result.Value().key.featureMask == 1);
    REQUIRE(result.Value().specializationValues.size() == 2);
    CHECK(result.Value().specializationValues[0].valueBits == 1);
    CHECK(result.Value().specializationValues[1].valueBits == 4);
}

TEST_CASE("Runtime specialization values do not alter the compile-time key", "[runtime][renderer][shader-permutation]") {
    ShaderPermutationRequest first = Request();
    first.specializationValues = {{ShaderSpecializationId{2}, ShaderValueType::Uint32, 8}};
    ShaderPermutationRequest second = first;
    second.specializationValues.front().valueBits = 16;

    const auto firstResult = ResolveShaderPermutation(Manifest(), Model(), first);
    const auto secondResult = ResolveShaderPermutation(Manifest(), Model(), second);

    REQUIRE(firstResult.HasValue());
    REQUIRE(secondResult.HasValue());
    CHECK(firstResult.Value().key == secondResult.Value().key);
    CHECK(firstResult.Value().specializationValues != secondResult.Value().specializationValues);
}

TEST_CASE("Undeclared feature combinations fail without implicit fallback", "[runtime][renderer][shader-permutation]") {
    ShaderPermutationRequest request = Request();
    request.featureMask = 2;
    RequireError(ResolveShaderPermutation(Manifest(), Model(), request), ShaderPermutationErrors::UnsupportedPermutation);
}

TEST_CASE("Permutation models enforce declared bounds and canonical records", "[runtime][renderer][shader-permutation]") {
    ShaderPermutationModel model = Model();
    model.maximumVariantCount = 2;
    RequireError(ValidateShaderPermutationModel(Manifest(), model), ShaderPermutationErrors::InvalidModel);

    model = Model();
    std::swap(model.features[0], model.features[1]);
    RequireError(ValidateShaderPermutationModel(Manifest(), model), ShaderPermutationErrors::NonCanonicalInput);

    model = Model();
    model.admittedFeatureMasks.push_back(8);
    RequireError(ValidateShaderPermutationModel(Manifest(), model), ShaderPermutationErrors::InvalidModel);

    ShaderPermutationLimits limits;
    limits.maximumVariants = 0;
    RequireError(ValidateShaderPermutationModel(Manifest(), Model(), limits), ShaderPermutationErrors::InvalidLimits);
}

TEST_CASE("Permutation requests require stable logical identities", "[runtime][renderer][shader-permutation]") {
    ShaderPermutationRequest request = Request();
    request.passId = {};
    RequireError(ResolveShaderPermutation(Manifest(), Model(), request), ShaderPermutationErrors::InvalidRequest);

    request = Request();
    request.vertexLayoutCompatibility = {};
    RequireError(ResolveShaderPermutation(Manifest(), Model(), request), ShaderPermutationErrors::InvalidRequest);
}

TEST_CASE("Specialization overrides are declared typed canonical and finite", "[runtime][renderer][shader-permutation]") {
    ShaderPermutationRequest request = Request();
    request.specializationValues = {{ShaderSpecializationId{3}, ShaderValueType::Uint32, 1}};
    RequireError(ResolveShaderPermutation(Manifest(), Model(), request), ShaderPermutationErrors::InvalidSpecialization);

    request.specializationValues = {{ShaderSpecializationId{1}, ShaderValueType::Uint32, 1}};
    RequireError(ResolveShaderPermutation(Manifest(), Model(), request), ShaderPermutationErrors::InvalidSpecialization);

    request.specializationValues = {{ShaderSpecializationId{1}, ShaderValueType::Bool32, 2}};
    RequireError(ResolveShaderPermutation(Manifest(), Model(), request), ShaderPermutationErrors::InvalidSpecialization);

    request.specializationValues = {{ShaderSpecializationId{2}, ShaderValueType::Uint32, 1},
                                    {ShaderSpecializationId{1}, ShaderValueType::Bool32, 1}};
    RequireError(ResolveShaderPermutation(Manifest(), Model(), request), ShaderPermutationErrors::NonCanonicalInput);
}
