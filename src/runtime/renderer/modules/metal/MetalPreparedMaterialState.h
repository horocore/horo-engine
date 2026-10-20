#pragma once
/** @file MetalPreparedMaterialState.h
 * @brief Private Metal preparation metadata subordinate to frontend generation pins.
 */
#include "Horo/Runtime/Render/MaterialBinding.h"
#include "Horo/Runtime/Render/RenderShaderResources.h"

#import <Metal/Metal.h>
#include <algorithm>
#include <array>
#include <tuple>

namespace Horo::Render::Detail {
    /** @brief Fixed MeshVertex fetch slot, reserved from material buffers and shared with future draw encoding. */
    inline constexpr std::uint32_t MetalMeshVertexBufferIndex{30};

    /** @brief Exact cooked function; no source compiler or borrowed package storage. */
    struct MetalShaderModule {
        __strong id<MTLFunction> function{nil};
        RenderShaderModuleDescriptor descriptor;
        std::size_t cookedBytes{0};
    };

    /** @brief Immutable native pipeline and final reflected uniform block sizes. */
    struct MetalGraphicsPipeline {
        __strong id<MTLRenderPipelineState> pipeline{nil};
        __strong id<MTLDepthStencilState> depth{nil};
        MaterialBindingLayout layout;
        RenderGraphicsPipelineState state;
        PipelineCacheKey key;
        std::array<std::size_t, 31> bufferSizes{};
    };

    /** @brief Exact immutable sampling policy. */
    struct MetalSamplerInstance {
        __strong id<MTLSamplerState> sampler{nil};
        RenderSamplerDescriptor descriptor;
    };

    /** @brief Retains native objects independently of preparation metadata lookup lifetime. */
    struct MetalMaterialSlot {
        MaterialResidentResourceKind kind{MaterialResidentResourceKind::Buffer};
        __strong id<MTLBuffer> buffer{nil};
        __strong id<MTLTexture> texture{nil};
        __strong id<MTLSamplerState> sampler{nil};
        ShaderStageVisibility stages{ShaderStageVisibility::None};
        std::uint32_t binding{0};
        std::size_t offset{0};
    };

    /** @brief Owner-created immutable native table; frontend lease pins placed backing until completion. */
    class MetalMaterialBinding final : public IResidentMaterialBinding {
    public:
        const void *owner{nullptr};
        __strong id<MTLRenderPipelineState> pipeline{nil};
        __strong id<MTLDepthStencilState> depth{nil};
        RenderGraphicsPipelineState state;
        std::array<MetalMaterialSlot, 128> slots;
        std::size_t count{0};
    };

    /** @brief Checks logical declarations and exact cooked native packing without another layout authority. */
    inline bool SameMetalMaterialLayout(const MaterialBindingLayout &left, const MaterialBindingLayout &right) {
        const auto resourceFields = [](const ShaderReflectedBinding &resource) {
            return std::tie(resource.id, resource.active, resource.kind, resource.stages, resource.access, resource.arrayCount);
        };
        const auto parameterFields = [](const ShaderReflectedParameter &parameter) {
            return std::tie(parameter.id, parameter.binding, parameter.active, parameter.byteOffset, parameter.arrayStride,
                            parameter.matrixStride, parameter.columnMajor, parameter.type, parameter.rows, parameter.columns,
                            parameter.arrayCount);
        };
        const auto targetFields = [](const ShaderTargetBindingMapEntry &target) {
            return std::tie(target.id, target.active, target.generatedHelperIndex, target.nativeSpace, target.nativeBinding,
                            target.nativeName, target.pairedSampler);
        };
        return left.backend == right.backend && left.shaderInterface == right.shaderInterface &&
               std::ranges::equal(left.resources, right.resources, {}, resourceFields, resourceFields) &&
               std::ranges::equal(left.parameters, right.parameters, {}, parameterFields, parameterFields) &&
               std::ranges::equal(left.targetBindings, right.targetBindings, {}, targetFields, targetFields);
    }

    /** @brief Compares the complete cooked target before joining resident native stages. */
    inline bool SameMetalShaderTarget(const ShaderTargetRequirement &left, const ShaderTargetRequirement &right) noexcept {
        const auto fields = [](const ShaderTargetRequirement &target) {
            return std::tie(target.backend, target.payloadFormat, target.descriptorVersion, target.interfaceSchemaVersion,
                            target.maximumBindings, target.maximumInlineConstantBytes, target.supportsCompute,
                            target.supportsStorageResources);
        };
        return fields(left) == fields(right);
    }
}  // namespace Horo::Render::Detail
