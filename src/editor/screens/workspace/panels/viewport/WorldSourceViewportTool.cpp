#include "WorldSourceViewportTool.h"

#include "Horo/Editor/EditorTheme.h"
#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"
#include "editor/renderer/EditorViewportScene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <type_traits>

namespace Horo::Editor {
    namespace {
        namespace WS = WorldStreaming;
        using Point = Math::WorldCoordinate64;

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
        void Emit(EditorWorkspaceViewCommandData &command, WorldSourceToolAction action, WorldDocumentFence fence,
                  const WorldViewportBinding &binding, std::optional<PartitionedWorldSource> source = {}) {
            command.command = EditorWorkspaceViewCommand::WorldSourceTool;
            command.worldSource = WorldSourceToolCommand{.action = action,
                                                         .edit = {.expected = fence,
                                                                  .source = source ? source->id : WS::StreamingSourceId{},
                                                                  .value = std::move(source)},
                                                         .globalOrigin = binding.globalOrigin,
                                                         .viewRevision = binding.viewRevision};
        }

        /** @brief Builds bounded usable authored defaults, without runtime registration or activation. */
        Result<PartitionedWorldSource> DefaultSource(std::size_t shape, Point origin) {
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
            const auto translated = std::visit([&](auto &geometry) -> Result<void> {
                using T = std::decay_t<decltype(geometry)>;
                const auto translate = [&](Point &point) -> Result<void> {
                    const auto local = WorldSourceViewportPoint(point, {});
                    if (local.HasError())
                        return Result<void>::Failure(local.ErrorValue());
                    const auto global = WorldSourceViewportGlobal(origin, local.Value());
                    if (global.HasError())
                        return Result<void>::Failure(global.ErrorValue());
                    point = global.Value();
                    return Result<void>::Success();
                };
                if constexpr (std::is_same_v<T, WS::StreamingSourceSphere>)
                    return translate(geometry.center);
                else if constexpr (std::is_same_v<T, WS::StreamingSourceBox>) {
                    if (const auto minimum = translate(geometry.minimum); minimum.HasError())
                        return minimum;
                    return translate(geometry.maximum);
                } else if constexpr (std::is_same_v<T, WS::StreamingSourceFrustum>)
                    return translate(geometry.anchor);
                else {
                    for (std::size_t index = 0; index < geometry.pointCount; ++index) {
                        if (const auto point = translate(geometry.points[index]); point.HasError())
                            return point;
                    }
                    return Result<void>::Success();
                }
            }, value.shape);
            if (translated.HasError())
                return Result<PartitionedWorldSource>::Failure(translated.ErrorValue());
            return Result<PartitionedWorldSource>::Success(std::move(value));
        }
    }  // namespace

    /** @copydoc WorldSourceViewportTool::Cancel */
    void WorldSourceViewportTool::Cancel() {
        draft_.reset();
        dragged_.reset();
        fence_ = {};
        origin_ = {};
        viewRevision_ = 0;
    }

    /** @copydoc WorldSourceViewportTool::DrawPopup */
    void WorldSourceViewportTool::DrawPopup(const EditorWorkspaceViewModel &view, EditorWorkspaceViewCommandData &command,
                                            const EditorGuiContext &context) {
        const auto label = [&](const char *key) -> const char * {
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
            }
        }
        for (const auto &source : view.worldViewport.snapshot->document.sources) {
            const std::string name = std::to_string(source.id.Value());
            if (Ui::Button({.label = name.c_str(), .variant = Ui::ButtonVariant::Secondary})) {
                draft_ = source;
                fence_ = fence;
                origin_ = view.worldViewport.globalOrigin;
                viewRevision_ = view.worldViewport.viewRevision;
                dragged_.reset();
            }
        }
        if (draft_) {
            float priority = draft_->priority.Value();
            static_cast<void>(
                Ui::DrawFloatPropRow(label("workspace.world_sources.priority"), "##source_priority", priority, context.theme.fonts));
            if (const auto value = WS::StreamingSourcePriority::Create(priority); value.HasValue())
                draft_->priority = value.Value();
            int target = static_cast<int>(draft_->target);
            const std::array<const char *, 3> targets{label("workspace.world_sources.unloaded"), label("workspace.world_sources.loaded"),
                                                      label("workspace.world_sources.activated")};
            if (Ui::DrawComboPropRow(label("workspace.world_sources.target"), "##source_target", target, targets, context.theme.fonts))
                draft_->target = static_cast<WS::StreamingDesiredResidency>(target);
            int intent = static_cast<int>(draft_->intent);
            const std::array<const char *, 4> intents{label("workspace.world_sources.camera"), label("workspace.world_sources.gameplay"),
                                                      label("workspace.world_sources.network"), label("workspace.world_sources.preload")};
            if (Ui::DrawComboPropRow(label("workspace.world_sources.intent"), "##source_intent", intent, intents, context.theme.fonts))
                draft_->intent = static_cast<WS::StreamingSourceIntent>(intent);
            static_cast<void>(
                Ui::DrawColor3PropRow(label("workspace.world_sources.color"), "##source_color", draft_->debugColor, context.theme.fonts));
            if (Ui::Button({.label = label("workspace.world_sources.apply")}))
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
        if (view.worldSourceError)
            ImGui::TextWrapped("%s: %s", label("workspace.world_sources.rejected"), view.worldSourceError->code.Value().c_str());
    }

    /** @copydoc WorldSourceViewportTool::Draw */
    bool WorldSourceViewportTool::Draw(ImDrawList &draw, ImVec2 origin, ImVec2 size, const EditorWorkspaceViewModel &view,
                                       EditorWorkspaceViewCommandData &command, const EditorGuiContext &context, Math::ClipDepthRange depth,
                                       bool eligible) {
        if (!view.worldViewport.snapshot ||
            (draft_ && (fence_ != view.worldViewport.snapshot->fence || origin_ != view.worldViewport.globalOrigin ||
                        viewRevision_ != view.worldViewport.viewRevision)) ||
            view.playState != EditorPlayState::Idle || (!eligible && dragged_))
            Cancel();
        ImGui::SetCursorScreenPos({origin.x + Theme::Layout::BodyPadX, origin.y + size.y - ImGui::GetFrameHeightWithSpacing()});
        ImGui::BeginDisabled(!eligible);
        if (Ui::Button({.label = context.localization.Get("editor", "workspace.world_sources.title").c_str(),
                        .variant = Ui::ButtonVariant::Secondary}))
            ImGui::OpenPopup("##WorldSources");
        ImGui::EndDisabled();
        const bool toolbarHovered = ImGui::IsItemHovered();
        bool popup = false;
        if (ImGui::BeginPopup("##WorldSources")) {
            popup = true;
            DrawPopup(view, command, context);
            ImGui::EndPopup();
        }
        const bool handles =
            view.worldViewport.snapshot && DrawHandles(draw, origin, size, view, command, depth, eligible && !popup && !toolbarHovered);
        return popup || toolbarHovered || handles;
    }

    /** @copydoc WorldSourceViewportTool::DrawHandles */
    bool WorldSourceViewportTool::DrawHandles(ImDrawList &draw, ImVec2 origin, ImVec2 size, const EditorWorkspaceViewModel &view,
                                              EditorWorkspaceViewCommandData &command, Math::ClipDepthRange depth, bool eligible) {
        const bool hadDrag = dragged_.has_value();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || (dragged_ && !ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))) {
            Cancel();
            return hadDrag;
        }
        draw.PushClipRect(origin, {origin.x + size.x, origin.y + size.y}, true);
        for (const auto &authored : view.worldViewport.snapshot->document.sources) {
            const auto &source = draft_ && draft_->id == authored.id ? *draft_ : authored;
            const auto points = WorldSourceGizmoPoints(source);
            if (points.HasError())
                continue;
            const auto color = ImGui::ColorConvertFloat4ToU32({source.debugColor[0], source.debugColor[1], source.debugColor[2], 1});
            std::optional<ImVec2> previous;
            for (std::size_t handle = 0; handle < points.Value().size(); ++handle) {
                const auto local = WorldSourceViewportPoint(points.Value()[handle], view.worldViewport.globalOrigin);
                if (local.HasError())
                    continue;
                const auto projected = ProjectEditorViewportPoint(view.viewportCamera, local.Value(), size.x / size.y, depth);
                if (projected.HasError() || !projected.Value())
                    continue;
                const auto normalized = projected.Value()->viewportPosition;
                const ImVec2 pixel{origin.x + normalized.x * size.x, origin.y + normalized.y * size.y};
                if (previous)
                    draw.AddLine(*previous, pixel, color);
                previous = pixel;
                const ImVec2 mouse = ImGui::GetMousePos();
                const float dx = mouse.x - pixel.x;
                const float dy = mouse.y - pixel.y;
                const bool hovered = dx * dx + dy * dy <= 64;
                draw.AddCircleFilled(pixel, hovered ? 6 : 4, color);
                if (eligible && hovered && !dragged_ && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    const auto normal = Math::TryNormalize(view.viewportCamera.target - view.viewportCamera.position);
                    if (normal.HasError())
                        continue;
                    const auto intersection = PointerPoint(origin, size, view.viewportCamera, depth, normal.Value(), local.Value());
                    if (!intersection)
                        continue;
                    draft_ = authored;
                    fence_ = view.worldViewport.snapshot->fence;
                    origin_ = view.worldViewport.globalOrigin;
                    viewRevision_ = view.worldViewport.viewRevision;
                    dragged_ = handle;
                    dragStart_ = points.Value()[handle];
                    dragPlanePoint_ = local.Value();
                    dragNormal_ = normal.Value();
                    dragIntersection_ = *intersection;
                }
            }
        }
        draw.PopClipRect();
        if (!dragged_ || !draft_)
            return false;
        if (!eligible) {
            Cancel();
            return true;
        }
        const auto pointer = PointerPoint(origin, size, view.viewportCamera, depth, dragNormal_, dragPlanePoint_);
        if (pointer) {
            const auto position = WorldSourceViewportGlobal(dragStart_, *pointer - dragIntersection_);
            if (position.HasValue()) {
                const auto value = MoveWorldSourceGizmo(*draft_, *dragged_, position.Value());
                if (value.HasValue())
                    draft_ = value.Value();
            }
        }
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            Emit(command, WorldSourceToolAction::Edit, fence_, view.worldViewport, draft_);
            dragged_.reset();
        }
        return true;
    }
}  // namespace Horo::Editor
