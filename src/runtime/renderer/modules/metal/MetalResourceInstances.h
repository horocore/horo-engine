#pragma once

#include "Horo/Runtime/Render/RenderBackend.h"

#ifdef __OBJC__
#import <Metal/Metal.h>

namespace Horo::Render::Detail {
    /** @brief Owner-thread command evidence protecting placed storage against early aliasing. */
    struct MetalResidentUse {
        __strong id<MTLCommandBuffer> last{nil};
        __strong id<MTLCommandBuffer> previous{nil};
        bool retired{false};
    };

    struct MetalBufferInstance {
        __strong id<MTLBuffer> buffer{nil};
        RenderBufferUsage usage{RenderBufferUsage::None};
        RenderMemoryPoolId pool;
        MetalResidentUse use;
        std::uint64_t lightTableRevision{};
        LightCullingDispatch lightTableDispatch;
    };

    struct MetalMeshInstance {
        __strong id<MTLBuffer> vertexBuffer{nil};
        __strong id<MTLBuffer> indexBuffer{nil};
    };

    struct MetalTextureInstance {
        __strong id<MTLTexture> texture{nil};
        RenderTextureDescriptor descriptor;
        RenderMemoryPoolId pool;
        MetalResidentUse use;
    };

    struct MetalTextureViewInstance {
        __strong id<MTLTexture> texture{nil};
        RenderTextureFormat format{RenderTextureFormat::Rgba8Unorm};
        RenderTextureAspect aspect{RenderTextureAspect::Color};
        RenderTextureUsage usage{RenderTextureUsage::None};
    };

    struct MetalRenderTargetInstance {
        __strong id<MTLTexture> colorTexture{nil};
        __strong id<MTLTexture> depthTexture{nil};
    };
}  // namespace Horo::Render::Detail
#endif
