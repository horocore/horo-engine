#pragma once
/** @file ShaderReflectionCook.h
 * @brief Non-installed portable identity and value codec for the distinct final-artifact reflection stage. */
#include "Horo/Runtime/Render/ShaderCompilerPipeline.h"
#include "Horo/Runtime/Render/ShaderReflection.h"

#include <span>
#include <string_view>

namespace Horo::Render {
    /** @brief Explicit logical-to-authored member mapping; native offsets and bindings always come from final reflection. */
    struct ShaderReflectionParameterName {
        ShaderParameterId id;
        std::string name;
    };

    struct ShaderReflectionBindingName {
        ShaderBindingId id;
        std::string name;
        std::vector<ShaderReflectionParameterName> parameters;
    };

    /** @brief Exact stage output from the separately admitted SPIRV-Cross process. */
    struct SpirvReflectionStage {
        ShaderStage stage;
        std::string entryPoint;
        std::string json;
    };

    /** @brief Complete value identity for reflection of one immutable compilation artifact.
     * @details Payload remains in the existing HOROSHDR envelope. Layout version one supports Vulkan scalar/vector/matrix
     * uniform blocks and stage interfaces; source-map metadata is outside this bounded stage. */
    struct ReflectedShaderArtifact {
        std::uint32_t layoutVersion{1};
        Sha256Digest compiledArtifactKey;
        Sha256Digest payloadDigest;
        ShaderCompilerToolIdentity reflectionTool;
        NormalizedShaderReflection reflection;
        Sha256Digest cookedArtifactKey;
    };

    /** @brief Normalizes bounded official SPIRV-Cross JSON against exact authored IDs and the existing manifest contract. */
    [[nodiscard]] Result<NormalizedShaderReflection> NormalizeVulkanReflection(const ShaderManifest &manifest,
                                                                               const ShaderTargetRequirement &target,
                                                                               std::span<const SpirvReflectionStage> stages,
                                                                               std::span<const ShaderReflectionBindingName> names);
    /** @brief Binds exact compiled bytes/key, reflection tool identity and canonical normalized layout into one cooked identity. */
    [[nodiscard]] Result<ReflectedShaderArtifact> CookReflectedShaderArtifact(const CompiledShaderArtifact &compiled,
                                                                              const ShaderCompilerToolIdentity &reflectionTool,
                                                                              NormalizedShaderReflection reflection,
                                                                              std::uint32_t layoutVersion = 1);
    /** @brief Encodes at most one MiB of canonical value metadata; performs no compilation or native work. */
    [[nodiscard]] Result<std::vector<std::uint8_t>> EncodeReflectedShaderArtifact(const ReflectedShaderArtifact &artifact);
    /** @brief Rejects altered payload, layout, identity or malformed/oversized metadata before runtime pipeline admission. */
    [[nodiscard]] Result<ReflectedShaderArtifact> DecodeReflectedShaderArtifact(const ShaderManifest &manifest,
                                                                                const ShaderTargetRequirement &target,
                                                                                std::span<const std::uint8_t> payload,
                                                                                std::span<const std::uint8_t> metadata);
}  // namespace Horo::Render
