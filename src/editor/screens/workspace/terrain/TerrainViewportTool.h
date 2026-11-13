#pragma once
/** @file TerrainViewportTool.h @brief Private transient terrain brush overlay; all mutation is typed workspace intent. */
#include "Horo/Editor/EditorGuiContext.h"
#include "TerrainDocumentHost.h"
#include "TerrainToolSelection.h"
#include "TerrainViewportPicking.h"
#include "editor/project_model/EditorViewportCamera.h"

#include <array>
#include <imgui.h>

namespace Horo::Application {
    struct TerrainAuthoringCookCompletion;
}

namespace Horo::Editor {
    /** @brief Per-viewport transient brush/gesture state. Replacement, camera changes and focus loss discard its stroke. */
    class TerrainViewportTool final {
    public:
        struct Frame final {
            ImDrawList &draw;
            ImVec2 origin;
            ImVec2 size;
            const EditorViewportCamera &camera;
            Math::ClipDepthRange depth;
            Math::Transform datasetToViewport;
            const TerrainDocumentHost &source;
            bool eligible{};
            const TerrainToolSelection *selection{};
            bool cookAvailable{};
            bool cookRunning{};
            bool cookCurrent{};
            bool assetCreationAvailable{};
            std::uint64_t cookEpoch{};
            const Application::TerrainAuthoringCookCompletion *cookOutcome{};
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
        void ModeControls(const Frame &frame, const EditorGuiContext &context);
        void SculptControls(const EditorGuiContext &context);
        void PaintControls(const Frame &frame, const EditorGuiContext &context);
        void FoliageControls(const Frame &frame, const EditorGuiContext &context);
        void SourceControls(const Frame &frame, std::optional<TerrainToolCommand> &command, const EditorGuiContext &context);
        void LayerControls(const Terrain::TerrainAuthoringLayerBinding &binding, std::size_t count, const EditorGuiContext &context);
        void LayerRow(Terrain::TerrainLayerId layer, std::size_t index, const EditorGuiContext &context);
        void ActionControls(const Frame &frame, std::optional<TerrainToolCommand> &command, const EditorGuiContext &context);
        void CookStatus(const Frame &frame, const EditorGuiContext &context);
        void CookControls(const Frame &frame, std::optional<TerrainToolCommand> &command, const EditorGuiContext &context);
        void Surface(const Frame &frame, const Terrain::TerrainCanonicalSource &source) const;
        void Weights(const Frame &frame, const Terrain::TerrainCanonicalSource &source) const;
        void Brush(const Frame &frame, const Terrain::TerrainCanonicalSource &source, const TerrainSurfaceHit &hit) const;
        void BrushGrid(const Frame &frame, const Terrain::TerrainCanonicalSource &source, const TerrainSurfaceHit &hit) const;
        void Foliage(const Frame &frame, const Terrain::TerrainCanonicalSource &source) const;
        [[nodiscard]] std::optional<TerrainSurfaceHit> Pick(const Frame &frame, const Terrain::TerrainCanonicalSource &source,
                                                            std::optional<TerrainToolCommand> &command);
        [[nodiscard]] bool InputSurface(const Frame &frame, bool controls);
        [[nodiscard]] bool PrepareFrame(const Frame &frame, const Terrain::TerrainCanonicalSource &source);
        bool ProcessInput(const Frame &frame, const Terrain::TerrainCanonicalSource &source, bool controls,
                          std::optional<TerrainToolCommand> &command);
        [[nodiscard]] bool ModeAvailable(const Frame &frame) const;
        [[nodiscard]] bool FoliageAvailable(const Frame &frame) const;
        [[nodiscard]] bool ValidBrush() const;
        [[nodiscard]] TerrainToolCommand Intent(const Frame &frame, TerrainToolAction action) const;
        void BeginGesture(const Frame &frame, const TerrainSurfaceHit &hit);
        void ExtendGesture(const Terrain::TerrainCanonicalSource &source, const TerrainSurfaceHit &hit);
        bool FinishGesture(const Frame &frame, std::optional<TerrainToolCommand> &command);
        bool Matches(const Frame &frame, const TerrainDocumentView &document) const;
        bool MatchesCamera(const EditorViewportCamera &camera) const;
        bool MatchesResources(const Frame &frame) const;
        TerrainSculptBrush brush_;
        TerrainToolMode mode_{TerrainToolMode::Sculpt};
        TerrainPaintBrush paint_;
        FoliagePaintBrush foliage_;
        TerrainPaintFence capturedPaint_;
        std::optional<Terrain::FoliageAuthoringDefinitionReference> capturedType_;
        std::array<std::string, Terrain::TerrainDescriptorHardLimits::LayersPerTile> layerLabels_;
        Sha256Digest layerLabelsDigest_;
        std::string layerLabelsLocale_;
        int conversionLayer_{};
        std::array<TerrainSculptStamp, 64> stamps_{};
        std::size_t count_{};
        TerrainDocumentView captured_;
        EditorViewportCamera camera_;
        Math::Transform placement_;
        ImVec2 origin_{};
        ImVec2 extent_{};
        Math::ClipDepthRange depth_{};
        bool active_{};
        bool invalid_{};
    };
}  // namespace Horo::Editor
