#pragma once
/** @file MetalUiPaintKernel.h @brief Private retained UI raster pipelines and exact selected-runtime identity. */
#include "Horo/Runtime/Render/UiPaintKernel.h"

#include <array>

#ifdef __OBJC__
#import <Metal/Metal.h>

namespace Horo::Render::Detail {
    /** @brief Pins one concrete resource runtime identity while cooked raster preparations survive retirement. */
    struct MetalUiPaintOwnerIdentity final {};

    /** @brief Immutable raster variants, stencil transitions, fallback sampling and concrete device-epoch ownership. */
    class MetalUiPaintKernel final : public IResidentUiPaintKernel {
    public:
        static constexpr NSUInteger VertexBufferBinding = 30;
        __strong id<MTLRenderPipelineState> color{nil};
        __strong id<MTLRenderPipelineState> maskedColor{nil};
        __strong id<MTLRenderPipelineState> stencil{nil};
        __strong id<MTLRenderPipelineState> multiplyCoverage{nil};
        __strong id<MTLRenderPipelineState> maskedMultiplyCoverage{nil};
        std::array<id<MTLDepthStencilState>, 3> stencilStates{};
        __strong id<MTLDepthStencilState> unmasked{nil};
        std::array<id<MTLSamplerState>, 12> samplers{};
        __strong id<MTLTexture> white{nil};
        std::array<NSUInteger, 4> bindings{};
        std::shared_ptr<const MetalUiPaintOwnerIdentity> owner;
        std::uint64_t incarnation{};
        MTLPixelFormat colorFormat{MTLPixelFormatInvalid};
        MTLPixelFormat stencilFormat{MTLPixelFormatInvalid};
    };
}  // namespace Horo::Render::Detail
#endif
