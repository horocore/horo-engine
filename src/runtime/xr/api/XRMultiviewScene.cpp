#include "Horo/XR/XRMultiviewScene.h"

#include <algorithm>
#include <array>
#include <cmath>

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
            std::size_t depthCount = 0;
            for (const auto &target : targets) {
                if (const auto &baseline = target.role == XRExternalRenderTargetRole::ProjectionDepth ? *depth : first;
                    target.image != baseline.image || target.format != baseline.format || target.renderRectangle != first.renderRectangle ||
                    target.allocationExtent != first.allocationExtent || target.arrayLayerCount != first.arrayLayerCount ||
                    target.sampleCount != first.sampleCount)
                    return false;
                if (target.role == XRExternalRenderTargetRole::ProjectionDepth) {
                    ++depthCount;
                    const auto color = std::ranges::find_if(targets, [&target](const auto &candidate) {
                        return candidate.view == target.view && candidate.role == XRExternalRenderTargetRole::ProjectionColor;
                    });
                    if (color == targets.end() || color->arrayLayer != target.arrayLayer)
                        return false;
                }
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
