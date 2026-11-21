#include "EditorViewportVulkanCanvasProjection.h"

#include "editor/renderer/EditorRendererErrors.h"

namespace Horo::Editor {
    /** @copydoc FlipVulkanCanvasClipY */
    Math::Mat4 FlipVulkanCanvasClipY(Math::Mat4 clip) noexcept {
        for (std::size_t column = 0; column < 4; ++column)
            clip.values[column * 4 + 1] *= -1.0F;
        return clip;
    }

    /** @copydoc BuildVulkanEditorCanvasMvp */
    Result<Math::Mat4> BuildVulkanEditorCanvasMvp(const Render::UiRenderCompositionSpace space,
                                                  const std::optional<Math::Mat4> &worldFromCanvas, const Render::RenderCameraView &camera,
                                                  const Render::FramebufferExtent extent) {
        using enum Render::UiRenderCompositionSpace;
        using Outcome = Result<Math::Mat4>;
        if (!extent.IsValid() || !camera.IsValid() || (space != Camera && space != World) ||
            (space == World) != worldFromCanvas.has_value())
            return Outcome::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        if (space == Camera) {
            auto clip = Math::Mat4::Identity();
            clip.values[0] = 2.0F;
            clip.values[5] = -2.0F;
            return Outcome::Success(clip);
        }
        if (Math::TryInverseAffine(*worldFromCanvas).HasError())
            return Outcome::Failure(MakeError(RendererErrors::ViewportInvalidScene));
        const auto aspect = static_cast<float>(extent.width) / static_cast<float>(extent.height);
        auto clip = BuildRenderMvp(camera, *worldFromCanvas, aspect, Math::ClipDepthRange::ZeroToOne);
        if (clip.HasError())
            return clip;
        return Outcome::Success(FlipVulkanCanvasClipY(clip.Value()));
    }
}  // namespace Horo::Editor
