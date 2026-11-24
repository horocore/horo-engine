#include "TerrainViewportTool.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "TerrainViewportProjection.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Horo::Editor {
    namespace {
        /** @brief Project a source segment only when both actual source endpoints are present. */
        void Segment(const TerrainViewportTool::Frame &frame, const Terrain::TerrainCanonicalSource &source, TerrainSculptStamp a,
                     TerrainSculptStamp b, ImU32 color, TerrainSurfacePickMode mode) {
            const auto first = SampleTerrainPreview(source, a.sampleX, a.sampleZ, mode);
            const auto second = SampleTerrainPreview(source, b.sampleX, b.sampleZ, mode);
            if (!first || !second)
                return;
            const auto pa = ProjectTerrainPreview(frame, *first);
            const auto pb = ProjectTerrainPreview(frame, *second);
            if (pa && pb)
                frame.draw.AddLine(*pa, *pb, color, 1.F);
        }

        using ActiveWeightLayers = std::array<bool, 16>;

        /** @brief Resolve the transient selection to persisted semantic channel identities once per overlay. */
        ActiveWeightLayers SelectedWeightLayers(const Terrain::TerrainAuthoringLayerBinding &binding, const TerrainPaintBrush &brush,
                                                std::uint8_t layerCount) {
            ActiveWeightLayers active{};
            for (std::size_t layer = 0; layer < layerCount; ++layer)
                for (std::size_t target = 0; target < brush.targetCount; ++target)
                    active[layer] = active[layer] || binding.order[layer] == brush.targets[target].layer;
            return active;
        }

        /** @brief Show the actual normalized selected-channel weight at one visible canonical source node. */
        void WeightPoint(const TerrainViewportTool::Frame &frame, const Terrain::TerrainCanonicalSource &source,
                         const ActiveWeightLayers &active, std::uint32_t x, std::uint32_t z) {
            const auto point = SampleTerrainPreview(source, x, z);
            if (!point)
                return;
            const auto screen = ProjectTerrainPreview(frame, *point);
            if (!screen)
                return;
            std::uint32_t weight{};
            for (std::size_t layer = 0; layer < source.layerCount; ++layer)
                if (active[layer])
                    weight += source.weights[(std::size_t(z) * source.width + x) * source.layerCount + layer];
            auto color = Theme::Accent();
            color.w = 0.15F + 0.85F * float(weight) / 65535.F;
            frame.draw.AddCircleFilled(*screen, Ui::ScaledLayoutValue(3), Theme::U32(color), 6);
        }

        /** @brief Draw one authored placement as source feedback, never as proof of runtime render activation. */
        void Placement(const TerrainViewportTool::Frame &frame, const Terrain::TerrainCanonicalSource &source,
                       const TerrainAuthoredPlacement &placement, bool selected) {
            auto point = SampleTerrainPreview(source, placement.sampleX, placement.sampleZ);
            if (!point)
                return;
            point->y += placement.offsetMeters;
            const auto screen = ProjectTerrainPreview(frame, *point);
            if (!screen)
                return;
            const float radius = Ui::ScaledLayoutValue(selected ? 6 : 4);
            const auto color = Theme::U32(selected ? Theme::Accent() : Theme::Text());
            frame.draw.AddCircle(*screen, radius, color, 8, selected ? 2.F : 1.F);
            frame.draw.AddLine({screen->x - radius, screen->y}, {screen->x + radius, screen->y}, color);
        }
    }  // namespace

    /** @copydoc TerrainViewportTool::Surface */
    void TerrainViewportTool::Surface(const Frame &frame, const Terrain::TerrainCanonicalSource &source) const {
        constexpr std::uint32_t Divisions = 24;
        for (std::uint32_t z = 0; z <= Divisions; ++z)
            for (std::uint32_t x = 0; x <= Divisions; ++x) {
                const TerrainSculptStamp a{double(x) * (source.width - 1) / Divisions, double(z) * (source.height - 1) / Divisions};
                for (const auto &[nx, nz] : std::array{std::pair{x + 1, z}, std::pair{x, z + 1}}) {
                    if (nx > Divisions || nz > Divisions)
                        continue;
                    Segment(frame, source, a, {double(nx) * (source.width - 1) / Divisions, double(nz) * (source.height - 1) / Divisions},
                            Theme::U32(Theme::Muted()), TerrainSurfacePickMode::VisibleSurface);
                }
            }
    }

    /** @brief Display actual normalized active-layer weights with bounded source samples, without inventing material rendering. */
    void TerrainViewportTool::Weights(const Frame &frame, const Terrain::TerrainCanonicalSource &source) const {
        if (mode_ != TerrainToolMode::Weights || source.layerCount == 0 || source.layerCount > 16 || source.width <= 1 ||
            source.height <= 1)
            return;
        const auto *binding = frame.source.BorrowLayerBinding();
        if (!binding || source.weights.size() != std::uint64_t{source.width} * source.height * source.layerCount)
            return;
        const auto active = SelectedWeightLayers(*binding, paint_, source.layerCount);
        const auto divisionsX = std::min<std::uint32_t>(24, source.width - 1);
        const auto divisionsZ = std::min<std::uint32_t>(24, source.height - 1);
        for (std::uint32_t z = 0; z <= divisionsZ; ++z)
            for (std::uint32_t x = 0; x <= divisionsX; ++x)
                WeightPoint(frame, source, active, x * (source.width - 1) / divisionsX, z * (source.height - 1) / divisionsZ);
    }

    /** @copydoc TerrainViewportTool::BrushGrid */
    void TerrainViewportTool::BrushGrid(const Frame &frame, const Terrain::TerrainCanonicalSource &source,
                                        const TerrainSurfaceHit &hit) const {
        constexpr unsigned Divisions = 12;
        const double radiusX = brush_.radiusMeters / source.coordinates.spacingX;
        const double radiusZ = brush_.radiusMeters / source.coordinates.spacingZ;
        const auto mode =
            mode_ == TerrainToolMode::RestoreHole ? TerrainSurfacePickMode::UnderlyingHeightfield : TerrainSurfacePickMode::VisibleSurface;
        for (unsigned z = 0; z <= Divisions; ++z)
            for (unsigned x = 0; x <= Divisions; ++x) {
                const TerrainSculptStamp a{hit.stamp.sampleX + radiusX * (2. * x / Divisions - 1.),
                                           hit.stamp.sampleZ + radiusZ * (2. * z / Divisions - 1.)};
                for (const auto &[nx, nz] : std::array{std::pair{x + 1, z}, std::pair{x, z + 1}}) {
                    if (nx > Divisions || nz > Divisions)
                        continue;
                    Segment(frame, source, a,
                            {hit.stamp.sampleX + radiusX * (2. * nx / Divisions - 1.),
                             hit.stamp.sampleZ + radiusZ * (2. * nz / Divisions - 1.)},
                            Theme::U32(Theme::Accent()), mode);
                }
            }
    }

    /** @copydoc TerrainViewportTool::Brush */
    void TerrainViewportTool::Brush(const Frame &frame, const Terrain::TerrainCanonicalSource &source, const TerrainSurfaceHit &hit) const {
        BrushGrid(frame, source, hit);
        const auto mode =
            mode_ == TerrainToolMode::RestoreHole ? TerrainSurfacePickMode::UnderlyingHeightfield : TerrainSurfacePickMode::VisibleSurface;
        std::optional<ImVec2> prior;
        for (unsigned i = 0; i <= 64; ++i) {
            const double angle = double(i) * 2 * std::numbers::pi / 64;
            const auto point =
                SampleTerrainPreview(source, hit.stamp.sampleX + std::cos(angle) * brush_.radiusMeters / source.coordinates.spacingX,
                                     hit.stamp.sampleZ + std::sin(angle) * brush_.radiusMeters / source.coordinates.spacingZ, mode);
            const auto current = point ? ProjectTerrainPreview(frame, *point) : std::nullopt;
            if (prior && current)
                frame.draw.AddLine(*prior, *current, Theme::U32(invalid_ ? Theme::Err() : Theme::Accent()), 2.F);
            prior = current;
        }
    }

    /** @copydoc TerrainViewportTool::Foliage */
    void TerrainViewportTool::Foliage(const Frame &frame, const Terrain::TerrainCanonicalSource &source) const {
        const auto *document = frame.source.BorrowDocument();
        if (!document)
            return;
        const auto selected = frame.selection ? frame.selection->selection.Ids() : std::span<const Terrain::FoliageInstanceId>{};
        // Bound overlay work separately from canonical source capacity. Selected points are always given the first draw budget.
        std::size_t drawn{};
        for (auto id : selected) {
            if (drawn == 512)
                break;
            const auto found = document->Placements().find(id);
            if (found != document->Placements().end()) {
                Placement(frame, source, found->second, true);
                ++drawn;
            }
        }
        for (const auto &[id, placement] : document->Placements()) {
            if (drawn == 512)
                break;
            if (std::ranges::binary_search(selected, id))
                continue;
            Placement(frame, source, placement, false);
            ++drawn;
        }
    }

    /** @copydoc TerrainViewportTool::PrepareFrame */
    bool TerrainViewportTool::PrepareFrame(const Frame &frame, const Terrain::TerrainCanonicalSource &source) {
        if (frame.size.x <= 0 || frame.size.y <= 0) {
            Cancel();
            return false;
        }
        if (active_ && (!frame.eligible || !Matches(frame, frame.source.View())))
            Cancel();
        Surface(frame, source);
        Weights(frame, source);
        Foliage(frame, source);
        if (ImGui::GetIO().AppFocusLost) {
            Cancel();
            return false;
        }
        return true;
    }

    /** @copydoc TerrainViewportTool::ProcessInput */
    bool TerrainViewportTool::ProcessInput(const Frame &frame, const Terrain::TerrainCanonicalSource &source, bool controls,
                                           std::optional<TerrainToolCommand> &command) {
        if (!InputSurface(frame, controls) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            Cancel();
            return controls;
        }
        const auto hit = Pick(frame, source, command);
        if (!hit || !ValidBrush()) {
            Cancel();
            return false;
        }
        Brush(frame, source, *hit);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            BeginGesture(frame, *hit);
        if (active_ && ImGui::IsMouseDown(ImGuiMouseButton_Left))
            ExtendGesture(source, *hit);
        return FinishGesture(frame, command) || active_;
    }

    /** @copydoc TerrainViewportTool::Draw */
    bool TerrainViewportTool::Draw(const Frame &frame, std::optional<TerrainToolCommand> &command, const EditorGuiContext &context) {
        const auto *source = frame.source.BorrowSource();
        if (!source) {
            Cancel();
            return false;
        }
        if (!PrepareFrame(frame, *source))
            return false;
        if (const auto *document = frame.source.BorrowDocument(); document && document->Placements().size() > 512) {
            const auto cursor = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos({frame.origin.x + Ui::ScaledLayoutValue(8), frame.origin.y + Ui::ScaledLayoutValue(80)});
            Ui::WrappedText(std::max(1.F, frame.size.x - Ui::ScaledLayoutValue(16)),
                            context.localization.Get("editor", "terrain.tools.overlay_limit"), Theme::Muted(), context.theme.fonts);
            ImGui::SetCursorScreenPos(cursor);
        }
        return ProcessInput(frame, *source, Controls(frame, command, context), command);
    }
}  // namespace Horo::Editor
