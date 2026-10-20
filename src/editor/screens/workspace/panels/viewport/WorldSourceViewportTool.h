#pragma once

#include "Horo/Editor/EditorGuiContext.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

namespace Horo::Editor {
    /** @brief Transient viewport gesture/popup only; publication and history belong to the composed world owner. */
    class WorldSourceViewportTool final {
    public:
        /** @brief Draws the actual viewport adapter and returns whether scene input must yield to it. */
        bool Draw(ImDrawList &draw, ImVec2 origin, ImVec2 size, const EditorWorkspaceViewModel &view,
                  EditorWorkspaceViewCommandData &command, const EditorGuiContext &context, Math::ClipDepthRange depth, bool eligible);
        /** @brief Cancels previews without mutating the document on detach/focus loss. */
        void Cancel();

    private:
        void DrawPopup(const EditorWorkspaceViewModel &view, EditorWorkspaceViewCommandData &command, const EditorGuiContext &context);
        bool DrawHandles(ImDrawList &draw, ImVec2 origin, ImVec2 size, const EditorWorkspaceViewModel &view,
                         EditorWorkspaceViewCommandData &command, Math::ClipDepthRange depth, bool eligible);
        std::optional<PartitionedWorldSource> draft_;
        WorldDocumentFence fence_;
        Math::WorldCoordinate64 origin_;
        std::uint64_t viewRevision_{};
        std::optional<std::size_t> dragged_;
        Math::WorldCoordinate64 dragStart_;
        Math::Vec3 dragPlanePoint_;
        Math::Vec3 dragIntersection_;
        Math::Vec3 dragNormal_;
    };
}  // namespace Horo::Editor
