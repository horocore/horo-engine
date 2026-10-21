#pragma once

/** @file RenderShaderResources.h
 * @brief Cooked shader module and immutable graphics pipeline preparation contracts.
 */

#include "Horo/Runtime/Render/PipelineCache.h"
#include "Horo/Runtime/Render/RenderResourceDescriptors.h"
#include "Horo/Runtime/Render/ShaderPermutation.h"
#include "Horo/Runtime/Render/ShaderReflection.h"

namespace Horo::Render {
    /** @brief Selected backend's native-free pipeline compatibility facts.
     * @details Queried during owner-thread preparation after initialization. These facts identify cache compatibility;
     * they do not enumerate adapters or authorize retargeting an explicitly selected device. */
    struct RenderPipelineDeviceIdentity {
        RenderBackendId backend;
        std::uint64_t backendModuleVersion{0};
        RenderAdapterId adapter;
        Sha256Digest deviceCompatibilityDigest;
        std::string driverIdentity;
        std::string driverVersion;
    };

    /** @brief Exact cooked stage identity; resident identity is issued separately by the frontend registry. */
    struct RenderShaderModuleDescriptor {
        ShaderTargetRequirement target;
        ShaderStage stage{ShaderStage::Vertex};
        std::string entryPoint;
        Sha256Digest artifactKey;
        ShaderInterfaceCompatibilityId shaderInterface;
    };

    /** @brief Immutable triangle raster face rejection. */
    enum class RenderCullMode : std::uint8_t {
        None,
        Front,
        Back
    };
    /** @brief Immutable front-face winding independent of native API constants. */
    enum class RenderFrontFace : std::uint8_t {
        CounterClockwise,
        Clockwise
    };
    /** @brief Explicit supported color blending recipes. */
    enum class RenderBlendMode : std::uint8_t {
        Disabled,
        Alpha,
        PremultipliedAlpha,
        Additive
    };

    /**
     * @brief Immutable graphics state for the established MeshVertex triangle layout.
     * @details Pipeline creation neither chooses a frame recipe nor changes active rendering state. Output formats and
     * sample count must match the eventual render target. Stencil, tessellation and non-triangle geometry are outside
     * this descriptor; they require separate explicit contracts rather than silently changing this state.
     */
    struct RenderGraphicsPipelineState {
        RenderCullMode cull{RenderCullMode::Back};
        RenderFrontFace frontFace{RenderFrontFace::CounterClockwise};
        RenderBlendMode blend{RenderBlendMode::Disabled};
        bool depthTest{true};
        bool depthWrite{true};
        RenderCompareFunction depthCompare{RenderCompareFunction::LessEqual};
        RenderTextureFormat colorFormat{RenderTextureFormat::Rgba8Unorm};
        std::optional<RenderTextureFormat> depthFormat{RenderTextureFormat::Depth24Stencil8};
        std::uint32_t sampleCount{1};
        [[nodiscard]] bool operator==(const RenderGraphicsPipelineState &) const noexcept = default;
    };

    /**
     * @brief Owned exact shader/layout/state input for one graphics pipeline generation.
     * @details Vertex and fragment handles refer to the same cooked artifact and logical interface. The frontend keeps
     * both dependency generations pinned through pipeline retirement. Compatibility names the selected backend/device,
     * original artifact and canonical state digest; no native cache blob is required to rebuild this value.
     */
    struct RenderGraphicsPipelineDescriptor {
        RenderShaderModuleHandle vertex;
        RenderShaderModuleHandle fragment;
        ShaderManifest manifest;
        ShaderTargetRequirement target;
        NormalizedShaderReflection reflection;
        RenderGraphicsPipelineState state;
        PipelineCacheCompatibility compatibility;
        /** @brief Canonical sorted runtime overrides from permutation resolution; omitted IDs use manifest defaults.
         * @details Immutable for this generation and included in pipelineDescriptorDigest. Values never mutate cooked artifacts. */
        std::vector<ShaderSpecializationValue> specializationValues{};
    };

    /** @brief Validates finite cooked module identity without invoking a native API.
     * @param descriptor Exact cooked target, stage and artifact/interface identity.
     * @return Success or a typed invalid identity failure. */
    [[nodiscard]] Result<void> ValidateRenderShaderModuleDescriptor(const RenderShaderModuleDescriptor &descriptor);

    /** @brief Computes canonical graphics state identity without native objects or resident handle values.
     * @param state Complete immutable fixed state and established MeshVertex layout.
     * @return State digest or a typed invalid state failure. */
    [[nodiscard]] Result<Sha256Digest> ComputeRenderGraphicsPipelineStateDigest(const RenderGraphicsPipelineState &state);

    /** @brief Computes immutable graphics state and runtime specialization identity without native objects.
     * @param state Complete immutable fixed graphics state.
     * @param values At most 64 unique ID-sorted typed scalar overrides from permutation resolution.
     * @return Canonical digest or typed invalid value/state failure. Empty values preserve the existing state-only digest.
     * @details Callers supplying overrides use this digest for PipelineCacheCompatibility::pipelineDescriptorDigest. */
    [[nodiscard]] Result<Sha256Digest> ComputeRenderGraphicsPipelineDescriptorDigest(const RenderGraphicsPipelineState &state,
                                                                                     std::span<const ShaderSpecializationValue> values);

    /** @brief Validates exact pipeline identity and final-artifact reflection before native admission.
     * @param descriptor Complete immutable shader, reflection and fixed-state contract.
     * @return Canonical exact pipeline cache key or a typed manifest/reflection/identity failure.
     * @details Performs bounded caller-thread preparation; owns no native resource, callback, job or publication state. */
    [[nodiscard]] Result<PipelineCacheKey> ValidateRenderGraphicsPipelineDescriptor(const RenderGraphicsPipelineDescriptor &descriptor);
}  // namespace Horo::Render
