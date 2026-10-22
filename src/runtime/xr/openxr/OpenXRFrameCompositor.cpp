#include "host/OpenXRFrameCompositor.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <limits>

namespace Horo::XR::OpenXRInternal {
    namespace {
        /** @brief Preserves finite operation/native result context without runtime/device identifiers. */
        [[nodiscard]] Error NativeError(const char *operation, XrResult result) {
            return MakeError(result == XR_ERROR_FUNCTION_UNSUPPORTED || result == XR_ERROR_COLOR_SPACE_UNSUPPORTED_FB
                                 ? XRErrors::OperationUnsupported
                                 : XRErrors::OperationUnavailable,
                             std::format("OpenXR {} failed; native result={}", operation, static_cast<std::int32_t>(result)));
        }

        /** @brief Resolves an official prototype without publishing a partially resolved adapter. */
        template <typename T, typename ResolveFunction>
        [[nodiscard]] Result<T> Resolve(const ResolveFunction &getProc, XrInstance instance, const char *name) {
            PFN_xrVoidFunction raw{};
            if (const XrResult result = getProc(instance, name, &raw); XR_FAILED(result) || !raw)
                return Result<T>::Failure(NativeError(name, XR_FAILED(result) ? result : XR_ERROR_FUNCTION_UNSUPPORTED));
            return Result<T>::Success(std::bit_cast<T>(raw));
        }

        /** @brief Validates finite metric depth conventions, including explicitly reversed normalized depth. */
        [[nodiscard]] bool ValidDepth(const NativeProjectionDepth &depth) noexcept {
            return std::isfinite(depth.minimumDepth) && std::isfinite(depth.maximumDepth) && std::isfinite(depth.nearMeters) &&
                   std::isfinite(depth.farMeters) && depth.minimumDepth >= 0 && depth.minimumDepth <= 1 && depth.maximumDepth >= 0 &&
                   depth.maximumDepth <= 1 && depth.minimumDepth < depth.maximumDepth && depth.nearMeters > 0 && depth.farMeters > 0 &&
                   depth.farMeters != depth.nearMeters;
        }
    }  // namespace

    /** @copydoc OpenXRFrameCompositor::OpenXRFrameCompositor */
    OpenXRFrameCompositor::OpenXRFrameCompositor(OpenXRNativeSession &session, XRFrameLifecycle &frames,
                                                 IOpenXRCompositionSpace &spaces) noexcept
        : session_(&session), frames_(&frames), spaces_(&spaces) {}

    /** @copydoc OpenXRFrameCompositor::~OpenXRFrameCompositor */
    OpenXRFrameCompositor::~OpenXRFrameCompositor() {
        HORO_INVARIANT_MSG(thread_ == std::this_thread::get_id(), "OpenXR compositor destruction requires its control thread");
        HORO_INVARIANT_MSG(!nativeFrame_.IsValid(), "OpenXR compositor requires native end or quiescent reset before destruction");
    }

    /** @copydoc OpenXRFrameCompositor::Current */
    Result<NativeSessionBorrow> OpenXRFrameCompositor::Current() const {
        if (thread_ != std::this_thread::get_id())
            return Result<NativeSessionBorrow>::Failure(
                MakeError(XRErrors::OperationIncompatible, "XR compositor requires its control thread."));
        if (!owner_.IsValid() || !wait_ || faulted_)
            return Result<NativeSessionBorrow>::Failure(MakeError(XRErrors::OperationUnavailable));
        return session_->Borrow(owner_);
    }

    /** @copydoc OpenXRFrameCompositor::Bind */
    Result<void> OpenXRFrameCompositor::Bind(NativeCompositorDispatch dispatch, const NativeFramePacingPolicy &pacing,
                                             NativeCompositorColorSpace colorSpace) {
        // Snapshot before foreign loader callbacks can mutate the caller-owned policy.
        const NativeFramePacingPolicy admittedPacing = pacing;
        if (thread_ != std::this_thread::get_id() || nativeFrame_.IsValid() || frames_->Snapshot().phase != XRFramePhase::Idle ||
            owner_.IsValid() || closing_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (!dispatch.getProc || admittedPacing.gpuBudgetNanoseconds <= 0 || admittedPacing.compositorMarginNanoseconds < 0 ||
            admittedPacing.gpuBudgetNanoseconds > std::numeric_limits<std::int64_t>::max() - admittedPacing.compositorMarginNanoseconds ||
            colorSpace >= NativeCompositorColorSpace::Count || admittedPacing.maximumFramesInFlight == 0)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        if (admittedPacing.maximumFramesInFlight != 1)
            return Result<void>::Failure(
                MakeError(XRErrors::OperationUnsupported, "Release-complete XR pacing supports one frame in flight."));
        const XRSessionId owner = session_->Session();
        const auto native = session_->Borrow(owner);
        if (native.HasError())
            return Result<void>::Failure(native.ErrorValue());
        const auto wait = Resolve<PFN_xrWaitFrame>(dispatch.getProc, native.Value().instance, "xrWaitFrame");
        if (wait.HasError())
            return Result<void>::Failure(wait.ErrorValue());
        const auto begin = Resolve<PFN_xrBeginFrame>(dispatch.getProc, native.Value().instance, "xrBeginFrame");
        if (begin.HasError())
            return Result<void>::Failure(begin.ErrorValue());
        const auto end = Resolve<PFN_xrEndFrame>(dispatch.getProc, native.Value().instance, "xrEndFrame");
        if (end.HasError())
            return Result<void>::Failure(end.ErrorValue());
        if (colorSpace != NativeCompositorColorSpace::RuntimeDefault) {
            if (!session_->HasExtension(XR_FB_COLOR_SPACE_EXTENSION_NAME))
                return Result<void>::Failure(
                    MakeError(XRErrors::OperationUnsupported, "Requested compositor primaries require XR_FB_color_space."));
            const auto set = Resolve<PFN_xrSetColorSpaceFB>(dispatch.getProc, native.Value().instance, "xrSetColorSpaceFB");
            if (set.HasError())
                return Result<void>::Failure(set.ErrorValue());
            const XrColorSpaceFB selected =
                colorSpace == NativeCompositorColorSpace::Rec709 ? XR_COLOR_SPACE_REC709_FB : XR_COLOR_SPACE_REC2020_FB;
            const XrResult result = set.Value()(native.Value().session, selected);
            if (XR_FAILED(result))
                return Result<void>::Failure(NativeError("set color space", result));
        }
        owner_ = owner;
        policy_ = admittedPacing;
        wait_ = wait.Value();
        begin_ = begin.Value();
        end_ = end.Value();
        return Result<void>::Success();
    }

    /** @copydoc OpenXRFrameCompositor::Wait */
    Result<XRFrameWaitOutcome> OpenXRFrameCompositor::Wait() {
        using Outcome = Result<XRFrameWaitOutcome>;
        const auto native = Current();
        if (native.HasError())
            return Outcome::Failure(native.ErrorValue());
        if (closing_ || nativeFrame_.IsValid())
            return Outcome::Failure(
                MakeError(XRErrors::OperationUnavailable, "XR frame admission is closed or its single slot is occupied."));
        const auto reservation = frames_->ReserveWait(owner_);
        if (reservation.status != XRFrameStatus::Ok)
            return Outcome::Success(reservation);
        XrFrameState state{XR_TYPE_FRAME_STATE};
        const XrFrameWaitInfo info{XR_TYPE_FRAME_WAIT_INFO};
        const XrResult result = wait_(native.Value().session, &info, &state);
        if (XR_FAILED(result)) {
            static_cast<void>(frames_->CancelWait());
            return Outcome::Failure(NativeError("wait frame", result));
        }
        nativeFrame_ = reservation.frame;
        period_ = state.predictedDisplayPeriod;
        nativeDisplayTime_ = state.predictedDisplayTime;
        const auto prediction = XRRenderPredictionTime::Create(state.predictedDisplayTime);
        // SYNC: a completed native wait always creates begin/end debt, even with unusable prediction evidence.
        const auto recorded =
            frames_->RecordWait(prediction.HasValue() ? prediction.Value() : XRRenderPredictionTime{}, state.shouldRender == XR_TRUE);
        return Outcome::Success(recorded);
    }

    /** @copydoc OpenXRFrameCompositor::ValidateFrame */
    Result<void> OpenXRFrameCompositor::ValidateFrame(const XRFrameId &frame) const {
        if (const auto current = Current(); current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        if (!frame.IsValid() || frame != nativeFrame_ || frame != frames_->Snapshot().frame)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        const auto logical = frames_->ValidateCurrent(frame);
        if (logical != XRFrameStatus::Ok)
            return Result<void>::Failure(
                MakeError(logical == XRFrameStatus::StalePlan ? XRErrors::CapabilityStale : XRErrors::IdentityStale));
        return Result<void>::Success();
    }

    /** @copydoc OpenXRFrameCompositor::Begin */
    Result<void> OpenXRFrameCompositor::Begin(const XRFrameId &frame) {
        if (auto valid = ValidateFrame(frame); valid.HasError())
            return valid;
        if (begun_ || frames_->Snapshot().phase != XRFramePhase::Waited)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        const auto native = Current();
        if (native.HasError())
            return Result<void>::Failure(native.ErrorValue());
        const XrFrameBeginInfo info{XR_TYPE_FRAME_BEGIN_INFO};
        const XrResult result = begin_(native.Value().session, &info);
        if (XR_FAILED(result)) {
            faulted_ = true;
            return Result<void>::Failure(NativeError("begin frame", result));
        }
        begun_ = true;
        if (frames_->Begin(frame) != XRFrameStatus::Ok) {
            faulted_ = true;
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        }
        return Result<void>::Success();
    }

    /** @copydoc OpenXRFrameCompositor::ObservePacing */
    Result<NativeFramePacingObservation> OpenXRFrameCompositor::ObservePacing(const XRFrameId &frame, XRRenderPredictionTime now) const {
        using Outcome = Result<NativeFramePacingObservation>;
        using enum NativeFrameDeadline;
        if (auto valid = ValidateFrame(frame); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        const auto prediction = frames_->Snapshot().predictedDisplayTime;
        const auto reserve = policy_.gpuBudgetNanoseconds + policy_.compositorMarginNanoseconds;
        if (!now.IsValid() || !prediction.IsValid() || period_ <= 0 || reserve >= period_ || prediction.Nanoseconds() <= reserve)
            return Outcome::Failure(
                MakeError(XRErrors::TimeDomainIncompatible, "XR prediction period or converted clock cannot satisfy admitted budgets."));
        const auto deadline = prediction.Nanoseconds() - reserve;
        NativeFrameDeadline status = WithinBudget;
        if (now >= prediction)
            status = DisplayMissed;
        else if (now.Nanoseconds() > deadline)
            status = CpuSubmissionMissed;
        return Outcome::Success({frame, prediction, period_, deadline, deadline - now.Nanoseconds(), status, 1});
    }

    /** @copydoc OpenXRFrameCompositor::SubImage */
    Result<XrSwapchainSubImage> OpenXRFrameCompositor::SubImage(const XRExternalRenderTargetDescriptor &target,
                                                                std::span<const NativeProjectionTarget> owners) const {
        using Outcome = Result<XrSwapchainSubImage>;
        if (frames_->ValidateReleasedImage(nativeFrame_, target.image) != XRFrameStatus::Ok)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        const auto found = std::ranges::find(owners, target.image, &NativeProjectionTarget::image);
        if (found == owners.end() || !found->owner)
            return Outcome::Failure(MakeError(XRErrors::ExternalTargetInvalid));
        const auto borrowed = found->owner->BorrowReleased(target.image);
        if (borrowed.HasError())
            return Outcome::Failure(borrowed.ErrorValue());
        const auto &allocation = borrowed.Value().allocation;
        const auto role =
            target.role == XRExternalRenderTargetRole::ProjectionDepth ? NativeSwapchainRole::Depth : NativeSwapchainRole::Color;
        if (constexpr auto MaximumNative = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
            allocation.target != target.target || allocation.role != role || allocation.texture.format != target.format ||
            allocation.texture.extent != target.allocationExtent || allocation.texture.layerCount != target.arrayLayerCount ||
            allocation.texture.sampleCount != target.sampleCount || !Render::HasTextureUsage(allocation.texture.usage, target.usage) ||
            target.arrayLayer >= target.arrayLayerCount || !target.renderRectangle.FitsWithin(target.allocationExtent) ||
            target.allocationExtent.width > MaximumNative || target.allocationExtent.height > MaximumNative)
            return Outcome::Failure(MakeError(XRErrors::ExternalTargetInvalid));
        const auto &rectangle = target.renderRectangle;
        return Outcome::Success({borrowed.Value().swapchain,
                                 {{static_cast<std::int32_t>(rectangle.x), static_cast<std::int32_t>(rectangle.y)},
                                  {static_cast<std::int32_t>(rectangle.width), static_cast<std::int32_t>(rectangle.height)}},
                                 target.arrayLayer});
    }

    /** @copydoc OpenXRFrameCompositor::ValidateDepthPlan */
    Result<void> OpenXRFrameCompositor::ValidateDepthPlan(const ProjectionEncoding &encoding) const {
        if (encoding.depth && !ValidDepth(*encoding.depth))
            return Result<void>::Failure(
                MakeError(XRErrors::OperationInvalid, "Projection depth range or metric clip planes are invalid."));
        if (encoding.depth && !session_->HasExtension(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME))
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
        const auto depthCount = std::ranges::count(encoding.plan.Targets(), XRExternalRenderTargetRole::ProjectionDepth,
                                                   &XRExternalRenderTargetDescriptor::role);
        const auto requiredDepth = encoding.depth ? encoding.plan.Views().size() : 0;
        return static_cast<std::size_t>(depthCount) == requiredDepth ? Result<void>::Success()
                                                                     : Result<void>::Failure(MakeError(XRErrors::ViewPlanInvalid));
    }

    /** @copydoc OpenXRFrameCompositor::ValidateProjection */
    Result<void> OpenXRFrameCompositor::ValidateProjection(const XRFrameId &frame, const ProjectionEncoding &encoding) const {
        if (auto valid = ValidateFrame(frame); valid.HasError())
            return valid;
        const auto &plan = encoding.plan;
        const auto targets = encoding.targets;
        if (const auto snapshot = frames_->Snapshot();
            !begun_ || frames_->ValidateEnd(frame, 1) != XRFrameStatus::Ok || plan.Configuration().id != frame.configuration ||
            plan.Configuration().session != frame.session || plan.PredictedDisplayTime() != snapshot.predictedDisplayTime ||
            plan.Views().size() != snapshot.locatedViews || targets.empty() || targets.size() != snapshot.acquiredImages ||
            targets.size() > XRFrameHardLimits::MaximumImages)
            return Result<void>::Failure(MakeError(XRErrors::ViewPlanInvalid));
        // First production profile admits complete primary opaque stereo; no silent truncation or blend fallback.
        if (plan.Configuration().type != XRViewConfigurationType::PrimaryStereo || plan.Views().size() != 2 ||
            plan.Configuration().blendMode != XREnvironmentBlendMode::Opaque)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
        for (std::size_t index = 0; index < targets.size(); ++index) {
            const auto &target = targets[index];
            const bool known = std::ranges::any_of(plan.Targets(), [&](const auto &item) {
                return item.image == target.image;
            });
            const bool duplicate =
                std::ranges::find(targets.first(index), target.image, &NativeProjectionTarget::image) != targets.first(index).end();
            if (!target.owner || !target.image.IsValid() || !known || duplicate)
                return Result<void>::Failure(MakeError(XRErrors::ExternalTargetInvalid));
        }
        return ValidateDepthPlan(encoding);
    }

    /** @copydoc OpenXRFrameCompositor::EncodeDepth */
    Result<void> OpenXRFrameCompositor::EncodeDepth(const ProjectionEncoding &encoding, const XRExternalRenderTargetDescriptor &color,
                                                    XrCompositionLayerDepthInfoKHR &output) const {
        const auto target = std::ranges::find_if(encoding.plan.Targets(), [&](const auto &item) {
            return item.view == color.view && item.role == XRExternalRenderTargetRole::ProjectionDepth;
        });
        if (target == encoding.plan.Targets().end() || target->renderRectangle.width != color.renderRectangle.width ||
            target->renderRectangle.height != color.renderRectangle.height)
            return Result<void>::Failure(MakeError(XRErrors::ExternalTargetInvalid));
        const auto sub = SubImage(*target, encoding.targets);
        if (sub.HasError())
            return Result<void>::Failure(sub.ErrorValue());
        const auto &depth = *encoding.depth;
        output = {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR,
                  nullptr,
                  sub.Value(),
                  depth.minimumDepth,
                  depth.maximumDepth,
                  depth.nearMeters,
                  depth.farMeters};
        return Result<void>::Success();
    }

    /** @copydoc OpenXRFrameCompositor::EncodeView */
    Result<XrSpace> OpenXRFrameCompositor::EncodeView(const ProjectionEncoding &encoding, const XRViewDescriptor &view,
                                                      XrCompositionLayerProjectionView &output,
                                                      XrCompositionLayerDepthInfoKHR &depth) const {
        using Outcome = Result<XrSpace>;
        const auto pose = XRPoseSample::Create(view.pose, nativeFrame_.session, view.pose.target.worldOriginRevision);
        if (pose.HasError())
            return Outcome::Failure(pose.ErrorValue());
        const auto space = spaces_->Borrow(view.pose.target);
        if (space.HasError())
            return Outcome::Failure(space.ErrorValue());
        if (space.Value() == XR_NULL_HANDLE)
            return Outcome::Failure(MakeError(XRErrors::CoordinateSpaceIncompatible));
        const auto color = std::ranges::find_if(encoding.plan.Targets(), [&](const auto &target) {
            return target.view == view.id && target.role == XRExternalRenderTargetRole::ProjectionColor;
        });
        if (color == encoding.plan.Targets().end())
            return Outcome::Failure(MakeError(XRErrors::ExternalTargetInvalid));
        const auto sub = SubImage(*color, encoding.targets);
        if (sub.HasError())
            return Outcome::Failure(sub.ErrorValue());
        const auto &components = pose.Value().Components();
        if (!components.orientation.value || !components.positionMeters.value)
            return Outcome::Failure(MakeError(XRErrors::PoseInvalid));
        const auto &q = *components.orientation.value;
        const auto &p = *components.positionMeters.value;
        output = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
                  nullptr,
                  {{q.x, q.y, q.z, q.w}, {p.x, p.y, p.z}},
                  {std::atan(view.projection.leftTangent), std::atan(view.projection.rightTangent), std::atan(view.projection.upTangent),
                   std::atan(view.projection.downTangent)},
                  sub.Value()};
        if (encoding.depth) {
            if (const auto encoded = EncodeDepth(encoding, *color, depth); encoded.HasError())
                return Outcome::Failure(encoded.ErrorValue());
            output.next = &depth;
        }
        return Outcome::Success(space.Value());
    }

    /** @copydoc OpenXRFrameCompositor::EndProjection */
    Result<void> OpenXRFrameCompositor::EndProjection(const XRFrameId &frame, const XRViewRenderPlan &plan,
                                                      std::span<const NativeProjectionTarget> targets, const NativeProjectionDepth *depth) {
        const ProjectionEncoding encoding{plan, targets, depth};
        if (const auto valid = ValidateProjection(frame, encoding); valid.HasError())
            return valid;
        // PERF: fixed bounded native encoding storage; no frame allocation or native discovery.
        std::array<XrCompositionLayerProjectionView, XRHardLimits::MaximumViews> views{};
        std::array<XrCompositionLayerDepthInfoKHR, XRHardLimits::MaximumViews> depths{};
        XrSpace reference{XR_NULL_HANDLE};
        for (std::size_t index = 0; index < plan.Views().size(); ++index) {
            const auto space = EncodeView(encoding, plan.Views()[index], views[index], depths[index]);
            if (space.HasError())
                return Result<void>::Failure(space.ErrorValue());
            if (reference != XR_NULL_HANDLE && reference != space.Value())
                return Result<void>::Failure(MakeError(XRErrors::CoordinateSpaceIncompatible));
            reference = space.Value();
        }
        const XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION,
                                                 nullptr,
                                                 0,
                                                 reference,
                                                 static_cast<std::uint32_t>(plan.Views().size()),
                                                 views.data()};
        // OpenXR defines this native base-header view for projection layer dispatch.
        static_assert(sizeof(const XrCompositionLayerBaseHeader *) == sizeof(&layer));
        return EndNative(frame, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, std::bit_cast<const XrCompositionLayerBaseHeader *>(&layer));
    }

    /** @copydoc OpenXRFrameCompositor::EndNative */
    Result<void> OpenXRFrameCompositor::EndNative(const XRFrameId &frame, XrEnvironmentBlendMode blend,
                                                  const XrCompositionLayerBaseHeader *layer) {
        const auto native = Current();
        if (native.HasError())
            return Result<void>::Failure(native.ErrorValue());
        // SYNC: cleanup must echo the completed native wait even when its prediction could not enter Horo's time contract.
        const XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO, nullptr, nativeDisplayTime_, blend, layer ? 1U : 0U, layer ? &layer : nullptr};
        const XrResult result = end_(native.Value().session, &info);
        if (XR_FAILED(result)) {
            faulted_ = true;
            return Result<void>::Failure(NativeError("end frame", result));
        }
        const auto ended = frames_->End(frame, layer ? 1U : 0U);
        nativeFrame_ = {};
        begun_ = false;
        period_ = 0;
        nativeDisplayTime_ = 0;
        return ended == XRFrameStatus::Ok ? Result<void>::Success() : Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
    }

    /** @copydoc OpenXRFrameCompositor::EndEmpty */
    Result<void> OpenXRFrameCompositor::EndEmpty(const XRFrameId &frame) {
        if (auto valid = ValidateFrame(frame); valid.HasError())
            return valid;
        if (!begun_ || frames_->ValidateEnd(frame, 0) != XRFrameStatus::Ok)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        return EndNative(frame, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, nullptr);
    }

    /** @copydoc OpenXRFrameCompositor::BeginShutdown */
    Result<void> OpenXRFrameCompositor::BeginShutdown() {
        if (thread_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible, "XR shutdown requires its control thread."));
        closing_ = true;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRFrameCompositor::ResetAfterQuiescence */
    Result<void> OpenXRFrameCompositor::ResetAfterQuiescence() {
        if (thread_ != std::this_thread::get_id())
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible, "XR recovery requires its control thread."));
        owner_ = {};
        nativeFrame_ = {};
        wait_ = nullptr;
        begin_ = nullptr;
        end_ = nullptr;
        begun_ = false;
        faulted_ = false;
        period_ = 0;
        nativeDisplayTime_ = 0;
        return Result<void>::Success();
    }
}  // namespace Horo::XR::OpenXRInternal
