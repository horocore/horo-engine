#pragma once
/** @file EditorViewportBaselineDrawParameters.h @brief Canonical bounded CPU-light selection captured into real raster uniforms. */
#include "EditorViewportForwardLighting.h"
#include "EditorViewportShaderParameters.h"

namespace Horo::Editor {
    /** @brief Copies the existing forward-light owner's exact draw selection and remapped shadow into shader parameters.
     * @param lighting Prepared canonical CPU forward owner. @param scene Complete immutable source scene.
     * @param instance Actual scene instance. @param shadow Actual scene shadow frame, if admitted.
     * @param target Explicit shader coordinate convention selected by the host. @param destination Owned parameter storage.
     * @return Original selection/shadow failure or successful copied values; no GPU culling/completion or scene mutation is claimed.
     * @details Owner-lane, fixed sixteen-light scratch, no retained input borrows or allocation. Matrices/style remain caller-owned.
     */
    [[nodiscard]] Result<void> PrepareEditorViewportBaselineDrawParameters(const EditorViewportForwardLighting &lighting,
                                                                           const Render::RenderSceneView &scene,
                                                                           const Render::RenderStaticMeshInstance &instance,
                                                                           const std::optional<Render::DirectionalShadowFrame> &shadow,
                                                                           Render::ShaderTargetBackend target,
                                                                           EditorViewportShaderParameters &destination);
}  // namespace Horo::Editor
