#pragma once

/** @file
 * @brief Backend-private cooked material metadata; resident generations and completion remain frontend-owned.
 */
#include "OpenGLMaterialExecution.h"
#include "OpenGLShaderRealization.h"

#include <algorithm>
#include <array>
#include <optional>
#include <tuple>
#include <unordered_map>

namespace Horo::Render::Detail {
    /** @brief Native shader metadata; frontend registry remains the sole resident identity/dependency authority. */
    struct OpenGLShaderModule {
        RenderShaderModuleDescriptor descriptor;
        std::size_t cookedBytes{0};
    };

    /** @brief Immutable native program metadata subordinate to frontend generation and dependency pins. */
    struct OpenGLGraphicsPipeline {
        MaterialBindingLayout layout;
        RenderGraphicsPipelineState state;
        PipelineCacheKey key;
        OpenGLMaterialContextFeatures features;
    };

    /** @brief Immutable selected-backend recipe, retained by frontend material and submitted-frame leases. */
    class OpenGLMaterialBinding final : public IResidentMaterialBinding {
    public:
        const IRenderBackend *owner{nullptr};
        std::uint32_t program{0};
        std::array<OpenGLMaterialSlot, 128> slots;
        std::size_t count{0};
    };

    /** @brief Exact attachment facts retained with the native target while its frontend dependencies remain pinned. */
    struct OpenGLMaterialTarget {
        FramebufferExtent extent;
        std::uint32_t colorTexture{0};
        std::uint32_t depthTexture{0};
        RenderTextureFormat colorFormat{RenderTextureFormat::Rgba8Unorm};
        std::optional<RenderTextureFormat> depthFormat;
        std::uint32_t sampleCount{1};
    };

    /** @brief Checks every cooked packing/placement fact, not only a caller-supplied interface digest. */
    inline bool SameMaterialLayout(const MaterialBindingLayout &left, const MaterialBindingLayout &right) {
        return left.backend == right.backend && left.shaderInterface == right.shaderInterface &&
               std::ranges::equal(left.resources, right.resources, [](const auto &a, const auto &b) {
            return std::tie(a.id, a.kind, a.access, a.arrayCount, a.stages, a.active) ==
                   std::tie(b.id, b.kind, b.access, b.arrayCount, b.stages, b.active);
        }) && std::ranges::equal(left.parameters, right.parameters, [](const auto &a, const auto &b) {
            return std::tie(a.id, a.binding, a.type, a.rows, a.columns, a.arrayCount, a.byteOffset, a.arrayStride, a.matrixStride,
                            a.columnMajor, a.active) == std::tie(b.id, b.binding, b.type, b.rows, b.columns, b.arrayCount, b.byteOffset,
                                                                 b.arrayStride, b.matrixStride, b.columnMajor, b.active);
        }) && std::ranges::equal(left.targetBindings, right.targetBindings, [](const auto &a, const auto &b) {
            return std::tie(a.id, a.generatedHelperIndex, a.nativeSpace, a.nativeBinding, a.nativeName, a.pairedSampler, a.active) ==
                   std::tie(b.id, b.generatedHelperIndex, b.nativeSpace, b.nativeBinding, b.nativeName, b.pairedSampler, b.active);
        });
    }

    /** @brief Compares the full cooked target contract before joining resident stages. */
    inline bool SameShaderTarget(const ShaderTargetRequirement &left, const ShaderTargetRequirement &right) noexcept {
        return left.backend == right.backend && left.payloadFormat == right.payloadFormat &&
               left.descriptorVersion == right.descriptorVersion && left.interfaceSchemaVersion == right.interfaceSchemaVersion &&
               left.maximumBindings == right.maximumBindings && left.maximumInlineConstantBytes == right.maximumInlineConstantBytes &&
               left.supportsCompute == right.supportsCompute && left.supportsStorageResources == right.supportsStorageResources;
    }

    /** @brief Holds one concrete backend's prepared metadata without introducing another resource lifetime authority. */
    struct OpenGLPreparedMaterialState {
        std::unordered_map<std::uint32_t, OpenGLShaderModule> shaderModules;
        std::unordered_map<std::uint32_t, OpenGLGraphicsPipeline> graphicsPipelines;
        std::unordered_map<std::uint32_t, RenderSamplerDescriptor> samplerDescriptors;
        std::unordered_map<std::uint32_t, RenderMeshDescriptor> meshDescriptors;
        std::unordered_map<std::uint32_t, OpenGLMaterialTarget> materialTargets;

        /** @brief Clears native preparation in program/sampler/shader order after the owner drains completion and before context
         * destruction.
         * @param shaders Selected context's nonthrowing shader/program destruction callbacks.
         * @param samplers Selected context's nonthrowing sampler destruction callback.
         */
        void DestroyPrepared(const OpenGLShaderFunctions &shaders, const OpenGLSamplerFunctions &samplers) noexcept {
            for (const auto &[program, metadata] : graphicsPipelines)
                shaders.deleteProgram(program);
            graphicsPipelines.clear();
            for (const auto &[sampler, descriptor] : samplerDescriptors)
                samplers.destroy(sampler);
            samplerDescriptors.clear();
            for (const auto &[shader, metadata] : shaderModules)
                shaders.deleteShader(shader);
            shaderModules.clear();
        }
    };
}  // namespace Horo::Render::Detail
