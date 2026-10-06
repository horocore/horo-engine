#include "Horo/Runtime/Render/ShaderPermutationErrors.h"
#include "Horo/Runtime/Render/StandardPbrMaterialErrors.h"
#include "support/AllocationProbe.h"
#include "support/TerrainMaterialFixtures.h"

#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <limits>
#include <thread>

using namespace Horo;
using namespace Horo::Terrain;
using namespace Horo::Render;
using namespace Horo::TerrainRender;
using namespace Horo::Tests::TerrainMaterials;
using Horo::Tests::RequireError;

TEST_CASE("Host composes terrain layers with real PBR packing and exact cooked shader admission", "[terrain][render][material]") {
    Fixture fixture;
    TerrainMaterialBindingOwner owner;
    const auto published = owner.Publish(fixture.Request(), fixture.context, 0);
    REQUIRE(published.HasValue());
    const auto &binding = *published.Value();
    CHECK(binding.publication == 1);
    CHECK(binding.layers == fixture.layers.Data());
    CHECK(binding.permutation == fixture.key);
    REQUIRE(binding.materials.size() == 2);
    CHECK(binding.materials[0].parameterBytes.size() == 48);
    CHECK(binding.materials[0].selectedProfile == MaterialQualityProfile::High);
    CHECK(fixture.configuration.Data().tier == TerrainFeatureTier::Baseline);
    fixture.materials[0].descriptor.parameters.albedo.x = 0.1F;
    CHECK(binding.materials[0].parameterBytes != PrepareTerrainMaterialBinding(fixture.Request()).Value().materials[0].parameterBytes);
}

TEST_CASE("Terrain shader vocabulary and exact count hole classification masks cannot drift", "[terrain][render][material]") {
    auto model = Permutations();
    model.features[4].defineName = "UNRELATED_FEATURE";
    RequireError(PreparedTerrainMaterialShader::Create(Shader(), model), TerrainMaterialBindingErrors::Invalid);
    model = Permutations();
    model.maximumVariantCount = 129;
    RequireError(PreparedTerrainMaterialShader::Create(Shader(), model), TerrainMaterialBindingErrors::Invalid);
    RequireError(TerrainMaterialFeatureMask(0, false, MaterialAlphaMode::Opaque), TerrainMaterialBindingErrors::Invalid);
    CHECK(TerrainMaterialFeatureMask(2, true, MaterialAlphaMode::Opaque).Value() == 17);
    CHECK(TerrainMaterialFeatureMask(2, true, MaterialAlphaMode::Masked).Value() == 49);
    CHECK(TerrainMaterialFeatureMask(16, false, MaterialAlphaMode::Additive).Value() == 111);
}

TEST_CASE("Missing mismatched and foreign terrain variants fail before publication", "[terrain][render][material]") {
    Fixture fixture;
    auto request = fixture.Request();
    request.permutation.featureMask = 3;
    RequireError(PrepareTerrainMaterialBinding(request), TerrainMaterialBindingErrors::VariantUnavailable);
    request.permutation = fixture.permutation;
    request.variant.key.featureMask = 0;
    RequireError(PrepareTerrainMaterialBinding(request), TerrainMaterialBindingErrors::VariantUnavailable);
    request.variant.key = fixture.key;
    request.variant.expectedArtifact = {{8}};
    RequireError(PrepareTerrainMaterialBinding(request), TerrainMaterialBindingErrors::VariantUnavailable);
    request.variant.expectedArtifact = fixture.artifact.artifactKey;
    request.variant.pass = TerrainMaterialPass::Depth;
    RequireError(PrepareTerrainMaterialBinding(request), TerrainMaterialBindingErrors::VariantUnavailable);
    request.variant.pass = TerrainMaterialPass::Color;
    request.variant.target.backend = ShaderTargetBackend::Metal;
    RequireError(PrepareTerrainMaterialBinding(request), TerrainMaterialBindingErrors::VariantUnavailable);
    fixture.artifact.payload.clear();
    RequireError(PrepareTerrainMaterialBinding(fixture.Request()), TerrainMaterialBindingErrors::VariantUnavailable);
}

TEST_CASE("Terrain admission rejects undeclared target capacity and missing exact permutation", "[terrain][render][material]") {
    Fixture fixture;
    auto request = fixture.Request();
    ++request.variant.target.maximumBindings;
    RequireError(PrepareTerrainMaterialBinding(request), TerrainMaterialBindingErrors::VariantUnavailable);
    request.variant.target = Shader().targets.front();
    fixture.materials[0].reflection.interfaceSchemaVersion = 2;
    RequireError(PrepareTerrainMaterialBinding(request), TerrainMaterialBindingErrors::Invalid);
    fixture.materials[0].reflection.interfaceSchemaVersion = 1;
    auto model = Permutations();
    model.admittedFeatureMasks = {0};
    const auto shader = PreparedTerrainMaterialShader::Create(Shader(), model).Value();
    auto unavailable = fixture.Request();
    const TerrainMaterialBindingRequest missing{.layers = unavailable.layers,
                                                .configuration = unavailable.configuration,
                                                .shader = shader,
                                                .permutation = unavailable.permutation,
                                                .variant = unavailable.variant,
                                                .materials = unavailable.materials,
                                                .context = unavailable.context};
    RequireError(PrepareTerrainMaterialBinding(missing), ShaderPermutationErrors::UnsupportedPermutation);
}

TEST_CASE("Terrain materials preserve actual PBR missing texture reflection and quality failures", "[terrain][render][material]") {
    Fixture fixture;
    fixture.materials[1].reflection.parameters.clear();
    RequireError(PrepareTerrainMaterialBinding(fixture.Request()), StandardPbrMaterialErrors::ReflectionMismatch);
    fixture.materials[1].reflection = ParameterReflection();
    fixture.materials[1].descriptor.quality.requiredFeatures = StandardPbrFeature::NormalTexture;
    fixture.materials[1].descriptor.features = StandardPbrFeature::NormalTexture;
    RequireError(PrepareTerrainMaterialBinding(fixture.Request()), StandardPbrMaterialErrors::UnsupportedQuality);
    fixture.materials[1].descriptor.quality.requiredFeatures = StandardPbrFeature::None;
    fixture.materials[1].descriptor.features = StandardPbrFeature::None;
    fixture.materials[1].resident.selectedProfile = MaterialQualityProfile::Baseline;
    RequireError(PrepareTerrainMaterialBinding(fixture.Request()), StandardPbrMaterialErrors::UnsupportedQuality);
    fixture.materials[1].descriptor.quality.authoredFallbacks = {MaterialQualityProfile::Baseline};
    REQUIRE(PrepareTerrainMaterialBinding(fixture.Request()).HasValue());
}

TEST_CASE("Terrain replacement rollback cancellation and retained snapshots protect lifetime", "[terrain][render][material]") {
    Fixture fixture;
    TerrainMaterialBindingOwner owner;
    const auto first = owner.Publish(fixture.Request(), fixture.context, 0).Value();
    fixture.materials[0].descriptor.parameters.roughness = 2.0F;
    RequireError(owner.Publish(fixture.Request(), fixture.context, 1), StandardPbrMaterialErrors::InvalidDescriptor);
    CHECK(owner.Snapshot().Value() == first);
    fixture.materials[0].descriptor.parameters.roughness = 0.2F;
    CancellationSource cancellation;
    cancellation.RequestCancellation();
    RequireError(owner.Publish(fixture.Request(), fixture.context, 1, cancellation.Token()), TerrainErrors::CompositionCancelled);
    CHECK(owner.Snapshot().Value() == first);
    const auto next = owner.Publish(fixture.Request(), fixture.context, 1).Value();
    CHECK(next->publication == 2);
    CHECK(first->publication == 1);
    CHECK(first->materials[0].parameterBytes != next->materials[0].parameterBytes);
    RequireError(owner.Publish(fixture.Request(), fixture.context, 1), TerrainErrors::RevisionStale);
    REQUIRE(owner.Shutdown().HasValue());
    REQUIRE(owner.Shutdown().HasValue());
    CHECK(next->layers.layerCount == 2);
    RequireError(owner.Publish(fixture.Request(), fixture.context, 2), TerrainErrors::LifecycleUnavailable);
    RequireError(owner.Snapshot(), TerrainErrors::LifecycleUnavailable);
}

TEST_CASE("Terrain host generation fences and owner lane reject stale replacement", "[terrain][render][material]") {
    Fixture fixture;
    TerrainMaterialBindingOwner owner;
    auto context = fixture.context;
    ++context.renderGeneration;
    RequireError(owner.Publish(fixture.Request(), context, 0), TerrainErrors::RevisionStale);
    auto request = fixture.Request();
    request.context.lifecycle = TerrainRuntimeLifecycle::Closing;
    RequireError(PrepareTerrainMaterialBinding(request), TerrainErrors::LifecycleUnavailable);
    request.context = fixture.context;
    request.context.revisions.capability = Horo::Tests::IdentityValue<TerrainCapabilityRevision>(2);
    RequireError(PrepareTerrainMaterialBinding(request), TerrainErrors::RevisionStale);
    fixture.materials[0].asset = Stable<TerrainMaterialAssetId>(9);
    RequireError(PrepareTerrainMaterialBinding(fixture.Request()), TerrainMaterialBindingErrors::Invalid);
    bool rejected = false;
    std::thread other([&] {
        const auto result = owner.Snapshot();
        rejected = result.HasError() && result.ErrorValue().code.Value() == TerrainMaterialBindingErrors::WrongThread.code.Value();
    });
    other.join();
    CHECK(rejected);
}

TEST_CASE("Terrain material allocation failure preserves the current publication", "[terrain][render][material]") {
    Fixture fixture;
    TerrainMaterialBindingOwner owner;
    const auto current = owner.Publish(fixture.Request(), fixture.context, 0).Value();
    const auto request = fixture.Request();
    const auto failed = [&] {
        Horo::Tests::AllocationProbe::ScopedFailure failure;
        return owner.Publish(request, fixture.context, 1);
    }();
    REQUIRE(failed.HasError());
    CHECK(owner.Snapshot().Value() == current);
}

TEST_CASE("Canonical terrain blending preserves linear PBR and normalized tangent normals", "[terrain][render][material]") {
    Fixture fixture;
    const auto weights = NormalizeTerrainMaterialWeights(std::array<std::uint32_t, 2>{1, 3}).Value();
    std::array<TerrainPbrLayerSample, 2> samples{
        {{.albedo = {1.0F, 0.0F, 0.0F}, .normal = {1.0F, 0.0F, 0.0F}, .metallic = 1.0F, .roughness = 0.2F},
         {.albedo = {0.0F, 0.0F, 1.0F}, .normal = {0.0F, 0.0F, 1.0F}, .roughness = 0.6F}}};
    const auto blended = BlendTerrainPbrSamples(fixture.layers, weights, 3, samples);
    REQUIRE(blended.HasValue());
    CHECK(std::abs(blended.Value().albedo.x - 0.25F) < 0.0001F);
    CHECK(std::abs(blended.Value().roughness - 0.5F) < 0.0001F);
    CHECK(std::abs(blended.Value().normal.x - 0.31623F) < 0.0001F);
    CHECK(std::abs(blended.Value().normal.z - 0.94868F) < 0.0001F);
    RequireError(BlendTerrainPbrSamples(fixture.layers, weights, 1, samples), TerrainErrors::DescriptorInvalid);
    samples[1].roughness = std::numeric_limits<float>::quiet_NaN();
    RequireError(BlendTerrainPbrSamples(fixture.layers, weights, 3, samples), TerrainMaterialBindingErrors::Invalid);
    samples[1].roughness = 0.6F;
    samples[0].normal = {0.0F, 0.0F, 1.0F};
    samples[1].normal = {0.0F, 0.0F, -1.0F};
    TerrainMaterialWeights opposite{.layerCount = 2, .values = {32767, 32768}};
    // Quantized near-cancellation is still a defined finite direction; exact degenerate input is rejected before blending.
    CHECK(BlendTerrainPbrSamples(fixture.layers, opposite, 3, samples).HasValue());
    samples[1].normal = {};
    RequireError(BlendTerrainPbrSamples(fixture.layers, weights, 3, samples), TerrainMaterialBindingErrors::Invalid);
}

TEST_CASE("Terrain publication retains runtime specialization outside the compile key", "[terrain][render][material]") {
    Fixture fixture;
    auto manifest = Shader();
    manifest.specializationInputs = {{ShaderSpecializationId{1}, ShaderValueType::Uint32, 4, ShaderStageVisibility::Fragment}};
    const auto shader = PreparedTerrainMaterialShader::Create(manifest, Permutations()).Value();
    auto permutation = fixture.permutation;
    permutation.specializationValues = {{ShaderSpecializationId{1}, ShaderValueType::Uint32, 8}};
    const auto original = fixture.Request();
    const TerrainMaterialBindingRequest request{.layers = original.layers,
                                                .configuration = original.configuration,
                                                .shader = shader,
                                                .permutation = permutation,
                                                .variant = original.variant,
                                                .materials = original.materials,
                                                .context = original.context};
    const auto prepared = PrepareTerrainMaterialBinding(request);
    REQUIRE(prepared.HasValue());
    CHECK(prepared.Value().permutation == fixture.key);
    REQUIRE(prepared.Value().specializationValues.size() == 1);
    CHECK(prepared.Value().specializationValues[0].valueBits == 8);
}
