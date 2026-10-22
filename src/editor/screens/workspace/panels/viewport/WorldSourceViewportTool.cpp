#include "WorldSourceViewportTool.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/GuiLiveReload.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "ViewportOverlay.h"
#include "WorldSourceShapeControls.h"
#include "editor/renderer/EditorViewportScene.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Horo::Editor {
    namespace {
        namespace WS = WorldStreaming;
        using Point = Math::WorldCoordinate64;

        /** @brief Detects camera pose/projection changes inside the same host view before reprojecting a gesture. */
        bool SameCamera(const EditorViewportCamera &left, const EditorViewportCamera &right) {
            return left.projection == right.projection && left.position == right.position && left.target == right.target &&
                   left.up == right.up && left.verticalFovRadians == right.verticalFovRadians &&
                   left.orthographicHeight == right.orthographicHeight && left.nearPlane == right.nearPlane &&
                   left.farPlane == right.farPlane;
        }

        /** @brief Rejects distant points with exact unsigned differences before constructing any per-frame error value. */
        bool FitsViewportWindow(const Point &point, const Point &origin) {
            const auto p = point.Millimeters();
            const auto o = origin.Millimeters();
            for (std::size_t axis = 0; axis < p.size(); ++axis) {
                const auto distance = p[axis] >= o[axis] ? static_cast<std::uint64_t>(p[axis]) - static_cast<std::uint64_t>(o[axis])
                                                         : static_cast<std::uint64_t>(o[axis]) - static_cast<std::uint64_t>(p[axis]);
                if (distance > 8'000'000)
                    return false;
            }
            return true;
        }

        /** @brief Intersects the current pointer ray with the immutable camera-facing gesture plane. */
        std::optional<Math::Vec3> PointerPoint(ImVec2 origin, ImVec2 size, const EditorViewportCamera &camera, Math::ClipDepthRange depth,
                                               Math::Vec3 normal, Math::Vec3 planePoint) {
            const ImVec2 mouse = ImGui::GetMousePos();
            const auto ray =
                BuildEditorViewportRay(camera, (mouse.x - origin.x) / size.x, (mouse.y - origin.y) / size.y, size.x / size.y, depth);
            if (ray.HasError())
                return std::nullopt;
            const float divisor = Math::Dot(normal, ray.Value().direction);
            if (std::abs(divisor) < Math::DefaultEpsilon)
                return std::nullopt;
            const float distance = Math::Dot(normal, planePoint - ray.Value().origin) / divisor;
            if (!std::isfinite(distance) || distance < 0)
                return std::nullopt;
            return ray.Value().origin + ray.Value().direction * distance;
        }

        /** @brief Emits one complete intent; UI never obtains mutable document storage. */
        void Emit(EditorWorkspaceViewCommandData &command, WorldSourceToolAction action, const WorldDocumentFence &fence,
                  const WorldViewportBinding &binding, std::optional<PartitionedWorldSource> source = {}) {
            command.command = EditorWorkspaceViewCommand::WorldSourceTool;
            command.worldSource = WorldSourceToolCommand{.action = action,
                                                         .edit = {.expected = fence,
                                                                  .source = source ? source->id : WS::StreamingSourceId{},
                                                                  .value = std::move(source)},
                                                         .globalOrigin = binding.globalOrigin,
                                                         .viewRevision = binding.viewRevision};
        }

        /** @brief Translates a detached default point through checked local/global conversion. */
        Result<void> TranslatePoint(Point &point, const Point &origin) {
            const auto local = WorldSourceViewportPoint(point, {});
            if (local.HasError())
                return Result<void>::Failure(local.ErrorValue());
            const auto global = WorldSourceViewportGlobal(origin, local.Value());
            if (global.HasError())
                return Result<void>::Failure(global.ErrorValue());
            point = global.Value();
            return Result<void>::Success();
        }

        /** @brief Translates only the sphere center; its extent remains exact. */
        Result<void> TranslateGeometry(WS::StreamingSourceSphere &shape, const Point &origin) {
            return TranslatePoint(shape.center, origin);
        }

        /** @brief Rejects either box corner overflow before the detached default can be submitted. */
        Result<void> TranslateGeometry(WS::StreamingSourceBox &shape, const Point &origin) {
            if (const auto minimum = TranslatePoint(shape.minimum, origin); minimum.HasError())
                return minimum;
            return TranslatePoint(shape.maximum, origin);
        }

        /** @brief Translates the frustum anchor without changing its plane equations. */
        Result<void> TranslateGeometry(WS::StreamingSourceFrustum &shape, const Point &origin) {
            return TranslatePoint(shape.anchor, origin);
        }

        /** @brief Translates the complete bounded path prefix before publication. */
        Result<void> TranslateGeometry(WS::StreamingSourcePathVolume &shape, const Point &origin) {
            for (std::size_t index = 0; index < shape.pointCount; ++index) {
                if (const auto point = TranslatePoint(shape.points[index], origin); point.HasError())
                    return point;
            }
            return Result<void>::Success();
        }

        /** @brief Builds bounded usable authored defaults, without runtime registration or activation. */
        Result<PartitionedWorldSource> DefaultSource(std::size_t shape, const Point &origin) {
            PartitionedWorldSource value{.shape = WS::StreamingSourceSphere{{}, 1000},
                                         .priority = WS::StreamingSourcePriority::Create(1).Value()};
            if (shape == 1)
                value.shape = WS::StreamingSourceBox{Point::FromMillimeters(-500, -500, -500), Point::FromMillimeters(500, 500, 500)};
            if (shape == 2) {
                WS::StreamingSourceFrustum frustum{.maximumExtentMillimeters = 1000};
                const std::array<Math::Vec3, 6> normals{{{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}};
                for (std::size_t index = 0; index < normals.size(); ++index)
                    frustum.planes[index] = {normals[index], 500};
                value.shape = frustum;
            }
            if (shape == 3) {
                WS::StreamingSourcePathVolume path{.pointCount = 2, .halfExtentMillimeters = 200};
                path.points[1] = Point::FromMillimeters(1000, 0, 0);
                value.shape = path;
            }
            if (const auto translated = std::visit(
                    [&]<typename Geometry>(Geometry &geometry) {
                return TranslateGeometry(geometry, origin);
            }, value.shape);
                translated.HasError())
                return Result<PartitionedWorldSource>::Failure(translated.ErrorValue());
            return Result<PartitionedWorldSource>::Success(std::move(value));
        }
    }  // namespace

    /** @copydoc WorldSourceViewportTool::Cancel */
    void WorldSourceViewportTool::Cancel() {
        draft_.reset();
        projection_.reset();
        dragOriginal_.reset();
        dragged_.reset();
        previewError_.reset();
        fence_ = {};
        origin_ = {};
        viewRevision_ = 0;
    }

    /** @copydoc WorldSourceViewportTool::DrawSourceProperties */
    void WorldSourceViewportTool::DrawSourceProperties(const EditorGuiContext &context) {
        const auto label = [&](const char *key) {
            return context.localization.Get("editor", key).c_str();
        };
        float priority = draft_->priority.Value();
        static_cast<void>(
            Ui::DrawFloatPropRow(label("workspace.world_sources.priority"), "##source_priority", priority, context.theme.fonts));
        const auto value = WS::StreamingSourcePriority::Create(priority);
        draft_->priority = value.HasValue() ? value.Value() : WS::StreamingSourcePriority{};
        int target = static_cast<int>(draft_->target);
        if (const auto targets = std::array{label("workspace.world_sources.unloaded"), label("workspace.world_sources.loaded"),
                                            label("workspace.world_sources.activated")};
            Ui::DrawComboPropRow(label("workspace.world_sources.target"), "##source_target", target, targets, context.theme.fonts))
            draft_->target = static_cast<WS::StreamingDesiredResidency>(target);
        int intent = static_cast<int>(draft_->intent);
        if (const auto intents = std::array{label("workspace.world_sources.camera"), label("workspace.world_sources.gameplay"),
                                            label("workspace.world_sources.network"), label("workspace.world_sources.preload")};
            Ui::DrawComboPropRow(label("workspace.world_sources.intent"), "##source_intent", intent, intents, context.theme.fonts))
            draft_->intent = static_cast<WS::StreamingSourceIntent>(intent);
        static_cast<void>(
            Ui::DrawColor3PropRow(label("workspace.world_sources.color"), "##source_color", draft_->debugColor, context.theme.fonts));
        const auto previousShape = draft_->shape;
        const auto shape = DrawWorldSourceShapeControls(*draft_, context);
        if (shape.HasError())
            previewError_ = shape.ErrorValue();
        else if (previousShape != draft_->shape)
            previewError_.reset();
    }

    /** @copydoc WorldSourceViewportTool::DrawSourceList */
    void WorldSourceViewportTool::DrawSourceList(const EditorWorkspaceViewModel &view) {
        const auto fence = view.worldViewport.snapshot->fence;
        for (const auto &source : view.worldViewport.snapshot->document.sources) {
            const std::string name = std::to_string(source.id.Value());
            if (Ui::Button({.label = name.c_str(), .variant = Ui::ButtonVariant::Secondary})) {
                draft_ = source;
                projection_.reset();
                previewError_.reset();
                fence_ = fence;
                origin_ = view.worldViewport.globalOrigin;
                viewRevision_ = view.worldViewport.viewRevision;
                dragged_.reset();
            }
        }
    }

    /** @copydoc WorldSourceViewportTool::DrawPopup */
    void WorldSourceViewportTool::DrawPopup(const EditorWorkspaceViewModel &view, EditorWorkspaceViewCommandData &command,
                                            const EditorGuiContext &context) {
        const auto label = [&](const char *key) {
            return context.localization.Get("editor", key).c_str();
        };
        if (!view.worldViewport.snapshot) {
            ImGui::TextWrapped("%s", label("workspace.world_sources.unavailable"));
            return;
        }
        ImGui::TextWrapped("%s", label("workspace.world_sources.authoring_only"));
        const auto fence = view.worldViewport.snapshot->fence;
        ImGui::BeginDisabled(view.playState != EditorPlayState::Idle);
        constexpr std::array<const char *, 4> createKeys{"workspace.world_sources.sphere", "workspace.world_sources.box",
                                                         "workspace.world_sources.frustum", "workspace.world_sources.path"};
        for (std::size_t shape = 0; shape < createKeys.size(); ++shape) {
            if (Ui::Button({.label = label(createKeys[shape]), .variant = Ui::ButtonVariant::Secondary})) {
                const auto source = DefaultSource(shape, view.worldViewport.globalOrigin);
                if (source.HasValue())
                    Emit(command, WorldSourceToolAction::Create, fence, view.worldViewport, source.Value());
                else
                    previewError_ = source.ErrorValue();
            }
        }
        DrawSourceList(view);
        if (draft_) {
            DrawSourceProperties(context);
            if (Ui::Button({.label = label("workspace.world_sources.apply"), .enabled = !previewError_ && draft_->priority.IsValid()}))
                Emit(command, WorldSourceToolAction::Edit, fence_, view.worldViewport, draft_);
            if (Ui::Button({.label = label("workspace.world_sources.remove"), .variant = Ui::ButtonVariant::Secondary}))
                Emit(command, WorldSourceToolAction::Remove, fence_, view.worldViewport, draft_);
            if (Ui::Button({.label = label("workspace.world_sources.cancel"), .variant = Ui::ButtonVariant::Secondary}))
                Cancel();
        }
        if (Ui::Button({.label = label("workspace.world_sources.undo"), .variant = Ui::ButtonVariant::Secondary}))
            Emit(command, WorldSourceToolAction::Undo, fence, view.worldViewport);
        if (Ui::Button({.label = label("workspace.world_sources.redo"), .variant = Ui::ButtonVariant::Secondary}))
            Emit(command, WorldSourceToolAction::Redo, fence, view.worldViewport);
        ImGui::EndDisabled();
        const auto &error = previewError_ ? previewError_ : view.worldSourceError;
        if (error)
            ImGui::TextWrapped("%s: %s", label("workspace.world_sources.rejected"), error->code.Value().c_str());
    }

    /** @copydoc WorldSourceViewportTool::MatchesProjection */
    bool WorldSourceViewportTool::MatchesProjection(const EditorViewportCamera &camera, ImVec2 origin, ImVec2 extent,
                                                    Math::ClipDepthRange depth) const {
        if (!projection_)
            return true;
        const auto &captured = *projection_;
        return SameCamera(captured.camera, camera) && captured.origin.x == origin.x && captured.origin.y == origin.y &&
               captured.extent.x == extent.x && captured.extent.y == extent.y && captured.depth == depth;
    }

    /** @copydoc WorldSourceViewportTool::Draw */
    bool WorldSourceViewportTool::Draw(const Frame &frame, EditorWorkspaceViewCommandData &command, const EditorGuiContext &context) {
        const auto &[draw, origin, size, view, depth, eligible] = frame;
        if (!view.worldViewport.snapshot ||
            (draft_ && (fence_ != view.worldViewport.snapshot->fence || origin_ != view.worldViewport.globalOrigin ||
                        viewRevision_ != view.worldViewport.viewRevision)) ||
            !MatchesProjection(view.viewportCamera, origin, size, depth) || view.playState != EditorPlayState::Idle ||
            (!eligible && dragged_.has_value()))
            Cancel();
        const bool canEdit = eligible && view.playState == EditorPlayState::Idle;
        const auto row = ResolveViewportStatusRow(origin, size, view.objects.size(), context.localization);
        bool toolbarHovered = false;
        if (row.actionsWidth >= row.height) {
            const float scale = Theme::GetActiveTokens().sizes.uiScale;
            ImGui::SetCursorScreenPos(row.actionsPosition);
            if (Ui::Button({.label = context.localization.Get("editor", "workspace.content_browser.location.sources").c_str(),
                            .size = {std::min(row.actionsWidth, 112.0F * scale) / scale, row.height / scale},
                            .variant = Ui::ButtonVariant::Secondary,
                            .enabled = canEdit,
                            .componentSize = Ui::ComponentSize::Small}))
                ImGui::OpenPopup("##WorldSources");
            toolbarHovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
            if (toolbarHovered)
                Ui::ShowTooltip(context.localization.Get("editor", "workspace.world_sources.title").c_str(), &context.theme.fonts);
        }
        bool popup = false;
        if (Ui::BeginMenuPopup("##WorldSources")) {
            popup = true;
            DrawPopup(view, command, context);
            Ui::EndMenuPopup();
        }
        const bool handles =
            view.worldViewport.snapshot && DrawHandles(draw, origin, size, view, command, depth, canEdit && !popup && !toolbarHovered);
        return popup || toolbarHovered || handles;
    }

    /** @copydoc WorldSourceViewportTool::BeginDrag */
    void WorldSourceViewportTool::BeginDrag(const Frame &frame, const PartitionedWorldSource &source, std::size_t handle,
                                            const Point &global, Math::Vec3 local) {
        const auto normal = Math::TryNormalize(frame.view.viewportCamera.target - frame.view.viewportCamera.position);
        if (normal.HasError())
            return;
        const auto intersection = PointerPoint(frame.origin, frame.size, frame.view.viewportCamera, frame.depth, normal.Value(), local);
        if (!intersection)
            return;
        if (!draft_ || draft_->id != source.id)
            draft_ = source;
        dragOriginal_ = draft_;
        fence_ = frame.view.worldViewport.snapshot->fence;
        projection_ = ProjectionCapture{frame.view.viewportCamera, frame.origin, frame.size, frame.depth};
        origin_ = frame.view.worldViewport.globalOrigin;
        viewRevision_ = frame.view.worldViewport.viewRevision;
        dragged_ = handle;
        previewError_.reset();
        dragStart_ = global;
        dragPlanePoint_ = local;
        dragNormal_ = normal.Value();
        dragIntersection_ = *intersection;
    }

    /** @copydoc WorldSourceViewportTool::DrawSourceHandles */
    void WorldSourceViewportTool::DrawSourceHandles(const Frame &frame, const PartitionedWorldSource &authored) {
        const auto &source = draft_ && draft_->id == authored.id ? *draft_ : authored;
        const auto points = WorldSourceGizmoPoints(source);
        if (points.HasError())
            return;
        const auto color = ImGui::ColorConvertFloat4ToU32({source.debugColor[0], source.debugColor[1], source.debugColor[2], 1});
        std::optional<ImVec2> previous;
        for (std::size_t handle = 0; handle < points.Value().Points().size(); ++handle) {
            if (!FitsViewportWindow(points.Value().Points()[handle], frame.view.worldViewport.globalOrigin))
                continue;
            const auto local = WorldSourceViewportPoint(points.Value().Points()[handle], frame.view.worldViewport.globalOrigin);
            if (local.HasError())
                continue;
            const auto projected =
                ProjectEditorViewportPoint(frame.view.viewportCamera, local.Value(), frame.size.x / frame.size.y, frame.depth);
            if (projected.HasError() || !projected.Value())
                continue;
            const auto normalized = projected.Value()->viewportPosition;
            const ImVec2 pixel{frame.origin.x + normalized.x * frame.size.x, frame.origin.y + normalized.y * frame.size.y};
            if (previous)
                frame.draw.AddLine(*previous, pixel, color);
            previous = pixel;
            const ImVec2 mouse = ImGui::GetMousePos();
            const float dx = mouse.x - pixel.x;
            const float dy = mouse.y - pixel.y;
            const bool hovered = frame.eligible && dx * dx + dy * dy <= 64;
            frame.draw.AddCircleFilled(pixel, hovered ? 6 : 4, color);
            if (frame.eligible && hovered && !dragged_.has_value() && (!draft_ || draft_->id == authored.id) &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                BeginDrag(frame, authored, handle, points.Value().Points()[handle], local.Value());
        }
    }

    /** @copydoc WorldSourceViewportTool::UpdateDrag */
    bool WorldSourceViewportTool::UpdateDrag(const Frame &frame, EditorWorkspaceViewCommandData &command) {
        if (!dragged_.has_value() || !draft_)
            return false;
        if (!frame.eligible) {
            Cancel();
            return true;
        }
        if (const auto pointer =
                PointerPoint(frame.origin, frame.size, frame.view.viewportCamera, frame.depth, dragNormal_, dragPlanePoint_);
            pointer.has_value()) {
            const auto position = WorldSourceViewportGlobal(dragStart_, *pointer - dragIntersection_);
            if (position.HasValue()) {
                const auto value = MoveWorldSourceGizmo(*dragOriginal_, *dragged_, position.Value());
                previewError_ = value.HasError() ? std::optional<Error>{value.ErrorValue()} : std::nullopt;
                if (value.HasValue())
                    draft_ = value.Value();
            } else {
                previewError_ = position.ErrorValue();
            }
        } else {
            previewError_ = MakeError(GuiLive::HostSymbol<&WorldDocumentErrors::Invalid>());
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (!previewError_) {
                Emit(command, WorldSourceToolAction::Edit, fence_, frame.view.worldViewport, draft_);
                dragged_.reset();
                dragOriginal_.reset();
            } else {
                const auto error = previewError_;
                Cancel();
                previewError_ = error;
            }
        }
        return true;
    }

    /** @copydoc WorldSourceViewportTool::DrawHandles */
    bool WorldSourceViewportTool::DrawHandles(ImDrawList &draw, ImVec2 origin, ImVec2 size, const EditorWorkspaceViewModel &view,
                                              EditorWorkspaceViewCommandData &command, Math::ClipDepthRange depth, bool eligible) {
        const bool hadDrag = dragged_.has_value();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
            (dragged_.has_value() && !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))) {
            Cancel();
            return hadDrag;
        }
        const Frame frame{draw, origin, size, view, depth, eligible};
        draw.PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
        for (const auto &source : view.worldViewport.snapshot->document.sources)
            DrawSourceHandles(frame, source);
        draw.PopClipRect();
        return UpdateDrag(frame, command);
    }
}  // namespace Horo::Editor
