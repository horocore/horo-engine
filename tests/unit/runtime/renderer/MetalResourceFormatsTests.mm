#include "runtime/renderer/modules/metal/MetalResourceFormats.h"

#import <Metal/Metal.h>
#include <catch2/catch_test_macros.hpp>

namespace Horo::Render::Detail {
    TEST_CASE("Metal format translation covers every backend-neutral texture format") {
        CHECK(MetalPixelFormatValue(RenderTextureFormat::Rgba8Unorm) == MTLPixelFormatRGBA8Unorm);
        CHECK(MetalPixelFormatValue(RenderTextureFormat::Depth24Stencil8) == MTLPixelFormatDepth24Unorm_Stencil8);
        CHECK(MetalPixelFormatValue(RenderTextureFormat::Depth32Float) == MTLPixelFormatDepth32Float);
        CHECK(MetalPixelFormatValue(RenderTextureFormat::R8Unorm) == MTLPixelFormatR8Unorm);
        CHECK(MetalPixelFormatValue(RenderTextureFormat::Rg8Unorm) == MTLPixelFormatRG8Unorm);
        CHECK(MetalPixelFormatValue(RenderTextureFormat::Rgba8UnormSrgb) == MTLPixelFormatRGBA8Unorm_sRGB);
        CHECK(MetalPixelFormatValue(RenderTextureFormat::Bgra8Unorm) == MTLPixelFormatBGRA8Unorm);
        CHECK(MetalPixelFormatValue(RenderTextureFormat::Bgra8UnormSrgb) == MTLPixelFormatBGRA8Unorm_sRGB);
        CHECK(MetalPixelFormatValue(RenderTextureFormat::R16Float) == MTLPixelFormatR16Float);
        CHECK(MetalPixelFormatValue(RenderTextureFormat::Rg16Float) == MTLPixelFormatRG16Float);
        CHECK(MetalPixelFormatValue(RenderTextureFormat::Rgba16Float) == MTLPixelFormatRGBA16Float);
        CHECK(MetalPixelFormatValue(RenderTextureFormat::R32Float) == MTLPixelFormatR32Float);
        CHECK(MetalPixelFormatValue(RenderTextureFormat::Rg32Float) == MTLPixelFormatRG32Float);
        CHECK(MetalPixelFormatValue(RenderTextureFormat::Rgba32Float) == MTLPixelFormatRGBA32Float);
        CHECK(MetalPixelFormatValue(RenderTextureFormat::Depth16Unorm) == MTLPixelFormatDepth16Unorm);
        CHECK(MetalPixelFormatValue(RenderTextureFormat::Depth32FloatStencil8) == MTLPixelFormatDepth32Float_Stencil8);
        CHECK(MetalPixelFormatValue(static_cast<RenderTextureFormat>(255)) == static_cast<std::uint64_t>(MTLPixelFormatInvalid));
    }
}  // namespace Horo::Render::Detail
