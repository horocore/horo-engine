/** @file EditorViewportPreparedFrameOpenGL.cpp
 * @brief Prepared-frame context queries and canonical execution for the OpenGL viewport peer. */
#include "EditorViewportRendererOpenGL.h"
#include "Horo/Runtime/Render/RenderFrontend.h"

#include <algorithm>

namespace Horo::Editor {
    /** @copydoc IEditorViewportRenderer::RequestImageCapture */
    Result<std::uint64_t> EditorViewportRendererOpenGL::RequestImageCapture(EditorViewportImageRegion region,
                                                                            std::function<bool()> authorized) {
        const auto extent = resources_.AllocatedExtent();
        if (!initialized_ || !requestedExtent_.IsValid())
            return Result<std::uint64_t>::Failure(MakeError(RendererErrors::ViewportCapabilityUnavailable));
        return imageCapture_.Request(region, resources_.Target(), extent.width, extent.height, std::move(authorized));
    }

    /** @copydoc IEditorViewportRenderer::ReadImageCapture */
    std::optional<EditorViewportImageSnapshot> EditorViewportRendererOpenGL::ReadImageCapture(std::uint64_t id) {
        return imageCapture_.Read(id);
    }

    /** @copydoc IEditorViewportRenderer::CancelImageCapture */
    bool EditorViewportRendererOpenGL::CancelImageCapture(std::uint64_t id) noexcept {
        return imageCapture_.Cancel(id);
    }

    /** @copydoc EditorViewportRendererOpenGL::RequestedExtent */
    EditorViewportExtent EditorViewportRendererOpenGL::RequestedExtent() const noexcept {
        const EditorViewportExtent allocatedExtent = resources_.AllocatedExtent();
        return requestedExtent_.IsValid() && allocatedExtent.IsValid() ? allocatedExtent : requestedExtent_;
    }

    /** @copydoc EditorViewportRendererOpenGL::ClipDepthRange */
    Math::ClipDepthRange EditorViewportRendererOpenGL::ClipDepthRange() const noexcept {
        return Math::ClipDepthRange::NegativeOneToOne;
    }

    /** @copydoc IEditorViewportRenderer::ExecutePreparedFrame */
    Result<void> EditorViewportRendererOpenGL::ExecutePreparedFrame(Render::RenderFrameScope &frame,
                                                                    const std::span<const Render::RenderPassDescriptor> passes) {
        auto result = frame.Execute(passes);
        const auto target = resources_.Target();
        const auto extent = resources_.AllocatedExtent();
        const bool rendered = imageCapture_.NeedsFramebuffer() && requestedExtent_.IsValid() && result.HasValue() &&
                              std::ranges::any_of(passes, [target](const auto &pass) {
            return pass.staticMesh && pass.staticMesh->target == target;
        });
        std::optional<std::uint32_t> framebuffer;
        bool failed = result.HasError();
        if (rendered) {
            const auto native = OpenGLViewportResourceBridge::ResolveFramebuffer(*frontend_, target);
            if (native.HasValue())
                framebuffer = native.Value();
            else
                failed = true;
        }
        imageCapture_.Advance(target, extent.width, extent.height, failed, framebuffer);
        return result;
    }

}  // namespace Horo::Editor
