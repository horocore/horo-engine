#pragma once

/**
 * @file StandardPbrMaterial.h
 * @brief Backend-neutral standard PBR material validation, packing, and resident data.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Math/SceneMath.h"
#include "Horo/Runtime/Render/RenderResource.h"
#include "Horo/Runtime/Render/ShaderReflection.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Horo::Render {
    /** @brief Process-local semantic identity of one material-table entry. */
    struct MaterialRuntimeId {
        std::uint64_t value{0};

        /** @brief Reports whether the identity can name a material entry. @return True for a non-zero identity. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return value != 0;
        }

        [[nodiscard]] constexpr auto operator<=>(const MaterialRuntimeId &) const noexcept = default;
    };

    /** @brief Authored alpha and depth-write classification for the standard material. */
    enum class MaterialAlphaMode : std::uint8_t {
        Opaque,
        Masked,
        Translucent,
        Additive,
    };

    /** @brief Canonical semantic texture slot independent of backend binding positions. */
    enum class StandardPbrTextureRole : std::uint8_t {
        Albedo,
        MetallicRoughness,
        Normal,
        Occlusion,
        Emissive,
        Opacity,
    };

    /** @brief Canonical renderer product profile used by material admission policy. */
    enum class MaterialQualityProfile : std::uint8_t {
        Baseline,
        Standard,
        High,
        Ultra,
    };

    /** @brief Feature bits that affect the standard PBR shader permutation. */
    enum class StandardPbrFeature : std::uint32_t {
        None = 0,
        AlbedoTexture = 1U << 0U,
        MetallicRoughnessTexture = 1U << 1U,
        NormalTexture = 1U << 2U,
        OcclusionTexture = 1U << 3U,
        EmissiveTexture = 1U << 4U,
        OpacityTexture = 1U << 5U,
    };

    [[nodiscard]] constexpr StandardPbrFeature operator|(const StandardPbrFeature left, const StandardPbrFeature right) noexcept {
        return static_cast<StandardPbrFeature>(static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right));
    }

    [[nodiscard]] constexpr StandardPbrFeature operator&(const StandardPbrFeature left, const StandardPbrFeature right) noexcept {
        return static_cast<StandardPbrFeature>(static_cast<std::uint32_t>(left) & static_cast<std::uint32_t>(right));
    }

    /** @brief Reports whether every bit in `required` exists in `available`. */
    [[nodiscard]] constexpr bool HasStandardPbrFeatures(const StandardPbrFeature available, const StandardPbrFeature required) noexcept {
        return (available & required) == required;
    }

    /** @brief Stable logical IDs required from the final target reflection before material packing. */
    namespace StandardPbrParameterIds {
        inline constexpr ShaderBindingId Buffer{1};
        inline constexpr ShaderParameterId Albedo{1};
        inline constexpr ShaderParameterId Metallic{2};
        inline constexpr ShaderParameterId Roughness{3};
        inline constexpr ShaderParameterId Occlusion{4};
        inline constexpr ShaderParameterId Emissive{5};
        inline constexpr ShaderParameterId EmissiveIntensity{6};
        inline constexpr ShaderParameterId Opacity{7};
        inline constexpr ShaderParameterId OpacityMaskThreshold{8};
    }  // namespace StandardPbrParameterIds

    /** @brief Scalar and color values of the metallic-roughness standard material. */
    struct StandardPbrParameters {
        Math::Vec3 albedo{0.5F, 0.5F, 0.5F};
        float metallic{0.0F};
        float roughness{0.5F};
        float occlusion{1.0F};
        Math::Vec3 emissive{};
        float emissiveIntensity{1.0F};
        float opacity{1.0F};
        std::optional<float> opacityMaskThreshold;
    };

    /** @brief Explicit product-quality and required-feature admission request. */
    struct StandardPbrQualityRequirements {
        MaterialQualityProfile minimum{MaterialQualityProfile::Baseline};
        MaterialQualityProfile preferred{MaterialQualityProfile::Standard};
        StandardPbrFeature requiredFeatures{StandardPbrFeature::None};
        std::vector<MaterialQualityProfile> authoredFallbacks;
    };

    /** @brief One exact resident texture generation and sampler for a semantic PBR role. */
    struct StandardPbrTextureBinding {
        StandardPbrTextureRole role{StandardPbrTextureRole::Albedo};
        RenderTextureViewHandle texture;
        RenderSamplerHandle sampler;
    };

    /** @brief Complete semantic standard PBR entry produced by scene/material conversion. */
    struct StandardPbrMaterialDescriptor {
        MaterialRuntimeId id;
        std::uint64_t sourceRevision{0};
        StandardPbrParameters parameters;
        MaterialAlphaMode alphaMode{MaterialAlphaMode::Opaque};
        StandardPbrFeature features{StandardPbrFeature::None};
        StandardPbrQualityRequirements quality;
    };

    /** @brief Finite validation and packing envelope supplied by the runtime composition root. */
    struct StandardPbrMaterialLimits {
        static constexpr std::size_t HardMaxTextureBindings = 32;
        static constexpr std::size_t HardMaxParameterBytes = 64U * 1024U;
        static constexpr std::size_t MaximumAuthoredFallbacks = 3;

        std::size_t maximumTextureBindings{8};
        std::size_t maximumParameterBytes{4U * 1024U};

        /** @brief Validates finite non-zero material limits. @return True when both limits are within hard bounds. */
        [[nodiscard]] constexpr bool IsValid() const noexcept {
            return maximumTextureBindings > 0 && maximumTextureBindings <= HardMaxTextureBindings && maximumParameterBytes > 0 &&
                   maximumParameterBytes <= HardMaxParameterBytes;
        }
    };

    /** @brief Exact admitted resources and policy result used to realize a material generation. */
    struct StandardPbrResidentInputs {
        MaterialQualityProfile selectedProfile{MaterialQualityProfile::Baseline};
        StandardPbrFeature effectiveFeatures{StandardPbrFeature::None};
        RenderPipelineHandle pipeline;
        std::span<const StandardPbrTextureBinding> textures;
        std::uint64_t generation{0};
    };

    /**
     * @brief Immutable backend-neutral resident standard PBR material data.
     *
     * The value owns its packed bytes and texture-binding records. Renderer handles remain generation-safe
     * references owned by the RenderFrontend resource registry; no native object or backend type crosses this
     * boundary.
     */
    struct ResidentStandardPbrMaterial {
        MaterialRuntimeId id;
        std::uint64_t sourceRevision{0};
        std::uint64_t generation{0};
        MaterialAlphaMode alphaMode{MaterialAlphaMode::Opaque};
        MaterialQualityProfile selectedProfile{MaterialQualityProfile::Baseline};
        StandardPbrFeature features{StandardPbrFeature::None};
        ShaderInterfaceCompatibilityId shaderInterface;
        RenderPipelineHandle pipeline;
        std::vector<StandardPbrTextureBinding> textures;
        std::vector<std::byte> parameterBytes;
    };

    /**
     * @brief Validates and packs a standard PBR material against final-artifact target reflection.
     * @details Performs bounded synchronous work on the caller thread and retains no callbacks, jobs, or ambient state.
     * Cancellation and shutdown are therefore not applicable: failure publishes no resident value, and an existing value
     * is retired by releasing it at the RenderFrontend owner-thread safe point together with its generation-safe handles.
     * A lower quality profile is accepted only when it is explicitly present in `descriptor.quality.authoredFallbacks`.
     * @param descriptor Complete semantic standard material values and quality requirements.
     * @param reflection Validated final-target reflection whose offsets define parameter packing.
     * @param inputs Exact selected policy, feature set, and resident renderer resource generations.
     * @param limits Finite bounds applied before copying binding records or allocating the parameter block.
     * @return Wholly owned resident data, or a stable StandardPbrMaterialErrors failure without fallback.
     */
    [[nodiscard]] Result<ResidentStandardPbrMaterial> PrepareStandardPbrMaterial(const StandardPbrMaterialDescriptor &descriptor,
                                                                                 const NormalizedShaderReflection &reflection,
                                                                                 const StandardPbrResidentInputs &inputs,
                                                                                 const StandardPbrMaterialLimits &limits = {});
}  // namespace Horo::Render
