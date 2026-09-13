#include "Horo/Runtime/Render/StandardPbrMaterial.h"
#include "Horo/Runtime/Render/StandardPbrMaterialErrors.h"

#include <array>
#include <bit>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {
    using namespace Horo;
    using namespace Horo::Render;

    [[nodiscard]] NormalizedShaderReflection Reflection() {
        return {
            .backend = ShaderTargetBackend::Vulkan,
            .interfaceSchemaVersion = 3,
            .interfaceCompatibility = {},
            .bindings = {{StandardPbrParameterIds::Buffer, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1,
                          ShaderStageVisibility::Fragment, true}},
            .parameters =
                {
                    {StandardPbrParameterIds::Albedo, StandardPbrParameterIds::Buffer, ShaderValueType::Float32, 1, 3, 1, 0, 0, 0, true,
                     true},
                    {StandardPbrParameterIds::Metallic, StandardPbrParameterIds::Buffer, ShaderValueType::Float32, 1, 1, 1, 12, 0, 0, true,
                     true},
                    {StandardPbrParameterIds::Roughness, StandardPbrParameterIds::Buffer, ShaderValueType::Float32, 1, 1, 1, 16, 0, 0, true,
                     true},
                    {StandardPbrParameterIds::Occlusion, StandardPbrParameterIds::Buffer, ShaderValueType::Float32, 1, 1, 1, 20, 0, 0, true,
                     true},
                    {StandardPbrParameterIds::Emissive, StandardPbrParameterIds::Buffer, ShaderValueType::Float32, 1, 3, 1, 32, 0, 0, true,
                     true},
                    {StandardPbrParameterIds::EmissiveIntensity, StandardPbrParameterIds::Buffer, ShaderValueType::Float32, 1, 1, 1, 44, 0,
                     0, true, true},
                    {StandardPbrParameterIds::Opacity, StandardPbrParameterIds::Buffer, ShaderValueType::Float32, 1, 1, 1, 48, 0, 0, true,
                     true},
                    {StandardPbrParameterIds::OpacityMaskThreshold, StandardPbrParameterIds::Buffer, ShaderValueType::Float32, 1, 1, 1, 52,
                     0, 0, true, true},
                },
        };
    }

    [[nodiscard]] StandardPbrMaterialDescriptor Descriptor() {
        return {
            .id = {41},
            .sourceRevision = 7,
            .parameters = {},
            .alphaMode = MaterialAlphaMode::Opaque,
            .features = StandardPbrFeature::None,
            .quality = {.minimum = MaterialQualityProfile::Baseline,
                        .preferred = MaterialQualityProfile::Standard,
                        .requiredFeatures = StandardPbrFeature::None},
        };
    }

    [[nodiscard]] RenderResourceOwnerId Owner() {
        return {9};
    }

    [[nodiscard]] RenderPipelineHandle Pipeline() {
        return {Owner(), 2, 4};
    }

    [[nodiscard]] StandardPbrResidentInputs Inputs(const std::span<const StandardPbrTextureBinding> textures = {}) {
        return {.selectedProfile = MaterialQualityProfile::Standard,
                .effectiveFeatures = StandardPbrFeature::None,
                .pipeline = Pipeline(),
                .textures = textures,
                .generation = 3};
    }

    [[nodiscard]] float ReadFloat(const std::vector<std::byte> &bytes, const std::size_t offset) {
        std::uint32_t bits = 0;
        for (std::size_t byte = 0; byte < sizeof(bits); ++byte)
            bits |= std::to_integer<std::uint32_t>(bytes[offset + byte]) << (byte * 8U);
        return std::bit_cast<float>(bits);
    }

    template <typename ValueT> void RequireError(const Result<ValueT> &result, const ErrorCodeDescriptor &expected) {
        REQUIRE(result.HasError());
        CHECK(result.ErrorValue().domain.Value() == expected.domain.Value());
        CHECK(result.ErrorValue().code.Value() == expected.code.Value());
        CHECK_FALSE(result.ErrorValue().message.empty());
        CHECK_FALSE(expected.remediationHint.empty());
    }
}  // namespace

TEST_CASE("Standard PBR packs semantic values from final target reflection", "[runtime][renderer][material]") {
    auto descriptor = Descriptor();
    descriptor.parameters = {
        .albedo = {0.1F, 0.2F, 0.3F},
        .metallic = 0.4F,
        .roughness = 0.5F,
        .occlusion = 0.6F,
        .emissive = {0.7F, 0.8F, 0.9F},
        .emissiveIntensity = 2.0F,
        .opacity = 0.75F,
    };

    const auto prepared = PrepareStandardPbrMaterial(descriptor, Reflection(), Inputs());
    REQUIRE(prepared.HasValue());
    CHECK(prepared.Value().id == descriptor.id);
    CHECK(prepared.Value().sourceRevision == descriptor.sourceRevision);
    CHECK(prepared.Value().generation == 3);
    CHECK(prepared.Value().pipeline == Pipeline());
    REQUIRE(prepared.Value().parameterBytes.size() == 56);
    CHECK(ReadFloat(prepared.Value().parameterBytes, 0) == 0.1F);
    CHECK(ReadFloat(prepared.Value().parameterBytes, 4) == 0.2F);
    CHECK(ReadFloat(prepared.Value().parameterBytes, 8) == 0.3F);
    CHECK(ReadFloat(prepared.Value().parameterBytes, 12) == 0.4F);
    CHECK(ReadFloat(prepared.Value().parameterBytes, 16) == 0.5F);
    CHECK(ReadFloat(prepared.Value().parameterBytes, 20) == 0.6F);
    CHECK(ReadFloat(prepared.Value().parameterBytes, 32) == 0.7F);
    CHECK(ReadFloat(prepared.Value().parameterBytes, 44) == 2.0F);
    CHECK(ReadFloat(prepared.Value().parameterBytes, 48) == 0.75F);
    CHECK(ReadFloat(prepared.Value().parameterBytes, 52) == 0.0F);
}

TEST_CASE("Standard PBR accepts only explicit quality fallback and required features", "[runtime][renderer][material]") {
    auto descriptor = Descriptor();
    descriptor.quality.preferred = MaterialQualityProfile::High;
    descriptor.quality.authoredFallbacks = {MaterialQualityProfile::Baseline};
    descriptor.features = StandardPbrFeature::NormalTexture;
    descriptor.quality.requiredFeatures = StandardPbrFeature::NormalTexture;
    const std::array textures{StandardPbrTextureBinding{StandardPbrTextureRole::Normal, {Owner(), 4, 2}, {Owner(), 7, 1}}};

    auto fallbackInputs = Inputs(textures);
    fallbackInputs.selectedProfile = MaterialQualityProfile::Baseline;
    fallbackInputs.effectiveFeatures = StandardPbrFeature::NormalTexture;
    const auto fallback = PrepareStandardPbrMaterial(descriptor, Reflection(), fallbackInputs);
    REQUIRE(fallback.HasValue());
    CHECK(fallback.Value().selectedProfile == MaterialQualityProfile::Baseline);

    auto implicitInputs = fallbackInputs;
    implicitInputs.selectedProfile = MaterialQualityProfile::Standard;
    RequireError(PrepareStandardPbrMaterial(descriptor, Reflection(), implicitInputs), StandardPbrMaterialErrors::UnsupportedQuality);

    auto missingRequired = fallbackInputs;
    missingRequired.effectiveFeatures = StandardPbrFeature::None;
    RequireError(PrepareStandardPbrMaterial(descriptor, Reflection(), missingRequired), StandardPbrMaterialErrors::UnsupportedQuality);
}

TEST_CASE("Standard PBR admits an explicitly reduced effective feature set", "[runtime][renderer][material]") {
    auto descriptor = Descriptor();
    descriptor.features = StandardPbrFeature::AlbedoTexture | StandardPbrFeature::NormalTexture;
    descriptor.quality.requiredFeatures = StandardPbrFeature::AlbedoTexture;
    const std::array textures{StandardPbrTextureBinding{StandardPbrTextureRole::Albedo, {Owner(), 3, 2}, {Owner(), 8, 1}}};
    auto inputs = Inputs(textures);
    inputs.effectiveFeatures = StandardPbrFeature::AlbedoTexture;

    const auto prepared = PrepareStandardPbrMaterial(descriptor, Reflection(), inputs);
    REQUIRE(prepared.HasValue());
    CHECK(prepared.Value().features == StandardPbrFeature::AlbedoTexture);
    REQUIRE(prepared.Value().textures.size() == 1);
    CHECK(prepared.Value().textures.front().role == StandardPbrTextureRole::Albedo);
}

TEST_CASE("Standard PBR texture roles are exact, unique, valid, and canonically owned", "[runtime][renderer][material]") {
    auto descriptor = Descriptor();
    descriptor.features = StandardPbrFeature::AlbedoTexture | StandardPbrFeature::NormalTexture;
    std::array textures{
        StandardPbrTextureBinding{StandardPbrTextureRole::Normal, {Owner(), 4, 2}, {Owner(), 7, 1}},
        StandardPbrTextureBinding{StandardPbrTextureRole::Albedo, {Owner(), 3, 2}, {Owner(), 8, 1}},
    };
    auto inputs = Inputs(textures);
    inputs.effectiveFeatures = descriptor.features;

    const auto prepared = PrepareStandardPbrMaterial(descriptor, Reflection(), inputs);
    REQUIRE(prepared.HasValue());
    REQUIRE(prepared.Value().textures.size() == 2);
    CHECK(prepared.Value().textures[0].role == StandardPbrTextureRole::Albedo);
    CHECK(prepared.Value().textures[1].role == StandardPbrTextureRole::Normal);

    textures[0].role = StandardPbrTextureRole::Opacity;
    CHECK(prepared.Value().textures[1].role == StandardPbrTextureRole::Normal);

    textures[0] = textures[1];
    RequireError(PrepareStandardPbrMaterial(descriptor, Reflection(), inputs), StandardPbrMaterialErrors::TextureBindingMismatch);

    textures[0] = StandardPbrTextureBinding{StandardPbrTextureRole::Normal, {}, {Owner(), 7, 1}};
    RequireError(PrepareStandardPbrMaterial(descriptor, Reflection(), inputs), StandardPbrMaterialErrors::InvalidResidentResource);
}

TEST_CASE("Standard PBR rejects malformed semantic and resident lifecycle data", "[runtime][renderer][material]") {
    auto descriptor = Descriptor();
    descriptor.parameters.roughness = 1.1F;
    RequireError(PrepareStandardPbrMaterial(descriptor, Reflection(), Inputs()), StandardPbrMaterialErrors::InvalidDescriptor);

    descriptor = Descriptor();
    descriptor.alphaMode = MaterialAlphaMode::Masked;
    RequireError(PrepareStandardPbrMaterial(descriptor, Reflection(), Inputs()), StandardPbrMaterialErrors::InvalidDescriptor);

    descriptor.parameters.opacityMaskThreshold = 0.4F;
    REQUIRE(PrepareStandardPbrMaterial(descriptor, Reflection(), Inputs()).HasValue());

    auto inputs = Inputs();
    inputs.generation = 0;
    RequireError(PrepareStandardPbrMaterial(descriptor, Reflection(), inputs), StandardPbrMaterialErrors::InvalidResidentResource);
    inputs = Inputs();
    inputs.pipeline = {};
    RequireError(PrepareStandardPbrMaterial(descriptor, Reflection(), inputs), StandardPbrMaterialErrors::InvalidResidentResource);
}

TEST_CASE("Standard PBR reflection and allocation bounds fail without partial publication", "[runtime][renderer][material]") {
    auto reflection = Reflection();
    reflection.parameters.pop_back();
    RequireError(PrepareStandardPbrMaterial(Descriptor(), reflection, Inputs()), StandardPbrMaterialErrors::ReflectionMismatch);

    reflection = Reflection();
    reflection.parameters[0].columns = 4;
    RequireError(PrepareStandardPbrMaterial(Descriptor(), reflection, Inputs()), StandardPbrMaterialErrors::ReflectionMismatch);

    reflection = Reflection();
    reflection.parameters.back().byteOffset = 60;
    const StandardPbrMaterialLimits exact{.maximumTextureBindings = 1, .maximumParameterBytes = 64};
    REQUIRE(PrepareStandardPbrMaterial(Descriptor(), reflection, Inputs(), exact).HasValue());

    const StandardPbrMaterialLimits tooSmall{.maximumTextureBindings = 1, .maximumParameterBytes = 63};
    RequireError(PrepareStandardPbrMaterial(Descriptor(), reflection, Inputs(), tooSmall),
                 StandardPbrMaterialErrors::ParameterBufferTooLarge);

    auto invalidLimits = StandardPbrMaterialLimits{};
    invalidLimits.maximumTextureBindings = 0;
    RequireError(PrepareStandardPbrMaterial(Descriptor(), Reflection(), Inputs(), invalidLimits), StandardPbrMaterialErrors::InvalidLimits);
}
