#include "TerrainViewportTool.h"
#include "Horo/Editor/EditorUiComponents.h"
#include <cmath>
#include <limits>

namespace Horo::Editor {
    namespace {
        bool SameDocument(const TerrainDocumentView &a, const TerrainDocumentView &b) {
            return a.workspace == b.workspace && a.instance == b.instance && a.fence.session == b.fence.session &&
                a.fence.dataset == b.fence.dataset && a.fence.expectedRevision == b.fence.expectedRevision &&
                a.fence.capabilityRevision == b.fence.capabilityRevision;
        }
        bool SamePose(const EditorViewportCamera &a, const EditorViewportCamera &b) {
            return a.position == b.position && a.target == b.target && a.up == b.up && a.projection == b.projection;
        }
        bool SameLens(const EditorViewportCamera &a, const EditorViewportCamera &b) {
            return a.verticalFovRadians == b.verticalFovRadians && a.orthographicHeight == b.orthographicHeight &&
                a.nearPlane == b.nearPlane && a.farPlane == b.farPlane;
        }
        bool SameRectangle(ImVec2 origin, ImVec2 size, ImVec2 capturedOrigin, ImVec2 capturedSize) {
            return origin.x == capturedOrigin.x && origin.y == capturedOrigin.y && size.x == capturedSize.x && size.y == capturedSize.y;
        }
        bool IsFoliageMode(TerrainToolMode mode) {
            return mode == TerrainToolMode::FoliagePaint || mode == TerrainToolMode::FoliageErase || mode == TerrainToolMode::FoliageSelect;
        }
    }
    /** @copydoc TerrainViewportTool::Cancel */
    void TerrainViewportTool::Cancel() noexcept { active_ = false; invalid_ = false; count_ = 0; }
    /** @copydoc TerrainViewportTool::MatchesCamera */
    bool TerrainViewportTool::MatchesCamera(const EditorViewportCamera &camera) const {
        return SamePose(camera, camera_) && SameLens(camera, camera_);
    }
    /** @copydoc TerrainViewportTool::Intent */
    TerrainToolCommand TerrainViewportTool::Intent(const Frame &frame, TerrainToolAction action) const {
        TerrainToolCommand intent; intent.action = action; intent.document = frame.source.View(); intent.mode = mode_;
        intent.cookEpoch = frame.cookEpoch;
        intent.brush = brush_; intent.paint = paint_; intent.paint.radiusMeters = brush_.radiusMeters;
        if (mode_ == TerrainToolMode::CarveHole) intent.paint.mode = TerrainPaintMode::CarveHole;
        if (mode_ == TerrainToolMode::RestoreHole) intent.paint.mode = TerrainPaintMode::RestoreHole;
        intent.paintFence.document = intent.document.fence;
        if (const auto *binding = frame.source.BorrowLayerBinding()) {
            intent.paintFence.layerSet = binding->id; intent.paintFence.layerRevision = binding->revision;
            intent.paintFence.layerDigest = binding->digest;
        }
        intent.foliage = foliage_; intent.foliage.radiusMeters = brush_.radiusMeters;
        intent.foliage.mode = mode_ == TerrainToolMode::FoliageErase ? FoliagePaintMode::Erase : FoliagePaintMode::Paint;
        if (frame.selection)
            if (const auto *active = frame.selection->palette.Active()) intent.foliageType = active->source;
        return intent;
    }
    /** @copydoc TerrainViewportTool::MatchesResources */
    bool TerrainViewportTool::MatchesResources(const Frame &frame) const {
        const auto current = Intent(frame, TerrainToolAction::Stroke);
        if (mode_ <= TerrainToolMode::RestoreHole)
            return current.paintFence.layerSet == capturedPaint_.layerSet && current.paintFence.layerRevision == capturedPaint_.layerRevision &&
                current.paintFence.layerDigest == capturedPaint_.layerDigest;
        if (mode_ != TerrainToolMode::FoliagePaint && foliage_.allTypes) return true;
        return current.foliageType == capturedType_;
    }
    /** @copydoc TerrainViewportTool::Matches */
    bool TerrainViewportTool::Matches(const Frame &frame, const TerrainDocumentView &document) const {
        if (!SameDocument(captured_, document)) return false;
        if (!SameRectangle(frame.origin, frame.size, origin_, extent_)) return false;
        return frame.depth == depth_ && frame.datasetToViewport == placement_ && MatchesCamera(frame.camera) && MatchesResources(frame);
    }
    /** @copydoc TerrainViewportTool::ModeAvailable */
    bool TerrainViewportTool::ModeAvailable(const Frame &frame) const {
        const auto *source = frame.source.BorrowSource();
        if (!source) return false;
        switch (mode_) {
            case TerrainToolMode::Sculpt: return true;
            case TerrainToolMode::Weights: return source->HasWeights() && frame.source.BorrowLayerBinding() && paint_.targetCount > 0;
            case TerrainToolMode::CarveHole:
            case TerrainToolMode::RestoreHole: return source->HasHoles();
            default: return IsFoliageMode(mode_) && FoliageAvailable(frame);
        }
    }
    /** @copydoc TerrainViewportTool::FoliageAvailable */
    bool TerrainViewportTool::FoliageAvailable(const Frame &frame) const {
        if (!frame.source.View().containerSource || !frame.selection) return false;
        const bool active = frame.selection->palette.Active() != nullptr;
        if (mode_ == TerrainToolMode::FoliagePaint) return active;
        return foliage_.allTypes || active;
    }
    /** @copydoc TerrainViewportTool::ValidBrush */
    bool TerrainViewportTool::ValidBrush() const {
        if (!std::isfinite(brush_.radiusMeters) || brush_.radiusMeters <= 0) return false;
        if (mode_ == TerrainToolMode::Sculpt)
            return std::isfinite(brush_.amountMeters) && brush_.amountMeters >= 0 && std::isfinite(brush_.blend) && brush_.blend >= 0 && brush_.blend <= 1;
        return true; // The canonical producer admits mode-specific fields before submitting a transaction.
    }
    /** @copydoc TerrainViewportTool::InputSurface */
    bool TerrainViewportTool::InputSurface(const Frame &frame, bool controls) {
        if (controls || !frame.eligible || !ModeAvailable(frame)) return false;
        const auto cursor = ImGui::GetCursorScreenPos(); ImGui::SetCursorScreenPos(frame.origin);
        ImGui::InvisibleButton("##terrain_surface", frame.size, ImGuiButtonFlags_MouseButtonLeft);
        const bool hovered = ImGui::IsItemHovered(); ImGui::SetCursorScreenPos(cursor); return hovered;
    }
    /** @copydoc TerrainViewportTool::Pick */
    std::optional<TerrainSurfaceHit> TerrainViewportTool::Pick(const Frame &frame, const Terrain::TerrainCanonicalSource &source,
                                                              std::optional<TerrainToolCommand> &command) {
        const auto mouse = ImGui::GetMousePos();
        auto ray = BuildEditorViewportRay(frame.camera, (mouse.x - frame.origin.x) / frame.size.x,
            (mouse.y - frame.origin.y) / frame.size.y, frame.size.x / frame.size.y, frame.depth);
        if (ray.HasError()) { Cancel(); return {}; }
        const auto mode = mode_ == TerrainToolMode::RestoreHole ? TerrainSurfacePickMode::UnderlyingHeightfield : TerrainSurfacePickMode::VisibleSurface;
        auto picked = PickTerrainSurface(source, frame.datasetToViewport, ray.Value(), 4096, {}, mode);
        if (picked.HasError()) {
            if (active_ || ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                command = Intent(frame, TerrainToolAction::ReportFailure); command->failure = picked.ErrorValue();
            }
            Cancel(); return {};
        }
        if (!picked.Value()) { Cancel(); return {}; }
        return picked.Value();
    }
    /** @copydoc TerrainViewportTool::BeginGesture */
    void TerrainViewportTool::BeginGesture(const Frame &frame, const TerrainSurfaceHit &hit) {
        Cancel();
        if (brush_.noiseSeed == std::numeric_limits<std::uint64_t>::max()) return;
        ++brush_.noiseSeed; brush_.flattenMeters = hit.heightMeters;
        const auto intent = Intent(frame, TerrainToolAction::Stroke);
        captured_ = intent.document; capturedPaint_ = intent.paintFence; capturedType_ = intent.foliageType;
        camera_ = frame.camera; placement_ = frame.datasetToViewport; depth_ = frame.depth;
        origin_ = frame.origin; extent_ = frame.size; active_ = true; stamps_[count_++] = hit.stamp;
    }
    /** @copydoc TerrainViewportTool::ExtendGesture */
    void TerrainViewportTool::ExtendGesture(const Terrain::TerrainCanonicalSource &source, const TerrainSurfaceHit &hit) {
        const auto &last = stamps_[count_ - 1];
        const auto moved = std::hypot((last.sampleX - hit.stamp.sampleX) * source.coordinates.spacingX,
                                     (last.sampleZ - hit.stamp.sampleZ) * source.coordinates.spacingZ);
        if (moved < brush_.radiusMeters * .2) return;
        if (count_ == stamps_.size()) invalid_ = true;
        else stamps_[count_++] = hit.stamp;
    }
    /** @copydoc TerrainViewportTool::FinishGesture */
    bool TerrainViewportTool::FinishGesture(const Frame &frame, std::optional<TerrainToolCommand> &command) {
        if (!active_ || !ImGui::IsMouseReleased(ImGuiMouseButton_Left)) return false;
        command = Intent(frame, invalid_ ? TerrainToolAction::ReportFailure : TerrainToolAction::Stroke);
        command->document = captured_; command->paintFence = capturedPaint_; command->foliageType = capturedType_;
        command->stamps = stamps_; command->stampCount = count_;
        if (invalid_) command->failure = MakeError(TerrainEditErrors::LimitExceeded);
        Cancel(); return true;
    }
}
