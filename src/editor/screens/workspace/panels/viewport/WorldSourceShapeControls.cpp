#include "WorldSourceShapeControls.h"

#include "Horo/Editor/EditorUiComponents.h"
#include "Horo/Editor/Localization/ILocalizationService.h"

#include <algorithm>
#include <limits>

namespace Horo::Editor {
    namespace {
        namespace WS = WorldStreaming;

        /** @brief Keeps out-of-range integer drafts explicitly invalid rather than narrowing or wrapping them. */
        void DrawLength(const char *key, const char *id, std::int64_t &length, const EditorGuiContext &context) {
            auto draft = static_cast<std::uint64_t>(std::max<std::int64_t>(0, length));
            const bool invalid = length <= 0 || length > WS::StreamingSourceRangeLimits::MaximumExtentMillimeters;
            if (Ui::DrawUnsignedPropRow(context.localization.Get("editor", key).c_str(), id, draft, context.theme.fonts, invalid).changed)
                length =
                    draft <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ? static_cast<std::int64_t>(draft) : 0;
        }

        /** @brief Sphere position is handled by the viewport; this form exposes exact radius millimeters. */
        Result<void> DrawGeometry(WS::StreamingSourceSphere &sphere, const EditorGuiContext &context) {
            DrawLength("workspace.world_sources.radius", "##sphere_radius", sphere.radiusMillimeters, context);
            return Result<void>::Success();
        }

        /** @brief Both exact box corners are viewport control points. */
        Result<void> DrawGeometry(WS::StreamingSourceBox &, const EditorGuiContext &) {
            return Result<void>::Success();
        }

        /** @brief Exposes all frustum planes, including orientation, without silently normalizing a user's draft. */
        Result<void> DrawGeometry(WS::StreamingSourceFrustum &frustum, const EditorGuiContext &context) {
            DrawLength("workspace.world_sources.maximum_extent", "##frustum_extent", frustum.maximumExtentMillimeters, context);
            for (std::size_t index = 0; index < frustum.planes.size(); ++index) {
                ImGui::PushID(static_cast<int>(index));
                auto &plane = frustum.planes[index];
                std::array<float, 3> normal{plane.inwardNormal.x, plane.inwardNormal.y, plane.inwardNormal.z};
                if (Ui::DrawFloat3PropRow(context.localization.Get("editor", "workspace.world_sources.plane_normal").c_str(),
                                          "##plane_normal", normal, context.theme.fonts)
                        .changed)
                    plane.inwardNormal = {normal[0], normal[1], normal[2]};
                if (Ui::Button({.label = context.localization.Get("editor", "workspace.world_sources.normalize").c_str(),
                                .variant = Ui::ButtonVariant::Secondary})) {
                    const auto normalized = Math::TryNormalize(plane.inwardNormal);
                    if (normalized.HasError()) {
                        ImGui::PopID();
                        return Result<void>::Failure(normalized.ErrorValue());
                    }
                    plane.inwardNormal = normalized.Value();
                }
                static_cast<void>(Ui::DrawFloatPropRow(context.localization.Get("editor", "workspace.world_sources.plane_distance").c_str(),
                                                       "##plane_distance", plane.distanceMillimeters, context.theme.fonts));
                ImGui::PopID();
            }
            return Result<void>::Success();
        }

        /** @brief Adds or removes bounded owned vertices; an overflow failure never changes pointCount or existing points. */
        Result<void> DrawGeometry(WS::StreamingSourcePathVolume &path, const EditorGuiContext &context) {
            if (path.pointCount < 2 || path.pointCount > path.points.size())
                return Result<void>::Failure(MakeError(WorldDocumentErrors::Invalid));
            DrawLength("workspace.world_sources.half_extent", "##path_extent", path.halfExtentMillimeters, context);
            if (Ui::Button({.label = context.localization.Get("editor", "workspace.world_sources.add_point").c_str(),
                            .variant = Ui::ButtonVariant::Secondary,
                            .enabled = path.pointCount < path.points.size()})) {
                const auto point = WorldSourceViewportGlobal(path.points[path.pointCount - 1], {1, 0, 0});
                if (point.HasError())
                    return Result<void>::Failure(point.ErrorValue());
                path.points[path.pointCount++] = point.Value();
            }
            if (Ui::Button({.label = context.localization.Get("editor", "workspace.world_sources.remove_point").c_str(),
                            .variant = Ui::ButtonVariant::Secondary,
                            .enabled = path.pointCount > 2}))
                --path.pointCount;
            return Result<void>::Success();
        }
    }  // namespace

    /** @copydoc DrawWorldSourceShapeControls */
    Result<void> DrawWorldSourceShapeControls(PartitionedWorldSource &source, const EditorGuiContext &context) {
        const auto edited = std::visit([&](auto &shape) {
            return DrawGeometry(shape, context);
        }, source.shape);
        if (edited.HasError())
            return edited;
        return WS::ValidateStreamingSourceShape(source.shape);
    }
}  // namespace Horo::Editor
