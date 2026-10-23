/** @file EditorViewportPreparedFrameOpenGL.cpp
 * @brief Prepared-frame context queries and canonical execution for the OpenGL viewport peer. */
#include "EditorViewportRendererOpenGL.h"
#include "Horo/Runtime/Render/RenderFrontend.h"

namespace Horo::Editor {
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
        return frame.Execute(passes);
    }

}  // namespace Horo::Editor
