#include "EditorGuiRendererVulkan.h"
#include "editor/renderer/EditorRenderMemoryScopes.h"

#include <algorithm>
#include <imgui.h>
#include <limits>
#include <vector>

namespace Horo::Editor {
    /** @copydoc EditorGuiRendererVulkan::Realize */
    Result<void> EditorGuiRendererVulkan::Realize(const Render::ResourceOperationId operation) {
        if (const auto processed = frontend_.ProcessResourceRequests(); processed.HasError())
            return processed;
        return frontend_.ResourceOperationResult(operation);
    }

    /** @copydoc EditorGuiRendererVulkan::ReleaseTexture */
    void EditorGuiRendererVulkan::ReleaseTexture(Texture &texture) noexcept {
        if (texture.view.IsValid())
            static_cast<void>(frontend_.ReleaseTextureView(texture.view));
        if (texture.texture.IsValid())
            static_cast<void>(frontend_.ReleaseTexture(texture.texture));
        if (texture.staging.IsValid())
            static_cast<void>(frontend_.ReleaseBuffer(texture.staging));
        texture = {};
    }

    /** @copydoc EditorGuiRendererVulkan::CreateTexture */
    Result<std::uintptr_t> EditorGuiRendererVulkan::CreateTexture(const EditorRgba8ImageView &image) {
        using Outcome = Result<std::uintptr_t>;
        if (const auto valid = Validate(); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        const auto free = std::ranges::find(textures_, std::uintptr_t{}, &Texture::identity);
        if (!image.IsValid() || free == textures_.end() || nextIdentity_ == std::numeric_limits<std::uintptr_t>::max())
            return Outcome::Failure(MakeError(RendererErrors::GuiTextureInvalid));
        Texture pending;
        const auto fail = [&](const Error &error) {
            ReleaseTexture(pending);
            return Outcome::Failure(error);
        };
        const auto staging = frontend_.CreateBuffer(RenderMemoryScopes::GuiResources,
                                                    {.byteSize = image.pixels.size(),
                                                     .usage = Render::RenderBufferUsage::CopySource,
                                                     .access = Render::RenderBufferAccess::HostVisible},
                                                    std::as_bytes(image.pixels));
        if (staging.HasError())
            return fail(staging.ErrorValue());
        pending.staging = staging.Value().handle;
        if (const auto ready = Realize(staging.Value().operation); ready.HasError())
            return fail(ready.ErrorValue());
        const auto texture =
            frontend_.CreateTexture(RenderMemoryScopes::GuiResources,
                                    {.extent = {image.width, image.height},
                                     .format = Render::RenderTextureFormat::Rgba8Unorm,
                                     .usage = Render::RenderTextureUsage::Sampled | Render::RenderTextureUsage::CopyDestination});
        if (texture.HasError())
            return fail(texture.ErrorValue());
        pending.texture = texture.Value().handle;
        if (const auto ready = Realize(texture.Value().operation); ready.HasError())
            return fail(ready.ErrorValue());
        const auto view = frontend_.CreateTextureView(
            {.texture = pending.texture, .format = Render::RenderTextureFormat::Rgba8Unorm, .aspect = Render::RenderTextureAspect::Color});
        if (view.HasError())
            return fail(view.ErrorValue());
        pending.view = view.Value().handle;
        if (const auto ready = Realize(view.Value().operation); ready.HasError())
            return fail(ready.ErrorValue());
        pending.identity = nextIdentity_++;
        *free = pending;
        return Outcome::Success(pending.identity);
    }

    /** @copydoc EditorGuiRendererVulkan::DestroyTexture */
    void EditorGuiRendererVulkan::DestroyTexture(const std::uintptr_t identity) noexcept {
        if (owner_ != std::this_thread::get_id() || identity == fontIdentity_)
            return;
        const auto found = std::ranges::find(textures_, identity, &Texture::identity);
        if (found != textures_.end())
            ReleaseTexture(*found);
    }

    /** @copydoc EditorGuiRendererVulkan::Resolve */
    Result<Render::GuiSampledResource> EditorGuiRendererVulkan::Resolve(const std::uintptr_t identity) const {
        const auto texture = std::ranges::find(textures_, identity, &Texture::identity);
        if (identity != 0 && texture != textures_.end())
            return Result<Render::GuiSampledResource>::Success({texture->view, sampler_, texture->staging});
        const auto &viewport = ViewportBinding();
        if (identity != 0 && viewport.textureId == identity && viewport.sampledView.IsValid())
            return Result<Render::GuiSampledResource>::Success(
                {viewport.sampledView, viewport.sampledSampler.IsValid() ? viewport.sampledSampler : sampler_, {}});
        return Result<Render::GuiSampledResource>::Failure(MakeError(RendererErrors::GuiCompositionInvalid));
    }

    /** @copydoc EditorGuiRendererVulkan::InitializeSlots */
    Result<void> EditorGuiRendererVulkan::InitializeSlots() {
        // Load-time zero initialization defines the complete allocation; the encoder replaces only this retired slot's used prefix.
        const std::array<std::size_t, 2> sizes{MaximumVertices * sizeof(ImDrawVert), MaximumIndices * sizeof(ImDrawIdx)};
        for (std::uint32_t slot = 0; slot < device_.frames; ++slot)
            for (std::size_t index = 0; index < sizes.size(); ++index) {
                const std::vector<std::byte> zero(sizes[index]);
                const auto buffer =
                    frontend_.CreateBuffer(RenderMemoryScopes::GuiResources,
                                           {.byteSize = sizes[index],
                                            .usage = index == 0 ? Render::RenderBufferUsage::Vertex : Render::RenderBufferUsage::Index,
                                            .access = Render::RenderBufferAccess::HostVisible},
                                           zero);
                if (buffer.HasError())
                    return Result<void>::Failure(buffer.ErrorValue());
                slots_[slot].buffers[index] = buffer.Value().handle;
                if (const auto ready = Realize(buffer.Value().operation); ready.HasError())
                    return ready;
            }
        return Result<void>::Success();
    }
}  // namespace Horo::Editor
