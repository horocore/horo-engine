#pragma once
/** @file TerrainViewportTool.h @brief Private transient terrain brush overlay; all mutation is typed workspace intent. */
#include "Horo/Editor/EditorGuiContext.h"
#include "TerrainDocumentHost.h"
#include "TerrainViewportPicking.h"
#include "editor/project_model/EditorViewportCamera.h"

#include <array>
#include <imgui.h>

namespace Horo::Editor {
    /** @brief Per-viewport transient brush/gesture state. Replacement, camera changes and focus loss discard its stroke. */
    class TerrainViewportTool final {
    public:
        struct Frame final {
            ImDrawList &draw;
            ImVec2 origin, size;
            const EditorViewportCamera &camera;
            Math::ClipDepthRange depth;
            Math::Transform datasetToViewport;
            const TerrainDocumentHost &source;
            bool eligible{};
        };

        /** @brief Draw bounded canonical surface/brush feedback and emit at most one detached typed intent.
         * @param frame Synchronous const owner/projection borrows. @param command Output intent, if any.
         * @param context Existing localization/theme controls. @return Whether terrain owns pointer interaction.
         */
        bool Draw(const Frame &frame, std::optional<TerrainToolCommand> &command, const EditorGuiContext &context);
        /** @brief Forget uncommitted transient input; authored source/history remain unchanged. */
        void Cancel() noexcept;

    private:
        bool Controls(const Frame &frame, std::optional<TerrainToolCommand> &command, const EditorGuiContext &context);
        void Surface(const Frame &frame, const Terrain::TerrainCanonicalSource &source) const;
        void Brush(const Frame &frame, const Terrain::TerrainCanonicalSource &source, const TerrainSurfaceHit &hit) const;
        bool Matches(const Frame &frame, const TerrainDocumentView &document) const;
        TerrainSculptBrush brush_;
        std::array<TerrainSculptStamp, 64> stamps_{};
        std::size_t count_{};
        TerrainDocumentView captured_;
        EditorViewportCamera camera_;
        Math::Transform placement_;
        ImVec2 origin_{}, extent_{};
        Math::ClipDepthRange depth_{};
        bool active_{}, invalid_{};
    };
}  // namespace Horo::Editor
