#pragma once

/**
 * @file ShaderReflection.h
 * @brief Backend-neutral final-artifact reflection, target-layout, and source-map validation.
 */

#include "Horo/Foundation/Result.h"
#include "Horo/Runtime/Render/ShaderManifest.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Horo::Render {
    /** @brief Direction of one reflected stage-interface value. */
    enum class ShaderInterfaceDirection : std::uint8_t {
        Input,
        Output
    };

    /** @brief Final-artifact evidence for one logical resource declared by the manifest. */
    struct ShaderReflectedBinding final {
        ShaderBindingId id;
        ShaderResourceKind kind{ShaderResourceKind::UniformBuffer};
        ShaderResourceAccess access{ShaderResourceAccess::ReadOnly};
        std::uint32_t arrayCount{1};
        ShaderStageVisibility stages{ShaderStageVisibility::None};
        bool active{true}; /**< False only when final optimization removed the declared resource. */
    };

    /** @brief Final target packing evidence for one logical shader parameter. */
    struct ShaderReflectedParameter final {
        ShaderParameterId id;
        ShaderBindingId binding;
        ShaderValueType type{ShaderValueType::Float32};
        std::uint8_t rows{1};
        std::uint8_t columns{1};
        std::uint32_t arrayCount{1};
        std::uint32_t byteOffset{0};
        std::uint32_t arrayStride{0};
        std::uint32_t matrixStride{0};
        bool columnMajor{true};
        bool active{true}; /**< Must match the activity of the containing resource. */
    };

    /** @brief Backend-neutral stage-link evidence from the final target artifact. */
    struct ShaderReflectedInterfaceVariable final {
        ShaderStage stage{ShaderStage::Vertex};
        ShaderInterfaceDirection direction{ShaderInterfaceDirection::Input};
        std::uint32_t location{0};
        ShaderValueType type{ShaderValueType::Float32};
        std::uint8_t rows{1};
        std::uint8_t columns{1};
        std::uint32_t arrayCount{1};
    };

    /** @brief Native target placement represented without exposing an API handle or native enum. */
    struct ShaderTargetBindingMapEntry final {
        ShaderBindingId id;
        std::uint32_t generatedHelperIndex{0}; /**< Zero is the logical resource; positive values are generated helpers. */
        std::uint32_t nativeSpace{0};
        std::uint32_t nativeBinding{0};
        std::string nativeName;        /**< Required for OpenGL runtime lookup; optional for other targets. */
        ShaderBindingId pairedSampler; /**< Optional sampler paired with a sampled texture. */
        bool active{true};             /**< Must match the reflected logical resource. */
    };

    /** @brief One generated-line range mapped back to an admitted source or graph location. */
    struct ShaderSourceMapEntry final {
        std::string generatedSourceIdentity;
        std::uint32_t generatedLineBegin{0};
        std::uint32_t generatedLineEnd{0};
        std::string sourceIdentity;
        std::uint32_t sourceLineBegin{0};
        std::string graphNodeIdentity;
        std::string graphPinIdentity;
    };

    /** @brief A compiler location before source-map normalization. */
    struct ShaderGeneratedSourceLocation final {
        std::string sourceIdentity;
        std::uint32_t line{0};
        std::uint32_t column{0};
    };

    /** @brief Preserved authored or explicitly generated diagnostic location. */
    struct ShaderMappedSourceLocation final {
        std::string sourceIdentity;
        std::uint32_t line{0};
        std::uint32_t column{0};
        std::string graphNodeIdentity;
        std::string graphPinIdentity;
        bool mapped{false}; /**< False preserves the generated location instead of inventing source evidence. */
    };

    /** @brief Raw backend-adapter evidence to validate and normalize for one final artifact. */
    struct ShaderReflectionCandidate final {
        ShaderTargetBackend backend{ShaderTargetBackend::Null};
        std::uint32_t interfaceSchemaVersion{0};
        std::vector<ShaderReflectedBinding> bindings;
        std::vector<ShaderReflectedParameter> parameters;
        std::vector<ShaderReflectedInterfaceVariable> stageInterface;
        std::vector<ShaderTargetBindingMapEntry> targetBindings;
        std::vector<ShaderSourceMapEntry> sourceMap;
    };

    /** @brief Validated canonical reflection and target map published with one artifact generation. */
    struct NormalizedShaderReflection final {
        ShaderTargetBackend backend{ShaderTargetBackend::Null};
        std::uint32_t interfaceSchemaVersion{0};
        ShaderInterfaceCompatibilityId interfaceCompatibility;
        std::vector<ShaderReflectedBinding> bindings;
        std::vector<ShaderReflectedParameter> parameters;
        std::vector<ShaderReflectedInterfaceVariable> stageInterface;
        std::vector<ShaderTargetBindingMapEntry> targetBindings;
        std::vector<ShaderSourceMapEntry> sourceMap;
    };

    /** @brief Finite validation limits for untrusted compiler/translator reflection output. */
    struct ShaderReflectionLimits final {
        std::size_t maximumBindings{128};
        std::size_t maximumParameters{512};
        std::size_t maximumStageInterfaceVariables{256};
        std::size_t maximumTargetBindingEntries{256};
        std::size_t maximumSourceMapEntries{4'096};
        std::size_t maximumIdentityBytes{256};
        std::uint32_t maximumBufferBytes{16U * 1024U * 1024U};
    };

    /**
     * @brief Validates final-artifact reflection against the manifest and exact target, then publishes a canonical owned value.
     * @details Synchronous caller-thread work only. The function owns no paths, threads, callbacks, native handles, cancellation,
     * or shutdown state. Inactive optimized bindings remain present under their original logical IDs. Missing or unprovable
     * required evidence fails without fallback.
     * @param manifest Validated logical shader interface.
     * @param target Exact target descriptor whose final artifact produced `candidate`.
     * @param candidate Immutable adapter-owned reflection and mapping evidence.
     * @param admittedIncludeIdentities Canonical sorted include identities from the immutable Asset Pipeline snapshot.
     * @param limits Finite bounds applied before copying or validating compiler output.
     * @return Canonical owned reflection, or a stable ShaderReflectionErrors failure.
     */
    [[nodiscard]] Result<NormalizedShaderReflection> NormalizeShaderReflection(
        const ShaderManifest &manifest, const ShaderTargetRequirement &target, const ShaderReflectionCandidate &candidate,
        const std::vector<std::string> &admittedIncludeIdentities = {}, const ShaderReflectionLimits &limits = {});

    /**
     * @brief Maps a generated compiler location to admitted authored/graph provenance when an exact line mapping exists.
     * @param location Generated diagnostic location to preserve.
     * @param sourceMap Validated canonical source-map records from a NormalizedShaderReflection.
     * @return Authored location with `mapped=true`, unchanged generated location with `mapped=false`, or AllocationFailed.
     */
    [[nodiscard]] Result<ShaderMappedSourceLocation> MapShaderSourceLocation(const ShaderGeneratedSourceLocation &location,
                                                                             const std::vector<ShaderSourceMapEntry> &sourceMap);
}  // namespace Horo::Render
