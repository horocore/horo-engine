#include "TerrainViewportTool.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/renderer/EditorViewportScene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace Horo::Editor {
    namespace {
        /** @brief Exact canonical first-LOD triangle interpolation; no plane substitute or height guess. */
        std::optional<Math::Vec3> Sample(const Terrain::TerrainCanonicalSource &source, double x, double z) {
            if (!std::isfinite(x) || !std::isfinite(z) || x < 0 || z < 0 || x > source.width - 1 || z > source.height - 1)
                return {};
            const auto ix = static_cast<std::uint32_t>(std::min(std::floor(x), double(source.width - 2)));
            const auto iz = static_cast<std::uint32_t>(std::min(std::floor(z), double(source.height - 2)));
            const double u = x - ix, v = z - iz;
            const auto index = std::size_t{iz} * source.width + ix;
            const double a = source.heightsMeters[index], b = source.heightsMeters[index + 1],
                         c = source.heightsMeters[index + source.width], d = source.heightsMeters[index + source.width + 1];
            const double height = u + v <= 1 ? a + (b - a) * u + (c - a) * v : d + (c - d) * (1 - u) + (b - d) * (1 - v);
            const Math::Vec3 point{static_cast<float>(x * source.coordinates.spacingX), static_cast<float>(height),
                                   static_cast<float>(z * source.coordinates.spacingZ)};
            return Math::IsFinite(point) ? std::optional{point} : std::nullopt;
        }

        /** @brief Project a real dataset sample through the captured asset-preview transform and camera. */
        std::optional<ImVec2> Project(const TerrainViewportTool::Frame &frame, const Math::Vec3 point) {
            auto matrix = frame.datasetToViewport.TryToMatrix();
            if (matrix.HasError())
                return {};
            auto world = Math::TryTransformPoint(matrix.Value(), point);
            if (world.HasError())
                return {};
            auto projected = ProjectEditorViewportPoint(frame.camera, world.Value(), frame.size.x / frame.size.y, frame.depth);
            if (projected.HasError() || !projected.Value())
                return {};
            return ImVec2{frame.origin.x + projected.Value()->viewportPosition.x * frame.size.x,
                          frame.origin.y + projected.Value()->viewportPosition.y * frame.size.y};
        }

        /** @brief Form one complete scalar revision fence without borrowing transient UI state. */
        TerrainToolCommand Intent(TerrainToolAction action, const TerrainDocumentHost &host) {
            TerrainToolCommand command;
            command.action = action;
            command.document = host.View();
            return command;
        }
    }  // namespace

    /** @copydoc TerrainViewportTool::Cancel */
    void TerrainViewportTool::Cancel() noexcept {
        active_ = false;
        invalid_ = false;
        count_ = 0;
    }

    /** @copydoc TerrainViewportTool::Matches */
    bool TerrainViewportTool::Matches(const Frame &frame, const TerrainDocumentView &document) const {
        return captured_.workspace == document.workspace && captured_.instance == document.instance &&
               captured_.fence.expectedRevision == document.fence.expectedRevision && captured_.fence.session == document.fence.session &&
               captured_.fence.capabilityRevision == document.fence.capabilityRevision &&
               captured_.fence.dataset == document.fence.dataset && frame.origin.x == origin_.x && frame.origin.y == origin_.y &&
               frame.size.x == extent_.x && frame.size.y == extent_.y && frame.depth == depth_ && frame.datasetToViewport == placement_ &&
               frame.camera.position == camera_.position && frame.camera.target == camera_.target && frame.camera.up == camera_.up &&
               frame.camera.projection == camera_.projection && frame.camera.verticalFovRadians == camera_.verticalFovRadians &&
               frame.camera.orthographicHeight == camera_.orthographicHeight && frame.camera.nearPlane == camera_.nearPlane &&
               frame.camera.farPlane == camera_.farPlane;
    }

    /** @copydoc TerrainViewportTool::Controls */
    bool TerrainViewportTool::Controls(const Frame &frame, std::optional<TerrainToolCommand> &command, const EditorGuiContext &context) {
        const auto label = [&](const char *key) {
            return context.localization.Get("editor", key).c_str();
        };
        const auto cursor = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos({frame.origin.x + Ui::ScaledLayoutValue(8), frame.origin.y + Ui::ScaledLayoutValue(44)});
        if (Ui::Button({.label = label("terrain.tools.title"),
                        .variant = Ui::ButtonVariant::Secondary,
                        .enabled = frame.eligible,
                        .componentSize = Ui::ComponentSize::Small})) {
            Cancel();
            ImGui::OpenPopup("##terrain_tools");
        }
        const bool buttonHovered = ImGui::IsItemHovered();
        ImGui::SetNextWindowSizeConstraints({std::max(1.0F, std::min(frame.size.x - 16.0F, Ui::ScaledLayoutValue(280))), 0},
                                            {std::max(1.0F, frame.size.x - 16.0F), std::max(1.0F, frame.size.y - 16.0F)});
        bool open = false;
        if (ImGui::BeginPopup("##terrain_tools")) {
            open = true;
            int mode = static_cast<int>(brush_.mode);
            const std::array choices{label("terrain.tools.raise"), label("terrain.tools.lower"), label("terrain.tools.smooth"),
                                     label("terrain.tools.flatten"), label("terrain.tools.noise")};
            if (Ui::DrawComboPropRow(label("terrain.tools.operation"), "##terrain_mode", mode, choices, context.theme.fonts))
                brush_.mode = static_cast<TerrainSculptMode>(mode);
            float radius = static_cast<float>(brush_.radiusMeters);
            if (Ui::DrawFloatPropRow(label("terrain.tools.radius"), "##terrain_radius", radius, context.theme.fonts).changed)
                brush_.radiusMeters = radius;
            static_cast<void>(
                Ui::DrawFloatPropRow(label("terrain.tools.amount"), "##terrain_amount", brush_.amountMeters, context.theme.fonts));
            static_cast<void>(Ui::DrawFloatPropRow(label("terrain.tools.blend"), "##terrain_blend", brush_.blend, context.theme.fonts));
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, context.localization.Get("editor", "terrain.tools.flatten_help"),
                            Theme::Muted(), context.theme.fonts);
            const auto view = frame.source.View();
            const std::array actions{std::pair{TerrainToolAction::Undo, "terrain.tools.undo"},
                                     std::pair{TerrainToolAction::Redo, "terrain.tools.redo"},
                                     std::pair{TerrainToolAction::Save, "terrain.tools.save"},
                                     std::pair{TerrainToolAction::Close, "terrain.tools.close"},
                                     std::pair{TerrainToolAction::Discard, "terrain.tools.discard"}};
            for (const auto &[action, key] : actions) {
                const bool enabled = frame.eligible && (action != TerrainToolAction::Undo || view.canUndo) &&
                                     (action != TerrainToolAction::Redo || view.canRedo) &&
                                     (action != TerrainToolAction::Save || view.dirty);
                if (Ui::Button({.label = label(key),
                                .variant = Ui::ButtonVariant::Secondary,
                                .enabled = enabled,
                                .style = {.width = Ui::StyleWidth::FillAvailable}})) {
                    command = Intent(action, frame.source);
                    Cancel();
                    ImGui::CloseCurrentPopup();
                }
            }
            Ui::WrappedText(ImGui::GetContentRegionAvail().x, context.localization.Get("editor", "terrain.tools.instructions"),
                            Theme::Muted(), context.theme.fonts);
            ImGui::EndPopup();
        }
        ImGui::SetCursorScreenPos(cursor);
        return open || buttonHovered;
    }

    /** @copydoc TerrainViewportTool::Surface */
    void TerrainViewportTool::Surface(const Frame &frame, const Terrain::TerrainCanonicalSource &source) const {
        constexpr std::uint32_t Divisions = 24;
        for (std::uint32_t z = 0; z <= Divisions; ++z)
            for (std::uint32_t x = 0; x <= Divisions; ++x) {
                const double sx = double(x) * (source.width - 1) / Divisions, sz = double(z) * (source.height - 1) / Divisions;
                const auto a = Sample(source, sx, sz);
                if (!a)
                    continue;
                const auto pa = Project(frame, *a);
                if (!pa)
                    continue;
                for (const auto &[nx, nz] : std::array{std::pair{x + 1, z}, std::pair{x, z + 1}}) {
                    if (nx > Divisions || nz > Divisions)
                        continue;
                    const auto b =
                        Sample(source, double(nx) * (source.width - 1) / Divisions, double(nz) * (source.height - 1) / Divisions);
                    const auto pb = b ? Project(frame, *b) : std::nullopt;
                    if (pb)
                        frame.draw.AddLine(*pa, *pb, Theme::U32(Theme::Muted()), 1.0F);
                }
            }
    }

    /** @copydoc TerrainViewportTool::Brush */
    void TerrainViewportTool::Brush(const Frame &frame, const Terrain::TerrainCanonicalSource &source, const TerrainSurfaceHit &hit) const {
        // A fixed local lattice exposes edited detail even when the whole-source overview is coarser than a small brush.
        constexpr unsigned Divisions = 12;
        const double radiusX = brush_.radiusMeters / source.coordinates.spacingX;
        const double radiusZ = brush_.radiusMeters / source.coordinates.spacingZ;
        for (unsigned z = 0; z <= Divisions; ++z) {
            for (unsigned x = 0; x <= Divisions; ++x) {
                const double sx = hit.stamp.sampleX + radiusX * (2.0 * x / Divisions - 1.0);
                const double sz = hit.stamp.sampleZ + radiusZ * (2.0 * z / Divisions - 1.0);
                const auto a = Sample(source, sx, sz);
                const auto pa = a ? Project(frame, *a) : std::nullopt;
                if (!pa)
                    continue;
                for (const auto &[nx, nz] : std::array{std::pair{x + 1, z}, std::pair{x, z + 1}}) {
                    if (nx > Divisions || nz > Divisions)
                        continue;
                    const auto b = Sample(source, hit.stamp.sampleX + radiusX * (2.0 * nx / Divisions - 1.0),
                                          hit.stamp.sampleZ + radiusZ * (2.0 * nz / Divisions - 1.0));
                    const auto pb = b ? Project(frame, *b) : std::nullopt;
                    if (pb)
                        frame.draw.AddLine(*pa, *pb, Theme::U32(Theme::Accent()), 1.0F);
                }
            }
        }
        std::optional<ImVec2> previous;
        for (unsigned i = 0; i <= 64; ++i) {
            const double angle = double(i) * 2 * std::numbers::pi / 64;
            const auto sample = Sample(source, hit.stamp.sampleX + std::cos(angle) * brush_.radiusMeters / source.coordinates.spacingX,
                                       hit.stamp.sampleZ + std::sin(angle) * brush_.radiusMeters / source.coordinates.spacingZ);
            const auto current = sample ? Project(frame, *sample) : std::nullopt;
            if (previous && current)
                frame.draw.AddLine(*previous, *current, Theme::U32(invalid_ ? Theme::Err() : Theme::Accent()), 2.0F);
            previous = current;
        }
    }

    /** @copydoc TerrainViewportTool::Draw */
    bool TerrainViewportTool::Draw(const Frame &frame, std::optional<TerrainToolCommand> &command, const EditorGuiContext &context) {
        const auto *source = frame.source.BorrowSource();
        if (!source || frame.size.x <= 0 || frame.size.y <= 0) {
            Cancel();
            return false;
        }
        const auto view = frame.source.View();
        if (active_ && (!frame.eligible || !Matches(frame, view)))
            Cancel();
        Surface(frame, *source);
        if (ImGui::GetIO().AppFocusLost) {
            Cancel();
            return false;
        }
        const bool controls = Controls(frame, command, context);
        bool surfaceHovered = false;
        if (!controls && frame.eligible) {
            const auto cursor = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(frame.origin);
            ImGui::InvisibleButton("##terrain_surface", frame.size, ImGuiButtonFlags_MouseButtonLeft);
            surfaceHovered = ImGui::IsItemHovered();
            ImGui::SetCursorScreenPos(cursor);
        }
        const auto mouse = ImGui::GetMousePos();
        const bool hovered = surfaceHovered && mouse.x >= frame.origin.x && mouse.y >= frame.origin.y &&
                             mouse.x < frame.origin.x + frame.size.x && mouse.y < frame.origin.y + frame.size.y;
        if (!frame.eligible || controls || !hovered || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            Cancel();
            return controls;
        }
        auto ray = BuildEditorViewportRay(frame.camera, (mouse.x - frame.origin.x) / frame.size.x,
                                          (mouse.y - frame.origin.y) / frame.size.y, frame.size.x / frame.size.y, frame.depth);
        if (ray.HasError()) {
            Cancel();
            return false;
        }
        auto picked = PickTerrainSurface(*source, frame.datasetToViewport, ray.Value());
        if (picked.HasError()) {
            if (active_ || ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                command = Intent(TerrainToolAction::ReportFailure, frame.source);
                command->failure = std::move(picked).ErrorValue();
            }
            Cancel();
            return false;
        }
        if (!picked.Value()) {
            Cancel();
            return false;
        }
        const auto hit = *picked.Value();
        if (!std::isfinite(brush_.radiusMeters) || brush_.radiusMeters <= 0 || !std::isfinite(brush_.amountMeters) ||
            brush_.amountMeters < 0 || !std::isfinite(brush_.blend) || brush_.blend < 0 || brush_.blend > 1) {
            Cancel();
            return false;
        }
        Brush(frame, *source, hit);
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            Cancel();
            active_ = true;
            captured_ = view;
            camera_ = frame.camera;
            placement_ = frame.datasetToViewport;
            depth_ = frame.depth;
            origin_ = frame.origin;
            extent_ = frame.size;
            brush_.flattenMeters = hit.heightMeters;
            if (brush_.noiseSeed == std::numeric_limits<std::uint64_t>::max()) {
                Cancel();
                return false;
            }
            ++brush_.noiseSeed;
            stamps_[count_++] = hit.stamp;
        }
        if (active_ && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const auto &last = stamps_[count_ - 1];
            const double moved = std::hypot((last.sampleX - hit.stamp.sampleX) * source->coordinates.spacingX,
                                            (last.sampleZ - hit.stamp.sampleZ) * source->coordinates.spacingZ);
            if (moved >= brush_.radiusMeters * 0.2) {
                if (count_ == stamps_.size())
                    invalid_ = true;
                else
                    stamps_[count_++] = hit.stamp;
            }
        }
        if (active_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (!invalid_) {
                command = Intent(TerrainToolAction::Stroke, frame.source);
                command->document = captured_;
                command->brush = brush_;
                command->stamps = stamps_;
                command->stampCount = count_;
            } else {
                command = Intent(TerrainToolAction::ReportFailure, frame.source);
                command->failure = MakeError(TerrainEditErrors::LimitExceeded);
            }
            Cancel();
            return true;
        }
        return active_;
    }
}  // namespace Horo::Editor
