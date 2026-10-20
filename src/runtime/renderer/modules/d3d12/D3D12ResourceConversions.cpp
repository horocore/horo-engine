#include "D3D12ResourceInternals.h"

#if defined(_WIN32) && defined(_M_X64)
namespace Horo::Render::D3D12Detail {
    /** @brief Preserves device removal/OOM separately from ordinary native resource errors. */
    Error ResourceError(const char *stage, const char *message, HRESULT cause) {
        const char *code = cause == DXGI_ERROR_DEVICE_REMOVED || cause == DXGI_ERROR_DEVICE_RESET ? "render.d3d12.device_lost"
                           : cause == E_OUTOFMEMORY                                               ? "render.d3d12.out_of_memory"
                                                                                                  : stage;
        return D3D12InitializationError(code, message, cause);
    }

    /** @brief Maps every canonical uncompressed storage format without native public values. */
    DXGI_FORMAT NativeFormat(RenderTextureFormat format) {
        constexpr std::array formats{DXGI_FORMAT_R8G8B8A8_UNORM,     DXGI_FORMAT_D24_UNORM_S8_UINT,
                                     DXGI_FORMAT_D32_FLOAT,          DXGI_FORMAT_R8_UNORM,
                                     DXGI_FORMAT_R8G8_UNORM,         DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                                     DXGI_FORMAT_B8G8R8A8_UNORM,     DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                     DXGI_FORMAT_R16_FLOAT,          DXGI_FORMAT_R16G16_FLOAT,
                                     DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R32_FLOAT,
                                     DXGI_FORMAT_R32G32_FLOAT,       DXGI_FORMAT_R32G32B32A32_FLOAT,
                                     DXGI_FORMAT_D16_UNORM,          DXGI_FORMAT_D32_FLOAT_S8X24_UINT};
        const auto index = static_cast<std::size_t>(format);
        return index < formats.size() ? formats[index] : DXGI_FORMAT_UNKNOWN;
    }

    /** @brief Identifies depth storage; typed depth resources are attachment-only in this implementation. */
    bool IsDepth(RenderTextureFormat format) {
        using enum RenderTextureFormat;
        return format == Depth16Unorm || format == Depth24Stencil8 || format == Depth32Float || format == Depth32FloatStencil8;
    }

    /** @brief Creates a native buffer shape; heap policy is selected separately. */
    D3D12_RESOURCE_DESC BufferShape(std::size_t bytes) {
        D3D12_RESOURCE_DESC shape{};
        shape.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        shape.Width = bytes;
        shape.Height = 1;
        shape.DepthOrArraySize = 1;
        shape.MipLevels = 1;
        shape.SampleDesc.Count = 1;
        shape.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return shape;
    }

    /** @brief Converts texture access requirements independently of shape dimensions. */
    D3D12_RESOURCE_FLAGS TextureFlags(const RenderTextureDescriptor &descriptor) {
        D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
        if (HasTextureUsage(descriptor.usage, RenderTextureUsage::RenderAttachment))
            flags |= IsDepth(descriptor.format) ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (HasTextureUsage(descriptor.usage, RenderTextureUsage::Storage))
            flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return flags;
    }

    /** @brief Translates validated finite dimensions without truncating native 16-bit fields. */
    D3D12_RESOURCE_DESC TextureShape(const RenderTextureDescriptor &descriptor) {
        D3D12_RESOURCE_DESC shape{};
        shape.Dimension = descriptor.dimension == RenderTextureDimension::OneD     ? D3D12_RESOURCE_DIMENSION_TEXTURE1D
                          : descriptor.dimension == RenderTextureDimension::ThreeD ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                                                                   : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        shape.Width = descriptor.extent.width;
        shape.Height = descriptor.extent.height;
        shape.DepthOrArraySize =
            static_cast<UINT16>(descriptor.dimension == RenderTextureDimension::ThreeD ? descriptor.depth : descriptor.layerCount);
        shape.MipLevels = static_cast<UINT16>(descriptor.mipCount);
        shape.Format = NativeFormat(descriptor.format);
        if (IsDepth(descriptor.format) && HasTextureUsage(descriptor.usage, RenderTextureUsage::Sampled)) {
            switch (descriptor.format) {
                case RenderTextureFormat::Depth16Unorm:
                    shape.Format = DXGI_FORMAT_R16_TYPELESS;
                    break;
                case RenderTextureFormat::Depth24Stencil8:
                    shape.Format = DXGI_FORMAT_R24G8_TYPELESS;
                    break;
                case RenderTextureFormat::Depth32Float:
                    shape.Format = DXGI_FORMAT_R32_TYPELESS;
                    break;
                case RenderTextureFormat::Depth32FloatStencil8:
                    shape.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
                    break;
                default:
                    break;
            }
        }
        shape.SampleDesc.Count = descriptor.sampleCount;
        shape.Flags = TextureFlags(descriptor);
        return shape;
    }

    /** @brief Chooses the exact depth/stencil plane for a shader view of typeless depth storage. */
    DXGI_FORMAT ShaderViewFormat(const RenderTextureViewDescriptor &view) {
        using enum RenderTextureFormat;
        switch (view.format) {
            case Depth16Unorm:
                return DXGI_FORMAT_R16_UNORM;
            case Depth24Stencil8:
                return view.aspect == RenderTextureAspect::Stencil ? DXGI_FORMAT_X24_TYPELESS_G8_UINT : DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            case Depth32Float:
                return DXGI_FORMAT_R32_FLOAT;
            case Depth32FloatStencil8:
                return view.aspect == RenderTextureAspect::Stencil ? DXGI_FORMAT_X32_TYPELESS_G8X24_UINT
                                                                   : DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            default:
                return NativeFormat(view.format);
        }
    }

    /** @brief Configures exact multisampled or regular ConfigureTwoD shader dimensions. */
    void ConfigureTwoD(D3D12_SHADER_RESOURCE_VIEW_DESC &native, const RenderTextureViewDescriptor &view, bool multisampled) {
        native.ViewDimension = multisampled ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
        if (!multisampled) {
            native.Texture2D.MostDetailedMip = view.baseMip;
            native.Texture2D.MipLevels = view.mipCount;
            native.Texture2D.PlaneSlice = view.aspect == RenderTextureAspect::Stencil ? 1 : 0;
        }
    }

    /** @brief Configures exact multisampled or regular ConfigureTwoDArray shader dimensions. */
    void ConfigureTwoDArray(D3D12_SHADER_RESOURCE_VIEW_DESC &native, const RenderTextureViewDescriptor &view, bool multisampled) {
        native.ViewDimension = multisampled ? D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY : D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        if (multisampled) {
            native.Texture2DMSArray.FirstArraySlice = view.baseLayer;
            native.Texture2DMSArray.ArraySize = view.layerCount;
        } else {
            native.Texture2DArray.MostDetailedMip = view.baseMip;
            native.Texture2DArray.MipLevels = view.mipCount;
            native.Texture2DArray.FirstArraySlice = view.baseLayer;
            native.Texture2DArray.ArraySize = view.layerCount;
            native.Texture2DArray.PlaneSlice = view.aspect == RenderTextureAspect::Stencil ? 1 : 0;
        }
    }

    /** @brief Expands canonical dimensions/ranges into an exact SRV descriptor. */
    D3D12_SHADER_RESOURCE_VIEW_DESC ShaderView(const RenderTextureViewDescriptor &view, bool multisampled) {
        D3D12_SHADER_RESOURCE_VIEW_DESC native{};
        native.Format = ShaderViewFormat(view);
        native.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        using enum RenderTextureViewDimension;
        switch (view.dimension) {
            case OneD:
                native.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
                native.Texture1D.MostDetailedMip = view.baseMip;
                native.Texture1D.MipLevels = view.mipCount;
                break;
            case TwoD:
                ConfigureTwoD(native, view, multisampled);
                break;
            case TwoDArray:
                ConfigureTwoDArray(native, view, multisampled);
                break;
            case Cube:
                native.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
                native.TextureCube.MostDetailedMip = view.baseMip;
                native.TextureCube.MipLevels = view.mipCount;
                break;
            case CubeArray:
                native.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
                native.TextureCubeArray.MostDetailedMip = view.baseMip;
                native.TextureCubeArray.MipLevels = view.mipCount;
                native.TextureCubeArray.First2DArrayFace = view.baseLayer;
                native.TextureCubeArray.NumCubes = view.layerCount / 6;
                break;
            case ThreeD:
                native.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
                native.Texture3D.MostDetailedMip = view.baseMip;
                native.Texture3D.MipLevels = view.mipCount;
                break;
        }
        return native;
    }

    /** @brief Produces an attachment view; cube storage is addressed as ordinary array slices. */
    D3D12_RENDER_TARGET_VIEW_DESC ColorView(const RenderTextureViewDescriptor &view, const RenderTextureDescriptor &texture) {
        D3D12_RENDER_TARGET_VIEW_DESC native{};
        native.Format = NativeFormat(view.format);
        using enum RenderTextureDimension;
        if (texture.dimension == OneD) {
            native.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;
            native.Texture1D.MipSlice = view.baseMip;
        } else if (texture.dimension == ThreeD) {
            native.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
            native.Texture3D.MipSlice = view.baseMip;
            native.Texture3D.WSize = std::max(1U, texture.depth >> view.baseMip);
        } else if (texture.layerCount > 1) {
            native.ViewDimension = texture.sampleCount > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY : D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            if (texture.sampleCount > 1) {
                native.Texture2DMSArray.FirstArraySlice = view.baseLayer;
                native.Texture2DMSArray.ArraySize = view.layerCount;
            } else {
                native.Texture2DArray.MipSlice = view.baseMip;
                native.Texture2DArray.FirstArraySlice = view.baseLayer;
                native.Texture2DArray.ArraySize = view.layerCount;
            }
        } else {
            native.ViewDimension = texture.sampleCount > 1 ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
            if (texture.sampleCount == 1)
                native.Texture2D.MipSlice = view.baseMip;
        }
        return native;
    }

    /** @brief Produces a typed depth attachment with the same exact array/sample range. */
    D3D12_DEPTH_STENCIL_VIEW_DESC DepthView(const RenderTextureViewDescriptor &view, const RenderTextureDescriptor &texture) {
        D3D12_DEPTH_STENCIL_VIEW_DESC native{};
        native.Format = NativeFormat(view.format);
        if (texture.dimension == RenderTextureDimension::OneD) {
            native.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE1D;
            native.Texture1D.MipSlice = view.baseMip;
        } else if (texture.layerCount > 1) {
            native.ViewDimension = texture.sampleCount > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY : D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
            if (texture.sampleCount > 1) {
                native.Texture2DMSArray.FirstArraySlice = view.baseLayer;
                native.Texture2DMSArray.ArraySize = view.layerCount;
            } else {
                native.Texture2DArray.MipSlice = view.baseMip;
                native.Texture2DArray.FirstArraySlice = view.baseLayer;
                native.Texture2DArray.ArraySize = view.layerCount;
            }
        } else {
            native.ViewDimension = texture.sampleCount > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
            if (texture.sampleCount == 1)
                native.Texture2D.MipSlice = view.baseMip;
        }
        return native;
    }

    /** @brief Produces storage descriptors, rejecting MSAA before this conversion. */
    D3D12_UNORDERED_ACCESS_VIEW_DESC StorageView(const RenderTextureViewDescriptor &view, const RenderTextureDescriptor &texture) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC native{};
        native.Format = NativeFormat(view.format);
        if (texture.dimension == RenderTextureDimension::OneD) {
            native.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
            native.Texture1D.MipSlice = view.baseMip;
        } else if (texture.dimension == RenderTextureDimension::ThreeD) {
            native.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            native.Texture3D.MipSlice = view.baseMip;
            native.Texture3D.WSize = std::max(1U, texture.depth >> view.baseMip);
        } else if (texture.layerCount > 1) {
            native.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            native.Texture2DArray.MipSlice = view.baseMip;
            native.Texture2DArray.FirstArraySlice = view.baseLayer;
            native.Texture2DArray.ArraySize = view.layerCount;
        } else {
            native.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            native.Texture2D.MipSlice = view.baseMip;
        }
        return native;
    }

}  // namespace Horo::Render::D3D12Detail
#endif
