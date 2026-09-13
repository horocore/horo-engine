#include "Horo/Runtime/Render/StandardPbrMaterial.h"

#include "Horo/Runtime/Render/StandardPbrMaterialErrors.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <new>
#include <ranges>
#include <utility>

namespace Horo::Render {
    namespace {
        static_assert(sizeof(float) == sizeof(std::uint32_t));

        struct ParameterValue {
            ShaderParameterId id;
            std::span<const float> values;
        };

        [[nodiscard]] constexpr bool IsKnown(const MaterialAlphaMode mode) noexcept {
            return static_cast<std::uint8_t>(mode) <= static_cast<std::uint8_t>(MaterialAlphaMode::Additive);
        }

        [[nodiscard]] constexpr bool IsKnown(const MaterialQualityProfile profile) noexcept {
            return static_cast<std::uint8_t>(profile) <= static_cast<std::uint8_t>(MaterialQualityProfile::Ultra);
        }

        [[nodiscard]] constexpr bool IsKnown(const StandardPbrTextureRole role) noexcept {
            return static_cast<std::uint8_t>(role) <= static_cast<std::uint8_t>(StandardPbrTextureRole::Opacity);
        }

        [[nodiscard]] constexpr StandardPbrFeature FeatureForRole(const StandardPbrTextureRole role) noexcept {
            switch (role) {
                case StandardPbrTextureRole::Albedo:
                    return StandardPbrFeature::AlbedoTexture;
                case StandardPbrTextureRole::MetallicRoughness:
                    return StandardPbrFeature::MetallicRoughnessTexture;
                case StandardPbrTextureRole::Normal:
                    return StandardPbrFeature::NormalTexture;
                case StandardPbrTextureRole::Occlusion:
                    return StandardPbrFeature::OcclusionTexture;
                case StandardPbrTextureRole::Emissive:
                    return StandardPbrFeature::EmissiveTexture;
                case StandardPbrTextureRole::Opacity:
                    return StandardPbrFeature::OpacityTexture;
            }
            return StandardPbrFeature::None;
        }

        [[nodiscard]] constexpr bool IsKnownFeatureMask(const StandardPbrFeature features) noexcept {
            constexpr auto all = StandardPbrFeature::AlbedoTexture | StandardPbrFeature::MetallicRoughnessTexture |
                                 StandardPbrFeature::NormalTexture | StandardPbrFeature::OcclusionTexture |
                                 StandardPbrFeature::EmissiveTexture | StandardPbrFeature::OpacityTexture;
            return (static_cast<std::uint32_t>(features) & ~static_cast<std::uint32_t>(all)) == 0;
        }

        [[nodiscard]] bool IsFinite(const StandardPbrParameters &parameters) noexcept {
            return Math::IsFinite(parameters.albedo) && std::isfinite(parameters.metallic) && std::isfinite(parameters.roughness) &&
                   std::isfinite(parameters.occlusion) && Math::IsFinite(parameters.emissive) &&
                   std::isfinite(parameters.emissiveIntensity) && std::isfinite(parameters.opacity) &&
                   (!parameters.opacityMaskThreshold.has_value() || std::isfinite(*parameters.opacityMaskThreshold));
        }

        [[nodiscard]] bool InUnitRange(const float value) noexcept {
            return value >= 0.0F && value <= 1.0F;
        }

        [[nodiscard]] Result<void> ValidateDescriptor(const StandardPbrMaterialDescriptor &descriptor) {
            if (!descriptor.id.IsValid() || descriptor.sourceRevision == 0 || !IsKnown(descriptor.alphaMode) ||
                !IsKnownFeatureMask(descriptor.features) || !IsKnownFeatureMask(descriptor.quality.requiredFeatures) ||
                !IsKnown(descriptor.quality.minimum) || !IsKnown(descriptor.quality.preferred) || !IsFinite(descriptor.parameters) ||
                !InUnitRange(descriptor.parameters.albedo.x) || !InUnitRange(descriptor.parameters.albedo.y) ||
                !InUnitRange(descriptor.parameters.albedo.z) || !InUnitRange(descriptor.parameters.metallic) ||
                !InUnitRange(descriptor.parameters.roughness) || !InUnitRange(descriptor.parameters.occlusion) ||
                descriptor.parameters.emissive.x < 0.0F || descriptor.parameters.emissive.y < 0.0F ||
                descriptor.parameters.emissive.z < 0.0F || descriptor.parameters.emissiveIntensity < 0.0F ||
                !InUnitRange(descriptor.parameters.opacity) ||
                (descriptor.alphaMode == MaterialAlphaMode::Masked &&
                 (!descriptor.parameters.opacityMaskThreshold.has_value() || !InUnitRange(*descriptor.parameters.opacityMaskThreshold))) ||
                (descriptor.alphaMode != MaterialAlphaMode::Masked && descriptor.parameters.opacityMaskThreshold.has_value()) ||
                static_cast<std::uint8_t>(descriptor.quality.preferred) < static_cast<std::uint8_t>(descriptor.quality.minimum) ||
                !HasStandardPbrFeatures(descriptor.features, descriptor.quality.requiredFeatures) ||
                descriptor.quality.authoredFallbacks.size() > StandardPbrMaterialLimits::MaximumAuthoredFallbacks)
                return Result<void>::Failure(MakeError(StandardPbrMaterialErrors::InvalidDescriptor));

            std::array<bool, 4> seenFallback{};
            for (const auto profile : descriptor.quality.authoredFallbacks) {
                const auto index = static_cast<std::size_t>(profile);
                if (!IsKnown(profile) || profile == descriptor.quality.preferred || seenFallback[index] ||
                    static_cast<std::uint8_t>(profile) < static_cast<std::uint8_t>(descriptor.quality.minimum))
                    return Result<void>::Failure(MakeError(StandardPbrMaterialErrors::InvalidDescriptor));
                seenFallback[index] = true;
            }
            return Result<void>::Success();
        }

        [[nodiscard]] bool IsProfileAdmitted(const StandardPbrQualityRequirements &quality,
                                             const MaterialQualityProfile selected) noexcept {
            return selected == quality.preferred ||
                   std::ranges::find(quality.authoredFallbacks, selected) != quality.authoredFallbacks.end();
        }

        [[nodiscard]] Result<void> ValidateTextures(const StandardPbrMaterialDescriptor &descriptor,
                                                    const StandardPbrResidentInputs &inputs, const StandardPbrMaterialLimits &limits) {
            if (inputs.textures.size() > limits.maximumTextureBindings)
                return Result<void>::Failure(MakeError(StandardPbrMaterialErrors::TextureBindingMismatch));

            std::array<bool, 6> seen{};
            StandardPbrFeature boundFeatures = StandardPbrFeature::None;
            for (const auto &binding : inputs.textures) {
                if (!IsKnown(binding.role))
                    return Result<void>::Failure(MakeError(StandardPbrMaterialErrors::TextureBindingMismatch));
                const auto index = static_cast<std::size_t>(binding.role);
                if (seen[index])
                    return Result<void>::Failure(MakeError(StandardPbrMaterialErrors::TextureBindingMismatch));
                if (!binding.texture.IsValid() || !binding.sampler.IsValid())
                    return Result<void>::Failure(MakeError(StandardPbrMaterialErrors::InvalidResidentResource));
                seen[index] = true;
                boundFeatures = boundFeatures | FeatureForRole(binding.role);
            }
            if (!IsKnownFeatureMask(inputs.effectiveFeatures) || boundFeatures != inputs.effectiveFeatures ||
                !HasStandardPbrFeatures(descriptor.features, inputs.effectiveFeatures))
                return Result<void>::Failure(MakeError(StandardPbrMaterialErrors::TextureBindingMismatch));
            return Result<void>::Success();
        }

        [[nodiscard]] const ShaderReflectedParameter *FindParameter(const NormalizedShaderReflection &reflection,
                                                                    const ShaderParameterId id) noexcept {
            const auto found = std::ranges::find(reflection.parameters, id, &ShaderReflectedParameter::id);
            return found == reflection.parameters.end() ? nullptr : &*found;
        }

        [[nodiscard]] Result<std::size_t> ValidatePacking(const NormalizedShaderReflection &reflection,
                                                          const std::span<const ParameterValue> values,
                                                          const StandardPbrMaterialLimits &limits) {
            const auto buffer = std::ranges::find(reflection.bindings, StandardPbrParameterIds::Buffer, &ShaderReflectedBinding::id);
            constexpr auto fragmentStage = static_cast<std::uint8_t>(ShaderStageVisibility::Fragment);
            if (reflection.interfaceSchemaVersion == 0 || buffer == reflection.bindings.end() || !buffer->active ||
                buffer->kind != ShaderResourceKind::UniformBuffer || buffer->access != ShaderResourceAccess::ReadOnly ||
                (static_cast<std::uint8_t>(buffer->stages) & fragmentStage) == 0)
                return Result<std::size_t>::Failure(MakeError(StandardPbrMaterialErrors::ReflectionMismatch));

            std::array<std::pair<std::size_t, std::size_t>, 8> occupied{};
            if (values.size() > occupied.size())
                return Result<std::size_t>::Failure(MakeError(StandardPbrMaterialErrors::ReflectionMismatch));
            std::size_t occupiedCount = 0;
            std::size_t byteCount = 0;
            for (const auto &value : values) {
                const auto *parameter = FindParameter(reflection, value.id);
                if (parameter == nullptr || !parameter->active || parameter->binding != StandardPbrParameterIds::Buffer ||
                    parameter->type != ShaderValueType::Float32 || parameter->rows != 1 || parameter->columns != value.values.size() ||
                    parameter->arrayCount != 1 || parameter->arrayStride != 0 || parameter->matrixStride != 0 ||
                    parameter->byteOffset % sizeof(float) != 0)
                    return Result<std::size_t>::Failure(MakeError(StandardPbrMaterialErrors::ReflectionMismatch));
                constexpr std::size_t componentBytes = sizeof(float);
                if (value.values.size() > (std::numeric_limits<std::size_t>::max() - parameter->byteOffset) / componentBytes)
                    return Result<std::size_t>::Failure(MakeError(StandardPbrMaterialErrors::ParameterBufferTooLarge));
                const std::size_t end = static_cast<std::size_t>(parameter->byteOffset) + value.values.size() * componentBytes;
                for (std::size_t existing = 0; existing < occupiedCount; ++existing) {
                    if (parameter->byteOffset < occupied[existing].second && end > occupied[existing].first)
                        return Result<std::size_t>::Failure(MakeError(StandardPbrMaterialErrors::ReflectionMismatch));
                }
                occupied[occupiedCount++] = {parameter->byteOffset, end};
                byteCount = std::max(byteCount, end);
            }
            if (byteCount == 0 || byteCount > limits.maximumParameterBytes)
                return Result<std::size_t>::Failure(MakeError(StandardPbrMaterialErrors::ParameterBufferTooLarge));
            return Result<std::size_t>::Success(byteCount);
        }

        void StoreFloat(std::vector<std::byte> &bytes, const std::size_t offset, const float value) noexcept {
            const auto bits = std::bit_cast<std::uint32_t>(value);
            for (std::size_t byte = 0; byte < sizeof(bits); ++byte)
                bytes[offset + byte] = static_cast<std::byte>((bits >> (byte * 8U)) & 0xffU);
        }

        [[nodiscard]] Result<ResidentStandardPbrMaterial> StageParameterValues(const StandardPbrMaterialDescriptor &descriptor,
                                                                               const NormalizedShaderReflection &reflection,
                                                                               const StandardPbrResidentInputs &inputs,
                                                                               const std::span<const ParameterValue> values,
                                                                               const std::size_t byteCount) {
            try {
                std::vector<std::byte> parameterBytes(byteCount);
                for (const auto &value : values) {
                    const auto *parameter = FindParameter(reflection, value.id);
                    for (std::size_t component = 0; component < value.values.size(); ++component)
                        StoreFloat(parameterBytes, parameter->byteOffset + component * sizeof(float), value.values[component]);
                }
                std::vector<StandardPbrTextureBinding> textures(inputs.textures.begin(), inputs.textures.end());
                std::ranges::sort(textures, {}, &StandardPbrTextureBinding::role);
                return Result<ResidentStandardPbrMaterial>::Success({
                    .id = descriptor.id,
                    .sourceRevision = descriptor.sourceRevision,
                    .generation = inputs.generation,
                    .alphaMode = descriptor.alphaMode,
                    .selectedProfile = inputs.selectedProfile,
                    .features = inputs.effectiveFeatures,
                    .shaderInterface = reflection.interfaceCompatibility,
                    .pipeline = inputs.pipeline,
                    .textures = std::move(textures),
                    .parameterBytes = std::move(parameterBytes),
                });
            } catch (const std::bad_alloc &) {
                return Result<ResidentStandardPbrMaterial>::Failure(MakeError(StandardPbrMaterialErrors::AllocationFailed));
            }
        }
    }  // namespace

    /** @copydoc PrepareStandardPbrMaterial */
    Result<ResidentStandardPbrMaterial> PrepareStandardPbrMaterial(const StandardPbrMaterialDescriptor &descriptor,
                                                                   const NormalizedShaderReflection &reflection,
                                                                   const StandardPbrResidentInputs &inputs,
                                                                   const StandardPbrMaterialLimits &limits) {
        if (!limits.IsValid())
            return Result<ResidentStandardPbrMaterial>::Failure(MakeError(StandardPbrMaterialErrors::InvalidLimits));
        if (auto valid = ValidateDescriptor(descriptor); valid.HasError())
            return Result<ResidentStandardPbrMaterial>::Failure(valid.ErrorValue());
        if (!inputs.pipeline.IsValid() || inputs.generation == 0)
            return Result<ResidentStandardPbrMaterial>::Failure(MakeError(StandardPbrMaterialErrors::InvalidResidentResource));
        if (!IsKnown(inputs.selectedProfile) || !IsProfileAdmitted(descriptor.quality, inputs.selectedProfile) ||
            !HasStandardPbrFeatures(inputs.effectiveFeatures, descriptor.quality.requiredFeatures))
            return Result<ResidentStandardPbrMaterial>::Failure(MakeError(StandardPbrMaterialErrors::UnsupportedQuality));
        if (auto textures = ValidateTextures(descriptor, inputs, limits); textures.HasError())
            return Result<ResidentStandardPbrMaterial>::Failure(textures.ErrorValue());

        const std::array albedo{descriptor.parameters.albedo.x, descriptor.parameters.albedo.y, descriptor.parameters.albedo.z};
        const std::array metallic{descriptor.parameters.metallic};
        const std::array roughness{descriptor.parameters.roughness};
        const std::array occlusion{descriptor.parameters.occlusion};
        const std::array emissive{descriptor.parameters.emissive.x, descriptor.parameters.emissive.y, descriptor.parameters.emissive.z};
        const std::array emissiveIntensity{descriptor.parameters.emissiveIntensity};
        const std::array opacity{descriptor.parameters.opacity};
        const std::array opacityMaskThreshold{descriptor.parameters.opacityMaskThreshold.value_or(0.0F)};
        const std::array values{
            ParameterValue{StandardPbrParameterIds::Albedo, albedo},
            ParameterValue{StandardPbrParameterIds::Metallic, metallic},
            ParameterValue{StandardPbrParameterIds::Roughness, roughness},
            ParameterValue{StandardPbrParameterIds::Occlusion, occlusion},
            ParameterValue{StandardPbrParameterIds::Emissive, emissive},
            ParameterValue{StandardPbrParameterIds::EmissiveIntensity, emissiveIntensity},
            ParameterValue{StandardPbrParameterIds::Opacity, opacity},
            ParameterValue{StandardPbrParameterIds::OpacityMaskThreshold, opacityMaskThreshold},
        };
        auto byteCount = ValidatePacking(reflection, values, limits);
        if (byteCount.HasError())
            return Result<ResidentStandardPbrMaterial>::Failure(byteCount.ErrorValue());

        return StageParameterValues(descriptor, reflection, inputs, values, byteCount.Value());
    }
}  // namespace Horo::Render
