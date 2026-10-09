#include "Horo/Runtime/Render/RenderFrontend.h"
#include "Horo/Runtime/Render/UiRenderSubmission.h"
#include "Horo/Runtime/Ui/UiErrors.h"

#include <algorithm>

namespace Horo::Render {
    /** @copydoc RenderFrontend::CreateUiImageTexture */
    Result<UiRenderImageTexture> RenderFrontend::CreateUiImageTexture(const Runtime::Ui::UiImageResourceRegistry &registry,
                                                                      const Runtime::Ui::UiImageResourceHandle image,
                                                                      const std::uint32_t page, const std::span<const std::byte> pixels) {
        using namespace Runtime::Ui;
        auto acquired = registry.Acquire(image);
        if (acquired.HasError())
            return Result<UiRenderImageTexture>::Failure(acquired.ErrorValue());
        const auto &snapshot = acquired.Value();
        if (!snapshot.IsDrawable() || page >= snapshot.Resource()->Pages().size() || pixels.empty())
            return Result<UiRenderImageTexture>::Failure(MakeError(UiErrors::ImageResourceInvalid));
        const auto &source = snapshot.Resource()->Pages()[page];
        const RenderTextureDescriptor descriptor{.extent = {source.extent.width, source.extent.height},
                                                 .format = source.colorSpace == UiImageColorSpace::Srgb
                                                               ? RenderTextureFormat::Rgba8UnormSrgb
                                                               : RenderTextureFormat::Rgba8Unorm,
                                                 .usage = RenderTextureUsage::Sampled};
        const auto created = CreateTexture(descriptor, pixels);
        if (created.HasError())
            return Result<UiRenderImageTexture>::Failure(created.ErrorValue());
        const UiRenderResourceReference reference{source.dependency.asset,
                                                  UiRenderResourceRevision::Create(snapshot.Revision().Value()).Value(),
                                                  UiRenderResourceRole::Image,
                                                  source.colorSpace,
                                                  source.sampling,
                                                  snapshot.Resource()->FallbackPolicy(),
                                                  snapshot.Residency()};
        return Result<UiRenderImageTexture>::Success(UiRenderImageTexture{created.Value(), reference, std::move(acquired).Value(), page});
    }

    /** @copydoc RenderFrontend::CreateUiGlyphAtlasTexture */
    Result<UiRenderAtlasTexture> RenderFrontend::CreateUiGlyphAtlasTexture(const Runtime::Ui::UiGlyphAtlas &atlas,
                                                                           const Runtime::Ui::UiGlyphAtlasPageId page,
                                                                           const std::span<const std::byte> pixels) {
        using namespace Runtime::Ui;
        auto source = atlas.AcquirePage(page);
        if (source.HasError())
            return Result<UiRenderAtlasTexture>::Failure(source.ErrorValue());
        if (pixels.empty())
            return Result<UiRenderAtlasTexture>::Failure(MakeError(UiErrors::GlyphAtlasFrameInvalid));
        const auto extent = atlas.PageExtent();
        const RenderTextureDescriptor descriptor{.extent = {extent.width, extent.height},
                                                 .format = atlas.Format() == UiGlyphAtlasRasterFormat::Alpha8
                                                               ? RenderTextureFormat::R8Unorm
                                                               : RenderTextureFormat::Rgba8Unorm,
                                                 .usage = RenderTextureUsage::Sampled};
        const auto created = CreateTexture(descriptor, pixels);
        if (created.HasError())
            return Result<UiRenderAtlasTexture>::Failure(created.ErrorValue());
        return Result<UiRenderAtlasTexture>::Success(UiRenderAtlasTexture{created.Value(), std::move(source).Value()});
    }
}  // namespace Horo::Render
