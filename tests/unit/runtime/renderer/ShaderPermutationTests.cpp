#include "Horo/Runtime/Render/ShaderPermutation.h"
#include "Horo/Runtime/Render/ShaderPermutationErrors.h"
#include "RendererTestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <utility>

namespace {
    using namespace Horo;
    using namespace Horo::Render;
    using Testing::RequireError;

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

    [[nodiscard]] PreparedShaderPermutationModel Prepared() {
        auto prepared = PrepareShaderPermutationModel(Manifest(), Model());
        REQUIRE(prepared.HasValue());
        return std::move(prepared).Value();
    }
}  // namespace

TEST_CASE("Shader permutations resolve only an explicit finite variant", "[runtime][renderer][shader-permutation]") {
    ShaderPermutationRequest request = Request();
    request.specializationValues = {{ShaderSpecializationId{1}, ShaderValueType::Bool32, 1}};

    const auto result = ResolveShaderPermutation(Prepared(), request);

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

    const PreparedShaderPermutationModel prepared = Prepared();
    const auto firstResult = ResolveShaderPermutation(prepared, first);
    const auto secondResult = ResolveShaderPermutation(prepared, second);

    REQUIRE(firstResult.HasValue());
    REQUIRE(secondResult.HasValue());
    CHECK(firstResult.Value().key == secondResult.Value().key);
    CHECK(firstResult.Value().specializationValues != secondResult.Value().specializationValues);
}

TEST_CASE("Prepared permutations own their validated loading snapshot", "[runtime][renderer][shader-permutation]") {
    ShaderManifest manifest = Manifest();
    ShaderPermutationModel model = Model();
    auto prepared = PrepareShaderPermutationModel(manifest, model);
    REQUIRE(prepared.HasValue());

    manifest.specializationInputs.clear();
    model.admittedFeatureMasks.clear();

    CHECK(ResolveShaderPermutation(prepared.Value(), Request()).HasValue());
}

TEST_CASE("Undeclared feature combinations fail without implicit fallback", "[runtime][renderer][shader-permutation]") {
    ShaderPermutationRequest request = Request();
    request.featureMask = 2;
    RequireError(ResolveShaderPermutation(Prepared(), request), ShaderPermutationErrors::UnsupportedPermutation);
}

TEST_CASE("Permutation models enforce declared bounds and canonical records", "[runtime][renderer][shader-permutation]") {
    ShaderPermutationModel model = Model();
    model.maximumVariantCount = 2;
    RequireError(ValidateShaderPermutationModel(Manifest(), model), ShaderPermutationErrors::InvalidModel);

    model = Model();
    std::swap(model.features[0], model.features[1]);
    RequireError(ValidateShaderPermutationModel(Manifest(), model), ShaderPermutationErrors::NonCanonicalInput);

    model = Model();
    model.features[0].defineName = "USE_SKINNING";
    model.features[1].defineName = "USE_NORMAL_MAP";
    CHECK(ValidateShaderPermutationModel(Manifest(), model).HasValue());

    model.features[1].defineName = "USE_SKINNING";
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
    RequireError(ResolveShaderPermutation(Prepared(), request), ShaderPermutationErrors::InvalidRequest);

    request = Request();
    request.vertexLayoutCompatibility = {};
    RequireError(ResolveShaderPermutation(Prepared(), request), ShaderPermutationErrors::InvalidRequest);
}

TEST_CASE("Specialization overrides are declared typed canonical and finite", "[runtime][renderer][shader-permutation]") {
    ShaderPermutationRequest request = Request();
    request.specializationValues = {{ShaderSpecializationId{3}, ShaderValueType::Uint32, 1}};
    const PreparedShaderPermutationModel prepared = Prepared();
    RequireError(ResolveShaderPermutation(prepared, request), ShaderPermutationErrors::InvalidSpecialization);

    request.specializationValues = {{ShaderSpecializationId{1}, ShaderValueType::Uint32, 1}};
    RequireError(ResolveShaderPermutation(prepared, request), ShaderPermutationErrors::InvalidSpecialization);

    request.specializationValues = {{ShaderSpecializationId{1}, ShaderValueType::Bool32, 2}};
    RequireError(ResolveShaderPermutation(prepared, request), ShaderPermutationErrors::InvalidSpecialization);

    request.specializationValues = {{ShaderSpecializationId{2}, ShaderValueType::Uint32, 1},
                                    {ShaderSpecializationId{1}, ShaderValueType::Bool32, 1}};
    RequireError(ResolveShaderPermutation(prepared, request), ShaderPermutationErrors::NonCanonicalInput);
}
