#pragma once

#include "Horo/TerrainRender/TerrainMaterialBinding.h"
#include "support/TypedIdentityTestSupport.h"

#include <array>

namespace Horo::Tests::TerrainMaterials {
    using namespace Terrain;
    using namespace Render;
    using namespace TerrainRender;

    template <typename Id> Id Stable(const std::uint8_t value) {
        SerializedTerrainIdentity bytes{};
        bytes.front() = value;
        return Id::Create(bytes).Value();
    }

    inline TerrainConfigurationSnapshot Configuration(const TerrainFeatureTier tier = TerrainFeatureTier::Baseline) {
        return TerrainConfigurationSnapshot::Create({.configuration = IdentityValue<TerrainConfigurationRevision>(1),
                                                     .capability = IdentityValue<TerrainCapabilityRevision>(1),
                                                     .tier = tier,
                                                     .limits = GetTerrainTierProfile(tier).Value().limits})
            .Value();
    }

    inline TerrainMaterialLayerSetData LayerData(const std::uint8_t count = 2) {
        TerrainMaterialLayerSetData data{.id = Stable<TerrainLayerSetId>(1),
                                         .revision = IdentityValue<TerrainContentRevision>(1),
                                         .layerCount = count};
        for (std::uint8_t index = 0; index < count; ++index)
            data.layers[index] = {.id = Stable<TerrainLayerId>(static_cast<std::uint8_t>(index + 1)),
                                  .material = Stable<TerrainMaterialAssetId>(static_cast<std::uint8_t>(index + 1))};
        return data;
    }

    inline ShaderManifest Shader() {
        return {.schemaVersion = 1,
                .sourceIdentity = "shaders.terrain.layers",
                .sourceRevision = 1,
                .entryPoints = {{ShaderStage::Vertex, "TerrainVertex"}, {ShaderStage::Fragment, "TerrainFragment"}},
                .targets = {{ShaderTargetBackend::Null, ShaderPayloadFormat::ValidationFixture, 1, 1, 16, 128}}};
    }

    inline ShaderPermutationModel Permutations() {
        return {.schemaVersion = 1,
                .maximumVariantCount = 128,
                .features = {{0, "TERRAIN_LAYER_BIT_0"},
                             {1, "TERRAIN_LAYER_BIT_1"},
                             {2, "TERRAIN_LAYER_BIT_2"},
                             {3, "TERRAIN_LAYER_BIT_3"},
                             {4, "TERRAIN_HOLES"},
                             {5, "TERRAIN_ALPHA_BIT_0"},
                             {6, "TERRAIN_ALPHA_BIT_1"}},
                .admittedFeatureMasks = {0, 1, 3, 7, 11, 15, 49}};
    }

    inline NormalizedShaderReflection ParameterReflection() {
        NormalizedShaderReflection reflection{.backend = ShaderTargetBackend::Null, .interfaceSchemaVersion = 1};
        reflection.bindings.push_back({StandardPbrParameterIds::Buffer, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly,
                                       1, ShaderStageVisibility::Fragment, true});
        constexpr std::array<std::uint8_t, 8> columns{3, 1, 1, 1, 3, 1, 1, 1};
        std::uint32_t offset = 0;
        for (std::uint32_t id = 1; id <= columns.size(); ++id) {
            reflection.parameters.push_back(
                {ShaderParameterId{id}, StandardPbrParameterIds::Buffer, ShaderValueType::Float32, 1, columns[id - 1], 1, offset});
            offset += columns[id - 1] * 4U;
        }
        return reflection;
    }

    struct Fixture final {
        TerrainConfigurationSnapshot configuration{Configuration()};
        TerrainMaterialLayerSet layers{TerrainMaterialLayerSet::Create(LayerData(), configuration).Value()};
        PreparedTerrainMaterialShader shader{PreparedTerrainMaterialShader::Create(Shader(), Permutations()).Value()};
        ShaderPermutationRequest permutation{.featureMask = 1,
                                             .passId = {4},
                                             .vertexLayoutCompatibility = {{1}},
                                             .target = AssetCookTargetId::Parse("linux-x64").Value()};
        CompiledShaderArtifact artifact{.backend = ShaderTargetBackend::Null,
                                        .payloadFormat = ShaderPayloadFormat::ValidationFixture,
                                        .artifactKey = {{7}},
                                        .payload = {1, 2, 3}};
        ShaderPermutationKey key{ResolveShaderPermutation(shader.Model(), permutation).Value().key};
        TerrainMaterialBindingContext context{.terrain = {.dataset = Stable<TerrainDatasetId>(1), .slot = {1, 1}},
                                              .revisions = {.content = IdentityValue<TerrainContentRevision>(1),
                                                            .residency = IdentityValue<TerrainResidencyRevision>(1),
                                                            .mutation = IdentityValue<TerrainMutationRevision>(1),
                                                            .capability = IdentityValue<TerrainCapabilityRevision>(1)},
                                              .configuration = IdentityValue<TerrainConfigurationRevision>(1),
                                              .renderGeneration = 1};
        std::array<TerrainLayerMaterialInput, 2> materials{};

        Fixture() {
            for (std::size_t index = 0; index < materials.size(); ++index) {
                materials[index] =
                    {.layer = layers.Data().layers[index].id,
                     .asset = layers.Data().layers[index].material,
                     .descriptor = {.id = {index + 1},
                                    .sourceRevision = 1,
                                    .quality = {.minimum = MaterialQualityProfile::Baseline, .preferred = MaterialQualityProfile::High}},
                     .reflection = ParameterReflection(),
                     .resident = {.selectedProfile = MaterialQualityProfile::High, .pipeline = {{1}, 1, 1}, .generation = 1}};
            }
        }

        TerrainMaterialBindingRequest Request() const {
            return {.layers = layers,
                    .configuration = configuration,
                    .shader = shader,
                    .permutation = permutation,
                    .variant = {.key = key,
                                .expectedArtifact = artifact.artifactKey,
                                .artifact = artifact,
                                .pipeline = {{1}, 2, 1},
                                .target = Shader().targets.front()},
                    .materials = materials,
                    .context = context};
        }
    };
}  // namespace Horo::Tests::TerrainMaterials
