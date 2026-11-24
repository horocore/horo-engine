#include "ColorOutputShaderRecipe.h"

#include "Horo/Runtime/Render/ShaderManifestErrors.h"

namespace Horo::Render {
    namespace {
        /** @brief Scene, two analytic auxiliary tables and previous GPU state all remain independent resources. */
        void DescribeScene(ShaderManifest &manifest, bool manualExposure = false) {
            using enum Horo::Render::ShaderStageVisibility;
            using enum Horo::Render::ShaderValueType;
            using enum Horo::Render::ShaderResourceAccess;
            using enum Horo::Render::ShaderResourceKind;
            manifest.sourceIdentity = "engine/runtime/color_output.hlsl";
            manifest.bindings = {{{1}, UniformBuffer, ReadOnly, 1, Fragment},  {{2}, SampledTexture, ReadOnly, 1, Fragment},
                                 {{3}, SampledTexture, ReadOnly, 1, Fragment}, {{4}, SampledTexture, ReadOnly, 1, Fragment},
                                 {{5}, Sampler, ReadOnly, 1, Fragment},        {{6}, Sampler, ReadOnly, 1, Fragment},
                                 {{7}, StorageBuffer, ReadOnly, 1, Fragment}};
            manifest.parameters = {{{1}, {1}, Float32, 1, 4, 1},
                                   {{2}, {1}, Float32, 1, 4, 1},
                                   {{3}, {1}, Float32, 1, 4, 1},
                                   {{100}, {7}, Uint32, 1, 1, 1}};
            if (manualExposure) {
                manifest.bindings.pop_back();
                manifest.parameters.resize(2);
                manifest.sourceRevision = 2;
            }
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
        void DescribeEncode(ShaderManifest &manifest, bool accessibility) {
            manifest.sourceIdentity = "engine/runtime/color_encode_sdr.hlsl";
            manifest
                .bindings = {{{1}, ShaderResourceKind::UniformBuffer, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment},
                             {{2}, ShaderResourceKind::SampledTexture, ShaderResourceAccess::ReadOnly, 1, ShaderStageVisibility::Fragment}};
            manifest.parameters = {{{1}, {1}, ShaderValueType::Float32, 1, 4, 1}};
            if (accessibility) {
                manifest.sourceRevision = 2;
                for (std::uint32_t id = 2; id <= 4; ++id)
                    manifest.parameters.push_back({{id}, {1}, ShaderValueType::Float32, 1, 4, 1});
            }
        }

        /** @brief Admits Metal color output and the Vulkan route with manual exposure and no creative look. */
        bool SupportsColorOutput(const ShaderTargetRequirement &target, ColorOutputShaderStage stage) noexcept {
            if (target.backend == ShaderTargetBackend::Metal)
                return target.payloadFormat == ShaderPayloadFormat::MetalLibrary24;
            return target.backend == ShaderTargetBackend::Vulkan && target.payloadFormat == ShaderPayloadFormat::SpirV16 &&
                   stage != ColorOutputShaderStage::SceneToDisplayLinearWithLook;
        }
    }  // namespace

    /** @copydoc MakeColorOutputShaderManifest */
    Result<ShaderManifest> MakeColorOutputShaderManifest(ColorOutputShaderStage stage, const ShaderTargetRequirement &target) {
        using Return = Result<ShaderManifest>;
        if (!SupportsColorOutput(target, stage))
            return Return::Failure(MakeError(ShaderManifestErrors::UnsupportedTarget));
        ShaderManifest manifest{.schemaVersion = 1,
                                .sourceRevision = 1,
                                .entryPoints = {{ShaderStage::Vertex, "VSMain"}, {ShaderStage::Fragment, "PSMain"}},
                                .targets = {target}};
        switch (stage) {
            using enum Horo::Render::ColorOutputShaderStage;
            case SceneToDisplayLinear:
                DescribeScene(manifest, target.backend == ShaderTargetBackend::Vulkan);
                break;
            case SceneToDisplayLinearWithLook:
                DescribeLook(manifest);
                break;
            case EncodeSdr:
            case EncodeSdrWithAccessibility:
                DescribeEncode(manifest, stage == EncodeSdrWithAccessibility);
                break;
            default:
                return Return::Failure(MakeError(ShaderManifestErrors::UnsupportedTarget));
        }
        if (auto valid = ValidateShaderManifest(manifest); valid.HasError())
            return Return::Failure(valid.ErrorValue());
        return Return::Success(std::move(manifest));
    }

    /** @copydoc ColorOutputShaderReflectionNames */
    std::vector<ShaderReflectionBindingName> ColorOutputShaderReflectionNames(ColorOutputShaderStage stage, ShaderTargetBackend backend) {
        if (backend != ShaderTargetBackend::Metal && backend != ShaderTargetBackend::Vulkan)
            return {};
        if (backend == ShaderTargetBackend::Vulkan && stage == ColorOutputShaderStage::SceneToDisplayLinearWithLook)
            return {};
        switch (stage) {
            using enum Horo::Render::ColorOutputShaderStage;
            case SceneToDisplayLinear: {
                std::vector<ShaderReflectionBindingName> names{{{1},
                                                                "type.ColorOutputParameters",
                                                                {{{1}, "SourceSize"}, {{2}, "ExposureSelection"}}},
                                                               {{2}, "SceneColor", {}},
                                                               {{3}, "horo_color_reach_m_table_0", {}},
                                                               {{4}, "horo_color_gamut_cusp_table_0", {}},
                                                               {{5}, "horo_color_reach_m_table_0Sampler", {}},
                                                               {{6}, "horo_color_gamut_cusp_table_0Sampler", {}}};
                if (backend == ShaderTargetBackend::Metal) {
                    names[0].parameters.push_back({{3}, "ExposureIdentity"});
                    names.push_back({{7}, "ExposureWords", {{{100}, "_m0"}}});
                }
                return names;
            }
            case SceneToDisplayLinearWithLook: {
                auto names = ColorOutputShaderReflectionNames(SceneToDisplayLinear);
                names[0].parameters.insert(names[0].parameters.end(),
                                           {{{4}, "LookDomainMinimum"}, {{5}, "LookDomainMaximum"}, {{6}, "LookControl"}});
                names.push_back({{8}, "LookCube", {}});
                return names;
            }
            case EncodeSdr:
                return {{{1}, "type.ColorEncodeParameters", {{{1}, "OutputControl"}}}, {{2}, "DisplayLinearColor", {}}};
            case EncodeSdrWithAccessibility:
                return {{{1},
                         "type.ColorEncodeParameters",
                         {{{1}, "OutputControl"}, {{2}, "AccessibilityRed"}, {{3}, "AccessibilityGreen"}, {{4}, "AccessibilityBlue"}}},
                        {{2}, "DisplayLinearColor", {}}};
        }
        return {};
    }
}  // namespace Horo::Render
