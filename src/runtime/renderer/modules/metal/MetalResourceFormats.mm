#include "MetalResourceFormats.h"

#import <Metal/Metal.h>
#include <array>
#include <ranges>
#include <utility>

namespace Horo::Render::Detail {
    /** @copydoc MetalPixelFormatValue */
    std::uint64_t MetalPixelFormatValue(const RenderTextureFormat format) noexcept {
        constexpr std::array<std::pair<RenderTextureFormat, MTLPixelFormat>, 16> formats{{
            {RenderTextureFormat::Rgba8Unorm, MTLPixelFormatRGBA8Unorm},
            {RenderTextureFormat::Depth24Stencil8, MTLPixelFormatDepth24Unorm_Stencil8},
            {RenderTextureFormat::Depth32Float, MTLPixelFormatDepth32Float},
            {RenderTextureFormat::R8Unorm, MTLPixelFormatR8Unorm},
            {RenderTextureFormat::Rg8Unorm, MTLPixelFormatRG8Unorm},
            {RenderTextureFormat::Rgba8UnormSrgb, MTLPixelFormatRGBA8Unorm_sRGB},
            {RenderTextureFormat::Bgra8Unorm, MTLPixelFormatBGRA8Unorm},
            {RenderTextureFormat::Bgra8UnormSrgb, MTLPixelFormatBGRA8Unorm_sRGB},
            {RenderTextureFormat::R16Float, MTLPixelFormatR16Float},
            {RenderTextureFormat::Rg16Float, MTLPixelFormatRG16Float},
            {RenderTextureFormat::Rgba16Float, MTLPixelFormatRGBA16Float},
            {RenderTextureFormat::R32Float, MTLPixelFormatR32Float},
            {RenderTextureFormat::Rg32Float, MTLPixelFormatRG32Float},
            {RenderTextureFormat::Rgba32Float, MTLPixelFormatRGBA32Float},
            {RenderTextureFormat::Depth16Unorm, MTLPixelFormatDepth16Unorm},
            {RenderTextureFormat::Depth32FloatStencil8, MTLPixelFormatDepth32Float_Stencil8},
        }};
        const auto found = std::ranges::find(formats, format, [](const auto &entry) {
            return entry.first;
        });
        return found == formats.end() ? static_cast<std::uint64_t>(MTLPixelFormatInvalid) : static_cast<std::uint64_t>(found->second);
    }
}  // namespace Horo::Render::Detail
