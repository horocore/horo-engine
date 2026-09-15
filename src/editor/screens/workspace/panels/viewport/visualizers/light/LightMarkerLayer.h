#pragma once

#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <imgui.h>
#include <optional>
#include <span>

namespace Horo::Editor {
    struct ViewportLightMarkerContext {
        ImDrawList &drawList;
        ImVec2 origin;
        float width;
        float height;
        const EditorViewportCamera &camera;
        Math::ClipDepthRange depthRange;
        bool acceptInput;
    };

    /**
     * @brief Draws constant-size viewport Light markers and resolves one clicked object.
     * @return Stable Light object identity when clicked, empty when no marker was clicked, or a typed projection failure.
     */
    [[nodiscard]] Result<std::optional<SceneObjectId>> DrawViewportLightMarkers(const ViewportLightMarkerContext &context,
                                                                                std::span<const ViewportLightPresentation> lights,
                                                                                std::optional<SceneObjectId> primarySelection);
}  // namespace Horo::Editor
