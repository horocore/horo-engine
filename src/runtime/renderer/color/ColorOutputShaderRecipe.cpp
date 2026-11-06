#include "ColorOutputShaderRecipe.h"

#include "Horo/Runtime/Render/ShaderManifestErrors.h"

namespace Horo::Render {
    namespace {
        /** @brief Scene, two analytic auxiliary tables and previous GPU state all remain independent resources. */
        void DescribeScene(ShaderManifest &manifest) {
            manifest.sourceIdentity = "engine/runtime/color_output.hlsl";
            manifest
                .bindings = {{{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment},
                             {{2}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment},
                             {{3}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment},
                             {{4}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment},
                             {{5}, ShaderResourceKind::Sampler, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment},
                             {{6}, ShaderResourceKind::Sampler, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment},
                             {{7}, ShaderResourceKind::StorageBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment}};
            manifest.parameters = {{{1}, {1}, ShaderValueType::Float32, 1, 4, 1},
                                   {{2}, {1}, ShaderValueType::Float32, 1, 4, 1},
                                   {{3}, {1}, ShaderValueType::Float32, 1, 4, 1},
                                   {{100}, {7}, ShaderValueType::Uint32, 1, 1, 1}};
        }

        /** @brief Extends the exact scene route with a real independently bound creative cube atlas. */
        void DescribeLook(ShaderManifest &manifest) {
            DescribeScene(manifest);
            manifest.sourceIdentity = "engine/runtime/color_output_look.hlsl";
            manifest.sourceRevision = 2;
            manifest.bindings.push_back(
                {{8}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment});
            for (std::uint32_t id = 4; id <= 6; ++id)
                manifest.parameters.insert(manifest.parameters.end() - 1, {{id}, {1}, ShaderValueType::Float32, 1, 4, 1});
        }

        /** @brief Final encoder reads the complete display-linear composition without sampling interpolation. */
        void DescribeEncode(ShaderManifest &manifest) {
            manifest.sourceIdentity = "engine/runtime/color_encode_sdr.hlsl";
            manifest
                .bindings = {{{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment},
                             {{2}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment}};
            manifest.parameters = {{{1}, {1}, ShaderValueType::Float32, 1, 4, 1}};
        }
    }  // namespace

    /** @copydoc MakeColorOutputShaderManifest */
    Result<ShaderManifest> MakeColorOutputShaderManifest(ColorOutputShaderStage stage, const ShaderTargetRequirement &target) {
        using Return = Result<ShaderManifest>;
        if (target.backend != ShaderTargetBackend::Metal || target.payloadFormat != ShaderPayloadFormat::MetalLibrary24)
            return Return::Failure(MakeError(ShaderManifestErrors::UnsupportedTarget));
        ShaderManifest manifest{.schemaVersion = 1,
                                .sourceRevision = 1,
                                .entryPoints = {{ShaderStage::Vertex, "VSMain"}, {ShaderStage::Fragment, "PSMain"}},
                                .targets = {target}};
        switch (stage) {
            case ColorOutputShaderStage::SceneToDisplayLinear:
                DescribeScene(manifest);
                break;
            case ColorOutputShaderStage::SceneToDisplayLinearWithLook:
                DescribeLook(manifest);
                break;
            case ColorOutputShaderStage::EncodeSdr:
                DescribeEncode(manifest);
                break;
            default:
                return Return::Failure(MakeError(ShaderManifestErrors::UnsupportedTarget));
        }
        if (auto valid = ValidateShaderManifest(manifest); valid.HasError())
            return Return::Failure(valid.ErrorValue());
        return Return::Success(std::move(manifest));
    }

    /** @copydoc ColorOutputShaderReflectionNames */
    std::vector<ShaderReflectionBindingName> ColorOutputShaderReflectionNames(ColorOutputShaderStage stage) {
        switch (stage) {
            case ColorOutputShaderStage::SceneToDisplayLinear:
                return {{{1}, "type.ColorOutputParameters", {{{1}, "SourceSize"}, {{2}, "ExposureSelection"}, {{3}, "ExposureIdentity"}}},
                        {{2}, "SceneColor", {}},
                        {{3}, "horo_color_reach_m_table_0", {}},
                        {{4}, "horo_color_gamut_cusp_table_0", {}},
                        {{5}, "horo_color_reach_m_table_0Sampler", {}},
                        {{6}, "horo_color_gamut_cusp_table_0Sampler", {}},
                        {{7}, "ExposureWords", {{{100}, "_m0"}}}};
            case ColorOutputShaderStage::SceneToDisplayLinearWithLook: {
                auto names = ColorOutputShaderReflectionNames(ColorOutputShaderStage::SceneToDisplayLinear);
                names[0].parameters.insert(names[0].parameters.end(),
                                           {{{4}, "LookDomainMinimum"}, {{5}, "LookDomainMaximum"}, {{6}, "LookControl"}});
                names.push_back({{8}, "LookCube", {}});
                return names;
            }
            case ColorOutputShaderStage::EncodeSdr:
                return {{{1}, "type.ColorEncodeParameters", {{{1}, "OutputControl"}}}, {{2}, "DisplayLinearColor", {}}};
        }
        return {};
    }
}  // namespace Horo::Render
