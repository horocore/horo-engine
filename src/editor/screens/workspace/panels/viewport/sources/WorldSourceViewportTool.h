#pragma once

#include "Horo/Editor/EditorGuiContext.h"
#include "editor/screens/workspace/EditorWorkspaceViewModel.h"

#include <imgui.h>

namespace Horo::Editor {
    /** @brief Transient viewport gesture/popup only; publication and history belong to the composed world owner. */
    class WorldSourceViewportTool final {
    public:
        /** @brief Borrowed frame inputs; never retained by the gesture or source authority. */
        struct Frame {
            ImDrawList &draw;
            ImVec2 origin;
            ImVec2 size;
            const EditorWorkspaceViewModel &view;
            Math::ClipDepthRange depth;
            bool eligible{};
        };

        /** @brief Draws the actual viewport adapter; frame borrows end with this synchronous call. */
        bool Draw(const Frame &frame, EditorWorkspaceViewCommandData &command, const EditorGuiContext &context);
        /** @brief Cancels previews without mutating the document on detach/focus loss. */
        void Cancel();

    private:
        /** @brief Builds a gesture only after ray/plane admission succeeds. */
        void BeginDrag(const Frame &frame, const PartitionedWorldSource &source, std::size_t handle, const Math::WorldCoordinate64 &global,
                       Math::Vec3 local);
        /** @brief Draws exact source control points and admits a single selected-source gesture. */
        void DrawSourceHandles(const Frame &frame, const PartitionedWorldSource &source);
        /** @brief Updates a detached preview; invalid release never publishes the last valid partial draft. */
        bool UpdateDrag(const Frame &frame, EditorWorkspaceViewCommandData &command);
        /** @brief Uses shared controls to edit only the detached source property draft. */
        void DrawSourceProperties(const EditorGuiContext &context);
        /** @brief Selects a stable source into the detached property draft. */
        void DrawSourceList(const EditorWorkspaceViewModel &view);
        /** @brief Rejects camera, surface or depth changes before reprojecting transient pointer input. */
        [[nodiscard]] bool MatchesProjection(const EditorViewportCamera &camera, ImVec2 origin, ImVec2 extent,
                                             Math::ClipDepthRange depth) const;
        void DrawPopup(const EditorWorkspaceViewModel &view, EditorWorkspaceViewCommandData &command, const EditorGuiContext &context);
        bool DrawHandles(ImDrawList &draw, ImVec2 origin, ImVec2 size, const EditorWorkspaceViewModel &view,
                         EditorWorkspaceViewCommandData &command, Math::ClipDepthRange depth, bool eligible);
        std::optional<PartitionedWorldSource> draft_;
        std::optional<PartitionedWorldSource> dragOriginal_;
        std::optional<Error> previewError_;
        WorldDocumentFence fence_;

        /** @brief Read-only projection capture; camera/origin authority remains in the host. */
        struct ProjectionCapture {
            EditorViewportCamera camera;
            ImVec2 origin;
            ImVec2 extent;
            Math::ClipDepthRange depth;
        };

        std::optional<ProjectionCapture> projection_;
        Math::WorldCoordinate64 origin_;
        std::uint64_t viewRevision_{};
        std::optional<std::size_t> dragged_;
        Math::WorldCoordinate64 dragStart_;
        Math::Vec3 dragPlanePoint_;
        Math::Vec3 dragIntersection_;
        Math::Vec3 dragNormal_;
    };
}  // namespace Horo::Editor
