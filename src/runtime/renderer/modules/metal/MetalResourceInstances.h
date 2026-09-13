#pragma once

#include "Horo/Runtime/Render/RenderBackend.h"

#ifdef __OBJC__
#import <Metal/Metal.h>

namespace Horo::Render::Detail {
    struct MetalBufferInstance {
        __strong id<MTLBuffer> buffer{nil};
        RenderBufferUsage usage{RenderBufferUsage::None};
        RenderMemoryPoolId pool;
    };

    struct MetalMeshInstance {
        __strong id<MTLBuffer> vertexBuffer{nil};
        __strong id<MTLBuffer> indexBuffer{nil};
    };

    struct MetalTextureInstance {
        __strong id<MTLTexture> texture{nil};
        RenderTextureDescriptor descriptor;
        RenderMemoryPoolId pool;
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
