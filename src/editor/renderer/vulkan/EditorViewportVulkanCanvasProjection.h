#pragma once

/** @file EditorViewportVulkanCanvasProjection.h @brief Exact local canvas plane projection for the selected Vulkan viewport. */
#include "Horo/Runtime/Render/UiRenderComposition.h"
#include "editor/renderer/EditorViewportScene.h"

#include <optional>

namespace Horo::Editor {
    /** @brief Adapts upward clip Y to the material raster pass's positive Vulkan viewport height. */
    [[nodiscard]] Math::Mat4 FlipVulkanCanvasClipY(Math::Mat4 clip) noexcept;

    /** @brief Projects a centered unit XY sampled canvas with top-left UVs into the actual acquired Vulkan viewport.
     * @param space Selected scene composition role, World or Camera.
     * @param worldFromCanvas World placement in the camera's coordinate frame, with upper-left at (-0.5,+0.5,0) in quad space.
     * @param camera Exact selected viewport camera.
     * @param extent Actual viewport framebuffer extent used by the scene pass.
     * @return Finite clip-from-local transform or typed incompatible view/space/plane failure.
     * @details Camera projects the complete layout to view pixels. World uses the ordinary scene's zero-to-one projection and
     * equivalent Vulkan Y inversion. No placement is inferred from a camera name or a different view. */
    [[nodiscard]] Result<Math::Mat4> BuildVulkanEditorCanvasMvp(Render::UiRenderCompositionSpace space,
                                                                const std::optional<Math::Mat4> &worldFromCanvas,
                                                                const Render::RenderCameraView &camera, Render::FramebufferExtent extent);
}  // namespace Horo::Editor
