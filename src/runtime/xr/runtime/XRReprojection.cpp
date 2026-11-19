#include "Horo/XR/XRFrameLifecycle.h"

#include <algorithm>
#include <cmath>

namespace Horo::XR {
    namespace {
        /** @brief Rejects a missing fixed bound or mixed fixed and observed evidence policies. */
        bool ValidCadence(const XRReprojectionPolicy &policy) noexcept {
            switch (policy.cadence) {
                case XRReprojectionCadenceMode::FixedInterval:
                    return policy.maximumPredictionIntervalNanoseconds > 0;
                case XRReprojectionCadenceMode::ObservedNativePeriods:
                    return policy.maximumPredictionIntervalNanoseconds == 0;
            }
            return false;
        }

        /** @brief This envelope is deliberately narrower than any one native extension's full formats. */
        bool CompatiblePolicy(const XRReprojectionPolicy &policy) noexcept {
            using enum Render::RenderTextureFormat;
            return ValidCadence(policy) && policy.motion == XRReprojectionMotionConvention::CurrentNdcMinusPreviousNdcXYZ &&
                   (policy.motionFormat == Rgba16Float || policy.motionFormat == Rgba32Float) &&
                   (policy.depthFormat == Depth16Unorm || policy.depthFormat == Depth32Float);
        }

        /** @brief Verifies every active extent before frame admission can index the policy. */
        bool ValidViewExtents(const XRReprojectionPolicy &policy, const XRFrameLimits limits) noexcept {
            if (policy.viewCount == 0 || policy.viewCount > limits.maximumViews || policy.viewCount > policy.viewExtents.size())
                return false;
            return std::ranges::all_of(std::span{policy.viewExtents}.first(policy.viewCount), &Render::FramebufferExtent::IsValid);
        }

        /** @brief Array storage must contain the exact rectangles of all admitted views. */
        bool CoversViewExtents(const XRReprojectionPolicy &policy, const Render::FramebufferExtent allocation) noexcept {
            return std::ranges::all_of(std::span{policy.viewExtents}.first(policy.viewCount), [&](const auto extent) {
                return allocation.width >= extent.width && allocation.height >= extent.height;
            });
        }

        /** @brief Rejects invalid camera movement without silently substituting an identity delta. */
        bool ValidApplicationDelta(const std::optional<XRReprojectionSpaceDelta> &delta) noexcept {
            if (!delta || !Math::IsFinite(delta->translationMeters))
                return false;
            const auto &q = delta->rotation;
            return Math::IsFinite(q) && std::abs((q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w) - 1.0F) < 0.001F;
        }

        /** @brief Validates normalized finite depth and metric endpoint semantics. */
        bool ValidDepth(const XRReprojectionViewInput &input) noexcept {
            return std::isfinite(input.minimumDepth) && std::isfinite(input.maximumDepth) && input.minimumDepth >= 0.0F &&
                   input.minimumDepth < input.maximumDepth && input.maximumDepth <= 1.0F && std::isfinite(input.nearMeters) &&
                   std::isfinite(input.farMeters) && input.nearMeters > 0.0F && input.farMeters > 0.0F &&
                   input.nearMeters != input.farMeters;
        }

        /** @brief Compares layout/format independently of the necessarily different current and previous acquisitions. */
        bool CompatibleProjectionTarget(const XRExternalRenderTargetDescriptor &a, const XRExternalRenderTargetDescriptor &b) noexcept {
            return a.role == b.role && a.view == b.view && a.target == b.target && a.format == b.format && a.usage == b.usage &&
                   a.allocationExtent == b.allocationExtent && a.renderRectangle == b.renderRectangle && a.arrayLayer == b.arrayLayer &&
                   a.arrayLayerCount == b.arrayLayerCount && a.sampleCount == b.sampleCount;
        }

        /** @brief Fences projection layout, tracking reference and camera validity across the predecessor boundary. */
        bool CompatibleHistory(const XRViewRenderPlan &current, const XRViewRenderPlan &previous) noexcept {
            const auto &now = current.Configuration();
            const auto &before = previous.Configuration();
            if (now.id != before.id || now.revision != before.revision || now.type != before.type || now.blendMode != before.blendMode ||
                current.Views().size() != previous.Views().size() || current.Targets().size() != previous.Targets().size())
                return false;
            if (!std::ranges::equal(current.Targets(), previous.Targets(), CompatibleProjectionTarget))
                return false;
            return std::ranges::equal(current.Views(), previous.Views(), [](const auto &a, const auto &b) {
                return a.id == b.id && a.role == b.role && a.projection == b.projection && a.pose.source == b.pose.source &&
                       a.pose.target == b.pose.target && a.pose.components.positionMeters.value && a.pose.components.orientation.value &&
                       b.pose.components.positionMeters.value && b.pose.components.orientation.value;
            });
        }

        /** @brief Checks that every submitted view is expressed in the observed native projection space. */
        bool UsesReference(const XRViewRenderPlan &plan, const XRPoseSample &reference) noexcept {
            return std::ranges::all_of(plan.Views(), [&](const auto &view) {
                return view.pose.target == reference.Source();
            });
        }

        /** @brief Allows both exact quaternion encodings of the same rotation without inventing a tolerance. */
        bool SameRotation(const Math::Quaternion a, const Math::Quaternion b) noexcept {
            return a == b || a == Math::Quaternion{-b.x, -b.y, -b.z, -b.w};
        }

        /** @brief Proves that the app-controlled projection reference did not move between these native predictions. */
        bool UnchangedReference(const XRViewRenderPlan &current, const std::optional<XRPoseSample> &now, const XRViewRenderPlan &previous,
                                const std::optional<XRPoseSample> &before) noexcept {
            if (!now || !before || now->Source() != before->Source() || now->Target() != before->Target() ||
                !UsesReference(current, *now) || !UsesReference(previous, *before))
                return false;
            return now->Components().positionMeters.value == before->Components().positionMeters.value &&
                   SameRotation(*now->Components().orientation.value, *before->Components().orientation.value);
        }

        /** @brief Requires complete zero-unresolved coverage of both actual released outputs for every view. */
        XRReprojectionStatus ValidateCoverage(const XRFrameId &frame, const XRViewRenderPlan &plan,
                                              const XRReprojectionInputs &inputs) noexcept {
            using enum XRReprojectionStatus;
            if (!inputs.coverage)
                return CoverageUnavailable;
            const auto &coverage = *inputs.coverage;
            if (coverage.session != frame.session || coverage.frameSequence != frame.sequence ||
                coverage.prediction != plan.PredictedDisplayTime() || coverage.previousFrameSequence != inputs.previousFrameSequence ||
                coverage.previousPrediction != inputs.previousPrediction || coverage.views.size() != inputs.views.size())
                return CoverageRejected;
            for (std::size_t eye = 0; eye < inputs.views.size(); ++eye) {
                const auto &input = inputs.views[eye];
                const auto &sample = coverage.views[eye];
                const auto expected = std::uint64_t{input.motion.rectangle.width} * input.motion.rectangle.height;
                if (sample.view != input.view || sample.motion != input.motion || sample.depth != input.depth ||
                    sample.classifiedSamples != expected || sample.unresolvedSamples != 0)
                    return CoverageRejected;
            }
            return Ready;
        }

        /** @brief The same physical subresource must never provide two distinct eye observations. */
        bool Aliases(const XRReprojectionSubImage &a, const XRReprojectionSubImage &b) noexcept {
            return a.image == b.image && a.arrayLayer == b.arrayLayer;
        }
    }  // namespace

    /** @copydoc XRFrameLifecycle::ValidateReprojectionPolicy */
    XRFrameStatus XRFrameLifecycle::ValidateReprojectionPolicy(const std::optional<XRReprojectionPolicy> &candidate,
                                                               const XRFrameLimits limits) const noexcept {
        const auto plan = sessions_->AcceptedPlan();
        const bool enabled = plan && plan->Decision(XRCapability::SpaceWarp).state == XRFeatureDecisionState::OptionalEnabled;
        if (!enabled)
            return candidate ? XRFrameStatus::Unsupported : XRFrameStatus::Ok;
        if (!candidate || !CompatiblePolicy(*candidate) || !ValidViewExtents(*candidate, limits))
            return XRFrameStatus::InvalidInput;
        return limits.maximumImages >= 3 ? XRFrameStatus::Ok : XRFrameStatus::CapacityExceeded;
    }

    /** @copydoc XRFrameLifecycle::ValidateReprojectionImage */
    XRFrameStatus XRFrameLifecycle::ValidateReprojectionImage(const XRReprojectionImage &image) const noexcept {
        if (!reprojection_)
            return XRFrameStatus::Unsupported;
        const auto &allocation = image.allocation;
        if (image.kind >= XRReprojectionImageKind::Count || !allocation.IsValid() ||
            allocation.dimension != Render::RenderTextureDimension::TwoD || allocation.sampleCount != 1 || allocation.mipCount != 1 ||
            allocation.layerCount < reprojection_->viewCount || allocation.layerCount > XRViewRenderHardLimits::MaximumArrayLayers)
            return XRFrameStatus::InvalidInput;
        const auto expected = image.kind == XRReprojectionImageKind::Motion ? reprojection_->motionFormat : reprojection_->depthFormat;
        const auto required = Render::RenderTextureUsage::RenderAttachment | Render::RenderTextureUsage::Sampled;
        if (allocation.format != expected || !Render::HasTextureUsage(allocation.usage, required) ||
            !CoversViewExtents(*reprojection_, allocation.extent))
            return XRFrameStatus::InvalidInput;
        return XRFrameStatus::Ok;
    }

    /** @copydoc XRFrameLifecycle::AcquireReprojectionImage */
    XRFrameStatus XRFrameLifecycle::AcquireReprojectionImage(const XRFrameId &frame, const XRReprojectionImage &image) noexcept {
        if (const auto status = ValidateCurrent(frame); status != XRFrameStatus::Ok)
            return status;
        if (const auto status = ValidateReprojectionImage(image); status != XRFrameStatus::Ok)
            return status;
        if (const auto status = Acquire(frame, image.image); status != XRFrameStatus::Ok)
            return status;
        reprojectionImages_[current_.acquiredImages - 1] = image;
        return XRFrameStatus::Ok;
    }

    /** @copydoc XRFrameLifecycle::ValidateReprojectionSubmission */
    XRFrameStatus XRFrameLifecycle::ValidateReprojectionSubmission(const XRFrameId &frame,
                                                                   const std::span<const XRSwapchainImageId> projection,
                                                                   const XRReprojectionImage &motion,
                                                                   const XRReprojectionImage &depth) const noexcept {
        using enum XRFrameStatus;
        if (const auto status = ValidateSubmission(frame, projection); status != Ok)
            return status;
        if (!reprojection_)
            return Unsupported;
        if (limits_.maximumImages - current_.acquiredImages < 2)
            return CapacityExceeded;
        if (motion.kind != XRReprojectionImageKind::Motion || depth.kind != XRReprojectionImageKind::Depth ||
            motion.allocation.extent != depth.allocation.extent || motion.allocation.layerCount != depth.allocation.layerCount ||
            current_.locatedViews != reprojection_->viewCount)
            return InvalidInput;
        if (motion.image.target.slot.index == depth.image.target.slot.index)
            return Duplicate;
        for (const auto *input : {&motion, &depth}) {
            if (!input->image.IsValid())
                return InvalidInput;
            if (input->image.target.session != frame.session)
                return StaleSession;
            if (std::ranges::any_of(projection, [&](const auto &image) {
                return image.target.slot.index == input->image.target.slot.index;
            }))
                return Duplicate;
            if (const auto status = ValidateReprojectionImage(*input); status != Ok)
                return status;
        }
        if (std::ranges::any_of(std::span{reprojectionImages_}.first(current_.acquiredImages), [](const auto &registered) {
            return registered.has_value();
        }))
            return Duplicate;
        return Ok;
    }

    /** @copydoc XRFrameLifecycle::RecordReprojectionImage */
    void XRFrameLifecycle::RecordReprojectionImage(const XRReprojectionImage &image) noexcept {
        const auto index = current_.acquiredImages++;
        images_[index] = image.image;
        reprojectionImages_[index] = image;
        released_[index] = false;
    }

    /** @copydoc XRFrameLifecycle::SubmitReprojection */
    XRFrameStatus XRFrameLifecycle::SubmitReprojection(const XRFrameId &frame, const std::span<const XRSwapchainImageId> projection,
                                                       const XRReprojectionImage &motion, const XRReprojectionImage &depth) noexcept {
        if (const auto status = ValidateReprojectionSubmission(frame, projection, motion, depth); status != XRFrameStatus::Ok)
            return status;
        RecordReprojectionImage(motion);
        RecordReprojectionImage(depth);
        submitted_ = true;
        current_.phase = XRFramePhase::RendererSubmitted;
        return XRFrameStatus::Ok;
    }

    /** @copydoc XRFrameLifecycle::ValidateProjectionEnd */
    XRFrameStatus XRFrameLifecycle::ValidateProjectionEnd(const XRFrameId &frame, const XRViewRenderPlan &plan) const noexcept {
        if (const auto status = ValidateEnd(frame, 1); status != XRFrameStatus::Ok)
            return status;
        if (plan.Configuration().session != frame.session || plan.Configuration().id != frame.configuration ||
            plan.PredictedDisplayTime() != current_.predictedDisplayTime || plan.Views().size() != current_.locatedViews)
            return XRFrameStatus::StalePlan;
        for (const auto &target : plan.Targets()) {
            if (const auto status = ValidateReleasedImage(frame, target.image); status != XRFrameStatus::Ok)
                return status;
            if (reprojectionImages_[FindImage(target.image)])
                return XRFrameStatus::IncompleteSubmission;
        }
        for (std::uint32_t index = 0; index < current_.acquiredImages; ++index) {
            if (!reprojectionImages_[index] && std::ranges::none_of(plan.Targets(), [&](const auto &target) {
                return target.image == images_[index];
            }))
                return XRFrameStatus::IncompleteSubmission;
        }
        return XRFrameStatus::Ok;
    }

    /** @copydoc XRFrameLifecycle::EndProjection */
    XRFrameStatus XRFrameLifecycle::EndProjection(const XRFrameId &frame, const XRViewRenderPlan &plan) noexcept {
        if (const auto status = ValidateProjectionEnd(frame, plan); status != XRFrameStatus::Ok)
            return status;
        if (reprojection_)
            previousProjection_.emplace(ProjectionHistory{plan, currentPeriod_, currentReference_});
        lastEnded_ = frame;
        ClearFrame();
        return XRFrameStatus::Ok;
    }

    /** @copydoc XRFrameLifecycle::RecordReprojectionReference */
    XRFrameStatus XRFrameLifecycle::RecordReprojectionReference(const XRFrameId &frame, const XRPoseSample &reference) noexcept {
        if (const auto valid = ValidateCurrent(frame); valid != XRFrameStatus::Ok)
            return valid;
        if (!reprojection_ || reprojection_->cadence != XRReprojectionCadenceMode::ObservedNativePeriods)
            return XRFrameStatus::Unsupported;
        if (current_.phase != XRFramePhase::ImagesAcquired)
            return XRFrameStatus::OutOfOrder;
        if (currentReference_)
            return XRFrameStatus::Duplicate;
        const auto source = reference.Source().kind;
        if (reference.Session() != frame.session || (source != XRSpaceKind::Local && source != XRSpaceKind::Stage) ||
            reference.Target().kind != XRSpaceKind::World || reference.Purpose() != XRPosePurpose::PresentationPrediction ||
            reference.Time().renderPrediction != current_.predictedDisplayTime)
            return XRFrameStatus::InvalidInput;
        const auto &components = reference.Components();
        if (components.positionMeters.validity != XRPoseComponentValidity::Tracked ||
            components.orientation.validity != XRPoseComponentValidity::Tracked || components.loss != XRTrackingLossState::None)
            return XRFrameStatus::InvalidInput;
        currentReference_ = reference;
        return XRFrameStatus::Ok;
    }

    /** @copydoc XRFrameLifecycle::MatchesReprojectionImage */
    bool XRFrameLifecycle::MatchesReprojectionImage(const XRFrameId &frame, const XRReprojectionSubImage &input,
                                                    const XRReprojectionImageKind kind,
                                                    const Render::FramebufferExtent extent) const noexcept {
        if (ValidateReleasedImage(frame, input.image) != XRFrameStatus::Ok)
            return false;
        const auto &registered = reprojectionImages_[FindImage(input.image)];
        return registered && registered->kind == kind && input.arrayLayer < registered->allocation.layerCount &&
               input.rectangle.FitsWithin(registered->allocation.extent) && input.rectangle.width == extent.width &&
               input.rectangle.height == extent.height;
    }

    /** @copydoc XRFrameLifecycle::ValidateReprojection */
    XRReprojectionStatus XRFrameLifecycle::ValidateReprojection(const XRFrameId &frame, const XRViewRenderPlan &plan,
                                                                const XRReprojectionInputs &inputs) const noexcept {
        using enum XRReprojectionStatus;
        if (ValidateProjectionEnd(frame, plan) != XRFrameStatus::Ok)
            return ProjectionInvalid;
        if (!reprojection_)
            return BaselineProjection;
        if (!previousProjection_ || frame.sequence <= 1 || lastEnded_.sequence != frame.sequence - 1 ||
            inputs.previousFrameSequence != lastEnded_.sequence ||
            inputs.previousPrediction != previousProjection_->plan.PredictedDisplayTime())
            return HistoryUnavailable;
        const bool observedCadence = reprojection_->cadence == XRReprojectionCadenceMode::ObservedNativePeriods;
        const auto maximumInterval = observedCadence ? std::min(currentPeriod_, previousProjection_->period).ToNanoseconds()
                                                     : reprojection_->maximumPredictionIntervalNanoseconds;
        const auto interval = plan.PredictedDisplayTime().Nanoseconds() - inputs.previousPrediction.Nanoseconds();
        if (interval <= 0 || interval > maximumInterval || !CompatibleHistory(plan, previousProjection_->plan))
            return HistoryUnavailable;
        if (observedCadence && !UnchangedReference(plan, currentReference_, previousProjection_->plan, previousProjection_->reference))
            return HistoryUnavailable;
        if (!ValidApplicationDelta(inputs.applicationDelta) || inputs.views.size() != plan.Views().size() ||
            inputs.views.size() != reprojection_->viewCount)
            return InputsInvalid;
        if (observedCadence && (inputs.applicationDelta->translationMeters != Math::Vec3{} ||
                                !SameRotation(inputs.applicationDelta->rotation, Math::Quaternion::Identity())))
            return InputsInvalid;
        for (std::size_t i = 0; i < inputs.views.size(); ++i) {
            const auto &view = inputs.views[i];
            const auto extent = reprojection_->viewExtents[i];
            if (view.view != plan.Views()[i].id || inputs.applicationDelta->reference != plan.Views()[i].pose.target || !ValidDepth(view) ||
                !MatchesReprojectionImage(frame, view.motion, XRReprojectionImageKind::Motion, extent) ||
                !MatchesReprojectionImage(frame, view.depth, XRReprojectionImageKind::Depth, extent))
                return InputsInvalid;
            if (std::ranges::any_of(inputs.views.first(i), [&](const auto &prior) {
                return Aliases(view.motion, prior.motion) || Aliases(view.depth, prior.depth);
            }))
                return InputsInvalid;
        }
        return ValidateCoverage(frame, plan, inputs);
    }
}  // namespace Horo::XR
