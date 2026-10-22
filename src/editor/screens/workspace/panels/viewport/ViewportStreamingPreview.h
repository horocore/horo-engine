#pragma once

#include "Horo/Math/SceneMath.h"

#include <imgui.h>

namespace Horo::Editor {
    struct EditorWorkspaceViewModel;
    struct EditorGuiContext;
    /** @brief Draws bounded immutable cell evidence only against the exact active-world view; never queries a runtime owner. */
    void DrawViewportStreamingPreview(ImDrawList &drawList, ImVec2 origin, ImVec2 extent, const EditorWorkspaceViewModel &view,
                                      const EditorGuiContext &context, Math::ClipDepthRange depthRange);
}  // namespace Horo::Editor
