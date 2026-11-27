#include "Horo/XR/XRMultiviewScene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace Horo::XR {
    namespace {
        /** @brief Checks whether native layer routing can address this complete XR target tuple in one rendering scope. */
        [[nodiscard]] bool LayeredTargets(const XRViewRenderPlan &plan) noexcept {
            const auto targets = plan.Targets();
            if (targets.empty())
                return false;
            const auto &first = targets.front();
            if (first.role != XRExternalRenderTargetRole::ProjectionColor || first.arrayLayerCount != plan.Views().size())
                return false;
            const auto depth = std::ranges::find_if(targets, [](const auto &target) {
                return target.role == XRExternalRenderTargetRole::ProjectionDepth;
            });
            std::size_t colorLayer = 0;
            std::size_t depthCount = 0;
            for (const auto &target : targets) {
                if (const auto &baseline = target.role == XRExternalRenderTargetRole::ProjectionDepth ? *depth : first;
                    target.image != baseline.image || target.format != baseline.format || target.renderRectangle != first.renderRectangle ||
                    target.allocationExtent != first.allocationExtent || target.arrayLayerCount != first.arrayLayerCount ||
                    target.sampleCount != first.sampleCount)
                    return false;
                if (target.role == XRExternalRenderTargetRole::ProjectionColor) {
                    // Indexed instancing addresses the array layer with the runtime view ordinal.
                    if (target.arrayLayer != colorLayer)
                        return false;
                    ++colorLayer;
                    continue;
                }
                ++depthCount;
                const auto color = std::ranges::find_if(targets, [&target](const auto &candidate) {
                    return candidate.view == target.view && candidate.role == XRExternalRenderTargetRole::ProjectionColor;
                });
                if (color == targets.end() || color->arrayLayer != target.arrayLayer)
                    return false;
            }
            return depthCount == 0 || depthCount == plan.Views().size();
        }

        /** @brief Intersects real backend capability with the actual image/layer topology without changing device selection. */
        [[nodiscard]] Render::MultiviewCapabilities EffectiveCapabilities(const XRViewRenderPlan &plan,
                                                                          const XRMultiviewOwnerState &active) noexcept {
            auto capabilities = active.capabilities;
            capabilities.instancedTargetRouting = capabilities.instancedTargetRouting && LayeredTargets(plan);
            return capabilities;
        }

        /** @brief Builds a finite right-handed asymmetric projection in the selected API clip-depth convention. */
        [[nodiscard]] Result<Math::Mat4> Projection(const XRViewProjection &projection, const float nearPlane, const float farPlane,
                                                    const Math::ClipDepthRange depthRange) {
            if (!projection.IsValid() || !std::isfinite(nearPlane) || !std::isfinite(farPlane) || nearPlane <= 0.0F ||
                farPlane <= nearPlane ||
                (depthRange != Math::ClipDepthRange::ZeroToOne && depthRange != Math::ClipDepthRange::NegativeOneToOne))
                return Result<Math::Mat4>::Failure(MakeError(XRErrors::ViewPlanInvalid));
            const double width = static_cast<double>(projection.rightTangent) - projection.leftTangent;
            const double height = static_cast<double>(projection.upTangent) - projection.downTangent;
            const double depth = static_cast<double>(nearPlane) - farPlane;
            const bool zeroToOne = depthRange == Math::ClipDepthRange::ZeroToOne;
            Math::Mat4 result;
            result.values[0] = static_cast<float>(2.0 / width);
            result.values[5] = static_cast<float>(2.0 / height);
            result.values[8] = static_cast<float>((static_cast<double>(projection.rightTangent) + projection.leftTangent) / width);
            result.values[9] = static_cast<float>((static_cast<double>(projection.upTangent) + projection.downTangent) / height);
            result.values[10] = static_cast<float>((zeroToOne ? farPlane : static_cast<double>(farPlane) + nearPlane) / depth);
            result.values[11] = -1.0F;
            result.values[14] = static_cast<float>((zeroToOne ? 1.0 : 2.0) * nearPlane * farPlane / depth);
            if (!Math::IsFinite(result))
                return Result<Math::Mat4>::Failure(MakeError(XRErrors::ViewPlanInvalid));
            return Result<Math::Mat4>::Success(result);
        }

        /** @brief Composes the view's declared reference into canonical world space without inventing tracking evidence. */
        [[nodiscard]] Result<Math::Mat4> WorldToView(const XRViewDescriptor &view, const XRPoseSample &referenceToWorld,
                                                     const XRMultiviewOwnerState &active) {
            if (referenceToWorld.Target().kind != XRSpaceKind::World)
                return Result<Math::Mat4>::Failure(MakeError(XRErrors::CoordinateSpaceIncompatible));
            auto pose = XRPoseSample::Create(view.pose, active.session, active.originRevision);
            if (pose.HasError())
                return Result<Math::Mat4>::Failure(pose.ErrorValue());
            const std::array chain{pose.Value(), referenceToWorld};
            const auto world = ComposeXRSpaceTransforms(chain, active.session, active.originRevision);
            if (world.HasError())
                return Result<Math::Mat4>::Failure(world.ErrorValue());
            const auto &components = world.Value().Components();
            if (!components.positionMeters.value || !components.orientation.value)
                return Result<Math::Mat4>::Failure(MakeError(XRErrors::OperationUnavailable));
            const Math::Transform worldFromView{.translation = *components.positionMeters.value, .rotation = *components.orientation.value};
            const auto transform = worldFromView.TryToMatrix();
            if (transform.HasError())
                return transform;
            return Math::TryInverseAffine(transform.Value());
        }

        /** @brief Requires the renderer's origin/configuration fences to name the exact admitted XR publication. */
        [[nodiscard]] Result<void> ValidateOwner(const XRViewRenderPlan &plan, const Render::MultiviewPublication &publication,
                                                 const XRMultiviewOwnerState &active) {
            if (const auto live = ValidateXRViewRenderPlan(plan, active.session, active.configuration, active.configurationRevision,
                                                           active.originRevision, active.acquiredImages);
                live.HasError())
                return live;
            if (publication.originRevision != active.originRevision.Value())
                return Result<void>::Failure(MakeError(XRErrors::OriginRevisionStale));
            if (publication.viewRevision != active.configurationRevision.Value())
                return Result<void>::Failure(MakeError(XRErrors::ViewConfigurationStale));
            return Result<void>::Success();
        }

        struct CanvasPoint final {
            double x, y;
        };

        struct CanvasClipEdge final {
            bool xAxis;
            double sign;
            double limit;

            [[nodiscard]] double Distance(const CanvasPoint point) const noexcept {
                return limit - sign * (xAxis ? point.x : point.y);
            }
        };

        /** @brief Clips a convex polygon against one viewport edge without duplicating boundary points. */
        std::optional<std::size_t> ClipCanvasEdge(const std::span<const CanvasPoint> input, const std::span<CanvasPoint> output,
                                                  const CanvasClipEdge edge) noexcept {
            if (input.empty())
                return 0;
            std::size_t count = 0;
            auto previous = input.back();
            auto priorDistance = edge.Distance(previous);
            for (const auto point : input) {
                const auto distance = edge.Distance(point);
                const bool crossing = (priorDistance < 0 && distance > 0) || (priorDistance > 0 && distance < 0);
                const bool inside = distance >= 0;
                const auto emitted = static_cast<std::size_t>(crossing) + static_cast<std::size_t>(inside);
                if (emitted > output.size() - count)
                    return std::nullopt;
                if (crossing) {
                    const auto fraction = priorDistance / (priorDistance - distance);
                    const auto boundary = edge.sign * edge.limit;
                    output[count++] = edge.xAxis ? CanvasPoint{boundary, std::lerp(previous.y, point.y, fraction)}
                                                 : CanvasPoint{std::lerp(previous.x, point.x, fraction), boundary};
                }
                if (inside)
                    output[count++] = point;
                previous = point;
                priorDistance = distance;
            }
            return count;
        }

        /** @brief Measures the narrowest width and, for an unclipped quad, its shortest perspective edge. */
        double CanvasSpan(const std::span<const CanvasPoint> polygon, const bool includeEdgeLength) noexcept {
            if (polygon.size() < 3)
                return 0;
            auto width = std::numeric_limits<double>::infinity();
            for (std::size_t index = 0; index < polygon.size(); ++index) {
                const auto a = polygon[index];
                const auto b = polygon[(index + 1) % polygon.size()];
                const auto dx = b.x - a.x;
                const auto dy = b.y - a.y;
                const auto length = std::hypot(dx, dy);
                if (length == 0) {
                    if (includeEdgeLength)
                        return 0;
                    continue;
                }
                const auto distance = [a, dx, dy](const CanvasPoint point) {
                    return dx * (point.y - a.y) - dy * (point.x - a.x);
                };
                const auto [low, high] = std::ranges::minmax_element(polygon, {}, distance);
                width = std::min(width, (distance(*high) - distance(*low)) / length);
                if (includeEdgeLength)
                    width = std::min(width, length);
            }
            return std::isfinite(width) ? width : 0;
        }

        /** @brief Clips a projected quad to the eye rectangle using at most eight vertices. */
        std::optional<double> VisibleCanvasSpan(const std::array<CanvasPoint, 4> &quad, const double halfWidth,
                                                const double halfHeight) noexcept {
            constexpr std::size_t MaximumIntersectionVertices = 8;
            std::array<CanvasPoint, MaximumIntersectionVertices> polygon, scratch;
            std::ranges::copy(quad, polygon.begin());
            auto count = quad.size();
            const std::array edges{CanvasClipEdge{true, 1, halfWidth}, CanvasClipEdge{true, -1, halfWidth},
                                   CanvasClipEdge{false, 1, halfHeight}, CanvasClipEdge{false, -1, halfHeight}};
            for (const auto edge : edges) {
                const auto clipped = ClipCanvasEdge(std::span{polygon}.first(count), scratch, edge);
                if (!clipped)
                    return std::nullopt;
                count = *clipped;
                std::ranges::copy(std::span{scratch}.first(count), polygon.begin());
            }
            return CanvasSpan(std::span{polygon}.first(count), false);
        }

        /** @brief Measures the visible quad in the exact eye rectangle. A depth crossing suppresses the eye before perspective division. */
        [[nodiscard]] Result<float> MeasureCanvasEye(const Math::Mat4 &clip, const XRRenderRectangle &rect,
                                                     const Math::ClipDepthRange depthRange) {
            using Measure = Result<float>;
            constexpr std::array corners{Math::Vec4{-0.5F, 0.5F, 0, 1}, Math::Vec4{0.5F, 0.5F, 0, 1}, Math::Vec4{0.5F, -0.5F, 0, 1},
                                         Math::Vec4{-0.5F, -0.5F, 0, 1}};
            std::array<Math::Vec4, corners.size()> points;
            std::ranges::transform(corners, points.begin(), [&clip](const auto corner) {
                return Math::TransformHomogeneous(clip, corner);
            });
            if (!std::ranges::all_of(points, [](const auto point) {
                return Math::IsFinite(point);
            }))
                return Measure::Failure(MakeError(XRErrors::ViewPlanInvalid));
            if (std::ranges::any_of(points, [depthRange](const auto point) {
                const auto nearDepth = depthRange == Math::ClipDepthRange::ZeroToOne ? 0.0F : -point.w;
                return point.w <= 0 || point.z < nearDepth || point.z > point.w;
            }))
                return Measure::Success(0);
            const auto halfWidth = rect.width * 0.5;
            const auto halfHeight = rect.height * 0.5;
            std::array<CanvasPoint, points.size()> projected;
            std::ranges::transform(points, projected.begin(), [halfWidth, halfHeight](const auto point) {
                return CanvasPoint{halfWidth * point.x / point.w, halfHeight * point.y / point.w};
            });
            const auto span = CanvasSpan(projected, true);
            if (std::ranges::all_of(projected, [halfWidth, halfHeight](const CanvasPoint point) {
                return std::abs(point.x) <= halfWidth && std::abs(point.y) <= halfHeight;
            }))
                return Measure::Success(static_cast<float>(span));
            const auto visible = VisibleCanvasSpan(projected, halfWidth, halfHeight);
            if (!visible)
                return Measure::Failure(MakeError(XRErrors::CapacityExceeded));
            return Measure::Success(static_cast<float>(std::min(span, *visible)));
        }
    }  // namespace

    /** @copydoc XRMultiviewScene::Create */
    Result<XRMultiviewScene> XRMultiviewScene::Create(const XRMultiviewSceneInput &input, const XRMultiviewOwnerState &active) {
        if (const auto live = ValidateOwner(input.plan, input.publication, active); live.HasError())
            return Result<XRMultiviewScene>::Failure(live.ErrorValue());
        const auto source = input.plan.Views();
        if (input.referenceToWorld.size() != source.size())
            return Result<XRMultiviewScene>::Failure(MakeError(XRErrors::CoordinateSpaceIncompatible));
        std::array<Render::MultiviewSceneView, XRHardLimits::MaximumViews> views;
        for (std::size_t index = 0; index < source.size(); ++index) {
            const auto projection = Projection(source[index].projection, input.nearPlane, input.farPlane, input.depthRange);
            if (projection.HasError())
                return Result<XRMultiviewScene>::Failure(projection.ErrorValue());
            const auto worldToView = WorldToView(source[index], input.referenceToWorld[index], active);
            if (worldToView.HasError())
                return Result<XRMultiviewScene>::Failure(worldToView.ErrorValue());
            views[index] = {.index = static_cast<std::uint32_t>(index),
                            .worldToClip = Math::Multiply(projection.Value(), worldToView.Value()),
                            .depthRange = input.depthRange};
        }
        auto scene =
            Render::MultiviewSceneSnapshot::Extract(input.publication, active.renderer, std::span{views}.first(source.size()),
                                                    input.instances, EffectiveCapabilities(input.plan, active), input.policy, input.limits);
        if (scene.HasError())
            return Result<XRMultiviewScene>::Failure(scene.ErrorValue());
        return Result<XRMultiviewScene>::Success(XRMultiviewScene{input.plan, std::move(scene).Value()});
    }

    XRMultiviewScene::XRMultiviewScene(const XRViewRenderPlan &plan, Render::MultiviewSceneSnapshot scene) noexcept
        : plan_(plan), scene_(std::move(scene)) {}

    /** @copydoc XRMultiviewScene::Plan */
    const XRViewRenderPlan &XRMultiviewScene::Plan() const noexcept {
        return plan_;
    }

    /** @copydoc XRMultiviewScene::Scene */
    const Render::MultiviewSceneSnapshot &XRMultiviewScene::Scene() const noexcept {
        return scene_;
    }

    /** @copydoc XRMultiviewScene::Validate */
    Result<void> XRMultiviewScene::Validate(const XRMultiviewOwnerState &active) const {
        if (const auto live = ValidateOwner(plan_, scene_.Publication(), active); live.HasError())
            return live;
        return scene_.Validate(active.renderer);
    }

    /** @copydoc XRMultiviewScene::ProjectWorldCanvas */
    Result<XRWorldCanvasProjection> XRMultiviewScene::ProjectWorldCanvas(const XRWorldCanvasDescriptor &canvas,
                                                                         const XRMultiviewOwnerState &active) const {
        using Projection = Result<XRWorldCanvasProjection>;
        if (const auto live = Validate(active); live.HasError())
            return Projection::Failure(live.ErrorValue());
        if (plan_.Configuration().type != XRViewConfigurationType::PrimaryStereo)
            return Projection::Failure(MakeError(XRErrors::OperationUnsupported));
        if (!std::isfinite(canvas.minimumSpanPixels) || canvas.minimumSpanPixels <= 0 ||
            Math::TryInverseAffine(canvas.worldFromCanvas).HasError())
            return Projection::Failure(MakeError(XRErrors::ViewPlanInvalid));

        XRWorldCanvasProjection output{scene_.Publication(), canvas.worldFromCanvas, {}, true};
        for (std::size_t index = 0; index < output.views.size(); ++index) {
            const auto view = plan_.Views()[index].id;
            const auto target = std::ranges::find_if(plan_.Targets(), [view](const auto &entry) {
                return entry.view == view && entry.role == XRExternalRenderTargetRole::ProjectionColor;
            });
            if (target == plan_.Targets().end())
                return Projection::Failure(MakeError(XRErrors::ViewPlanInvalid));
            const auto &eye = scene_.Views()[index];
            const auto matrix = Math::Multiply(eye.worldToClip, canvas.worldFromCanvas);
            const auto measured = MeasureCanvasEye(matrix, target->renderRectangle, eye.depthRange);
            if (measured.HasError())
                return Projection::Failure(measured.ErrorValue());
            output.views[index] = {view, matrix, target->renderRectangle, target->arrayLayer, measured.Value()};
            output.drawable = output.drawable && measured.Value() >= canvas.minimumSpanPixels;
        }
        return Projection::Success(output);
    }

    /** @copydoc XRMultiviewScene::ExtractInstances */
    Result<XRMultiviewScene> XRMultiviewScene::ExtractInstances(const std::span<const Render::MultiviewSceneInstance> instances,
                                                                const XRMultiviewOwnerState &active,
                                                                const Render::MultiviewExecutionPolicy policy,
                                                                const Render::MultiviewExtractionLimits &limits) const {
        if (const auto live = Validate(active); live.HasError())
            return Result<XRMultiviewScene>::Failure(live.ErrorValue());
        auto extracted = Render::MultiviewSceneSnapshot::Extract(scene_.Publication(), active.renderer, scene_.Views(), instances,
                                                                 EffectiveCapabilities(plan_, active), policy, limits);
        if (extracted.HasError())
            return Result<XRMultiviewScene>::Failure(extracted.ErrorValue());
        return Result<XRMultiviewScene>::Success(XRMultiviewScene{plan_, std::move(extracted).Value()});
    }
}  // namespace Horo::XR
