#pragma once
/** @file MetalMaterialEncoding.h @brief Shared prepared-only native material binding without allocation. */
#include "MetalPreparedMaterialState.h"

namespace Horo::Render::Detail {
    inline bool MetalMaterialVisible(const ShaderStageVisibility stages, const ShaderStageVisibility stage) noexcept {
        return (static_cast<unsigned>(stages) & static_cast<unsigned>(stage)) != 0;
    }

    inline void BindMetalMaterialSlot(id<MTLRenderCommandEncoder> encoder, const MetalMaterialSlot &slot) {
        if (slot.kind == MaterialResidentResourceKind::Buffer) {
            if (MetalMaterialVisible(slot.stages, ShaderStageVisibility::Vertex))
                [encoder setVertexBuffer:slot.buffer offset:slot.offset atIndex:slot.binding];
            if (MetalMaterialVisible(slot.stages, ShaderStageVisibility::Fragment))
                [encoder setFragmentBuffer:slot.buffer offset:slot.offset atIndex:slot.binding];
        } else if (slot.kind == MaterialResidentResourceKind::TextureView) {
            if (MetalMaterialVisible(slot.stages, ShaderStageVisibility::Vertex))
                [encoder setVertexTexture:slot.texture atIndex:slot.binding];
            if (MetalMaterialVisible(slot.stages, ShaderStageVisibility::Fragment))
                [encoder setFragmentTexture:slot.texture atIndex:slot.binding];
        } else {
            if (MetalMaterialVisible(slot.stages, ShaderStageVisibility::Vertex))
                [encoder setVertexSamplerState:slot.sampler atIndex:slot.binding];
            if (MetalMaterialVisible(slot.stages, ShaderStageVisibility::Fragment))
                [encoder setFragmentSamplerState:slot.sampler atIndex:slot.binding];
        }
    }

    /** @brief Applies the already-prepared canonical material without allocating descriptor or argument storage. */
    inline void BindMetalMaterial(id<MTLRenderCommandEncoder> encoder, const MetalMaterialBinding &material) {
        [encoder setRenderPipelineState:material.pipeline];
        [encoder setDepthStencilState:material.depth];
        [encoder setCullMode:material.state.cull == RenderCullMode::None    ? MTLCullModeNone
                             : material.state.cull == RenderCullMode::Front ? MTLCullModeFront
                                                                            : MTLCullModeBack];
        [encoder setFrontFacingWinding:material.state.frontFace == RenderFrontFace::Clockwise ? MTLWindingClockwise
                                                                                              : MTLWindingCounterClockwise];
        for (const auto &slot : std::span{material.slots}.first(material.count))
            BindMetalMaterialSlot(encoder, slot);
    }
}  // namespace Horo::Render::Detail
