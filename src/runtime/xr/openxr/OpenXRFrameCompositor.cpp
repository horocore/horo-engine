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

        /** @brief Preserves the view ordinals required by the OpenXR quad presentation ABI. */
        [[nodiscard]] bool HasNativeQuadOrder(std::span<const XRViewDescriptor> views) noexcept {
            constexpr std::array roles{XRViewRole::PrimaryLeft, XRViewRole::PrimaryRight, XRViewRole::FoveatedInsetLeft,
                                       XRViewRole::FoveatedInsetRight};
            return std::ranges::equal(views, roles, {}, &XRViewDescriptor::role);
        }

        /** @brief Checks the two encoded inset projections against their peripheral counterparts. */
        [[nodiscard]] bool ValidNativeInsets(std::span<const XrCompositionLayerProjectionView> views) noexcept {
            const auto nativeView = [](const XrCompositionLayerProjectionView &view) {
                return XrView{XR_TYPE_VIEW, nullptr, view.pose, view.fov};
            };
            constexpr XrViewStateFlags valid = XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
            return ContainsNativeInset(nativeView(views[0]), nativeView(views[2]), valid) &&
                   ContainsNativeInset(nativeView(views[1]), nativeView(views[3]), valid);
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
        if (fault_)
            return Result<NativeSessionBorrow>::Failure(WithCause(MakeError(XRErrors::OperationUnavailable), *fault_));
        if (!owner_.IsValid() || !wait_)
            return Result<NativeSessionBorrow>::Failure(MakeError(XRErrors::OperationUnavailable));
        return session_->Borrow(owner_);
    }

    /** @copydoc OpenXRFrameCompositor::Bind */
    Result<void> OpenXRFrameCompositor::Bind(NativeOpenXRDispatch dispatch, const NativeFramePacingPolicy &pacing,
                                             NativeCompositorColorSpace colorSpace, const NativeFrameMetricHandles &metrics) {
        // Snapshot before foreign loader callbacks can mutate the caller-owned policy.
        const NativeFramePacingPolicy admittedPacing = pacing;
        const NativeFrameMetricHandles admittedMetrics = metrics;
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
        metrics_ = admittedMetrics;
        metrics_.frameDepth.Observe(0);
        return Result<void>::Success();
    }

    /** @copydoc OpenXRFrameCompositor::RecordOutcome */
    void OpenXRFrameCompositor::RecordOutcome(const NativeFrameMetricOutcome outcome) const noexcept {
        metrics_.outcomes[static_cast<std::size_t>(outcome)].Add();
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
            RecordOutcome(NativeFrameMetricOutcome::WaitFailed);
            static_cast<void>(frames_->CancelWait());
            return Outcome::Failure(NativeError("wait frame", result));
        }
        nativeFrame_ = reservation.frame;
        RecordOutcome(NativeFrameMetricOutcome::WaitCompleted);
        metrics_.frameDepth.Observe(1);
        if (state.predictedDisplayPeriod > 0)
            metrics_.predictedPeriod.Observe(static_cast<double>(state.predictedDisplayPeriod) / 1'000'000'000.0);
        period_ = state.predictedDisplayPeriod;
        nativeDisplayTime_ = state.predictedDisplayTime;
        const auto prediction = XRRenderPredictionTime::Create(state.predictedDisplayTime);
        // SYNC: a completed native wait always creates begin/end debt, even with unusable prediction evidence.
        const auto recorded =
            frames_->RecordWaitWithNativePeriod(prediction.HasValue() ? prediction.Value() : XRRenderPredictionTime{},
                                                state.shouldRender == XR_TRUE, Duration::FromNanoseconds(state.predictedDisplayPeriod));
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
            RecordOutcome(NativeFrameMetricOutcome::BeginFailed);
            fault_ = NativeError("begin frame", result);
            return Result<void>::Failure(*fault_);
        }
        begun_ = true;
        RecordOutcome(result == XR_FRAME_DISCARDED ? NativeFrameMetricOutcome::BeginDiscarded : NativeFrameMetricOutcome::BeginCompleted);
        if (frames_->Begin(frame) != XRFrameStatus::Ok) {
            fault_ = MakeError(XRErrors::IdentityStale);
            return Result<void>::Failure(*fault_);
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

    std::uint32_t NativeProjectionResolution::BeginFrame(const XRFrameId &frame, const bool adaptive, const std::uint64_t deviceIncarnation,
                                                         const std::uint64_t rendererRevision) noexcept {
        if (!adaptive || !frame.IsValid() || deviceIncarnation == 0 || rendererRevision == 0) {
            *this = {};
            return scale_;
        }
        if (frame.session != frame_.session || frame.configuration != frame_.configuration ||
            frame.capabilityRevision != frame_.capabilityRevision || deviceIncarnation != deviceIncarnation_ ||
            rendererRevision != rendererRevision_)
            *this = {};
        if (frame.sequence <= frame_.sequence)
            return frameScale_;
        if (!completed_ || frame.sequence - frame_.sequence != 1) {
            late_ = 0;
            headroom_ = 0;
        }
        frame_ = frame;
        deviceIncarnation_ = deviceIncarnation;
        rendererRevision_ = rendererRevision;
        frameScale_ = scale_;
        completed_ = false;
        return frameScale_;
    }

    bool NativeProjectionResolution::AwaitsCompletion(const XRFrameId &frame) const noexcept {
        return frame.IsValid() && frame == frame_ && !completed_;
    }

    /** @copydoc NativeProjectionResolution::ObserveCompletion */
    void NativeProjectionResolution::ObserveCompletion(const NativeFramePacingObservation &observation,
                                                       const std::chrono::nanoseconds gpuElapsed) noexcept {
        if (!AwaitsCompletion(observation.frame) || observation.predictedPeriodNanoseconds <= 0 || gpuElapsed.count() <= 0)
            return;
        completed_ = true;
        // Keep one quarter of the native refresh interval outside scene rendering for the compositor.
        const auto budget = observation.predictedPeriodNanoseconds - observation.predictedPeriodNanoseconds / 4;
        if (gpuElapsed.count() > budget) {
            headroom_ = 0;
            if (++late_ == LateFrames) {
                scale_ = std::max(MinimumPercent, scale_ - StepPercent);
                late_ = 0;
            }
            return;
        }
        late_ = 0;
        if (observation.deadline != NativeFrameDeadline::WithinBudget || gpuElapsed.count() >= budget - budget / 5) {
            headroom_ = 0;
            return;
        }
        if (++headroom_ == HeadroomFrames) {
            scale_ = std::min(FullPercent, scale_ + StepPercent);
            headroom_ = 0;
        }
    }

    XRRenderRectangle NativeProjectionResolution::Rectangle(const Render::FramebufferExtent extent, const std::uint32_t percent) noexcept {
        const auto admitted = std::clamp(percent, MinimumPercent, FullPercent);
        const auto scale = [admitted](const std::uint32_t dimension) {
            const auto pixels = static_cast<std::uint32_t>(static_cast<std::uint64_t>(dimension) * admitted / FullPercent);
            return std::max(pixels, static_cast<std::uint32_t>(dimension != 0));
        };
        return {0, 0, scale(extent.width), scale(extent.height)};
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
        if (!begun_ || frames_->ValidateProjectionEnd(frame, plan) != XRFrameStatus::Ok || targets.empty() ||
            targets.size() > XRFrameHardLimits::MaximumImages)
            return Result<void>::Failure(MakeError(XRErrors::ViewPlanInvalid));
        const auto native = session_->ProjectionConfiguration(frame.session);
        if (native.HasError())
            return Result<void>::Failure(native.ErrorValue());
        const bool quad = native.Value().type == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO_WITH_FOVEATED_INSET;
        const auto layout = quad ? XRViewConfigurationType::QuadView : XRViewConfigurationType::PrimaryStereo;
        if (plan.Configuration().type != layout || plan.Views().size() != native.Value().viewCount ||
            plan.Configuration().blendMode != XREnvironmentBlendMode::Opaque)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
        if (quad && !HasNativeQuadOrder(plan.Views()))
            return Result<void>::Failure(MakeError(XRErrors::ViewPlanInvalid));
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
        if (!std::ranges::all_of(plan.Targets(), [&](const auto &target) {
            return std::ranges::find(targets, target.image, &NativeProjectionTarget::image) != targets.end();
        }))
            return Result<void>::Failure(MakeError(XRErrors::ExternalTargetInvalid));
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

    namespace {
        /** @brief Exposes only the latest released image on this provider, with its exact per-eye bounds. */
        std::optional<XrSwapchainSubImage> ReprojectionSubImage(const OpenXRSwapchain &owner, const XRReprojectionSubImage &image,
                                                                const NativeReprojectionProviderBorrow &provider,
                                                                const NativeSwapchainRole expected, const std::size_t eye) {
            const auto released = owner.BorrowReleased(image.image);
            if (released.HasError())
                return std::nullopt;
            const auto &allocation = released.Value().allocation;
            const auto &rect = image.rectangle;
            const auto &extent = provider.offer.extents[eye];
            constexpr auto maximum = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
            if (allocation.role != expected || !allocation.reprojection || allocation.reprojection->attempt != provider.attempt ||
                allocation.reprojection->route != provider.offer.route || allocation.reprojection->eyeExtents != provider.offer.extents ||
                image.arrayLayer >= allocation.texture.layerCount || rect.width != extent.width || rect.height != extent.height ||
                !rect.FitsWithin(allocation.texture.extent) || allocation.texture.extent.width > maximum ||
                allocation.texture.extent.height > maximum)
                return std::nullopt;
            return XrSwapchainSubImage{released.Value().swapchain,
                                       {{static_cast<std::int32_t>(rect.x), static_cast<std::int32_t>(rect.y)},
                                        {static_cast<std::int32_t>(rect.width), static_cast<std::int32_t>(rect.height)}},
                                       image.arrayLayer};
        }
    }  // namespace

    /** @copydoc OpenXRFrameCompositor::EncodeReprojection */
    std::optional<OpenXRFrameCompositor::ReprojectionEncoding> OpenXRFrameCompositor::EncodeReprojection(
        const NativeReprojectionSubmission &synthesis) const {
        if (synthesis.motion.session_ != session_ || synthesis.depth.session_ != session_)
            return std::nullopt;
        const auto borrowed = session_->BorrowReprojectionProvider(synthesis.provider);
        if (borrowed.HasError())
            return std::nullopt;
        const auto &provider = borrowed.Value();
        const auto &inputs = synthesis.inputs;
        if (inputs.views.size() != provider.offer.extents.size() || !inputs.applicationDelta)
            return std::nullopt;
        const auto &delta = *inputs.applicationDelta;
        const auto &p = delta.translationMeters;
        const auto &q = delta.rotation;
        const XrPosef pose{{q.x, q.y, q.z, q.w}, {p.x, p.y, p.z}};
        ReprojectionEncoding encoded{.route = provider.offer.route};
        for (std::size_t eye = 0; eye < inputs.views.size(); ++eye) {
            const auto &input = inputs.views[eye];
            const auto motion = ReprojectionSubImage(synthesis.motion, input.motion, provider, NativeSwapchainRole::Motion, eye);
            const auto depth = ReprojectionSubImage(synthesis.depth, input.depth, provider, NativeSwapchainRole::ReprojectionDepth, eye);
            if (!motion || !depth)
                return std::nullopt;
            if (encoded.route == NativeReprojectionRoute::FrameSynthesisExt) {
                encoded.ext[eye] = {XR_TYPE_FRAME_SYNTHESIS_INFO_EXT,
                                    nullptr,
                                    0,
                                    *motion,
                                    {1, 1, 1, 0},
                                    {},
                                    pose,
                                    *depth,
                                    input.minimumDepth,
                                    input.maximumDepth,
                                    input.nearMeters,
                                    input.farMeters};
            } else if (encoded.route == NativeReprojectionRoute::SpaceWarpFb) {
                encoded.fb[eye] = {XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB,
                                   nullptr,
                                   0,
                                   *motion,
                                   pose,
                                   *depth,
                                   input.minimumDepth,
                                   input.maximumDepth,
                                   input.nearMeters,
                                   input.farMeters};
            } else {
                return std::nullopt;
            }
        }
        const auto current = session_->BorrowReprojectionProvider(synthesis.provider);
        if (current.HasError())
            return std::nullopt;
        encoded.session = current.Value().session;
        encoded.end = current.Value().end;
        return encoded;
    }

    /** @copydoc OpenXRFrameCompositor::EndReprojectedProjection */
    Result<XRReprojectionStatus> OpenXRFrameCompositor::EndReprojectedProjection(const XRFrameId &frame, const XRViewRenderPlan &plan,
                                                                                 const std::span<const NativeProjectionTarget> targets,
                                                                                 const NativeReprojectionSubmission &synthesis) {
        using Outcome = Result<XRReprojectionStatus>;
        const ProjectionEncoding encoding{plan, targets, nullptr};
        if (const auto baseline = ValidateProjection(frame, encoding); baseline.HasError())
            return Outcome::Failure(baseline.ErrorValue());
        if (plan.Configuration().type != XRViewConfigurationType::PrimaryStereo)
            return Outcome::Failure(MakeError(XRErrors::OperationUnsupported));
        const auto status = frames_->ValidateReprojection(frame, plan, synthesis.inputs);
        const bool ready = status == XRReprojectionStatus::Ready;
        const auto ended = EndEncodedProjection(frame, encoding, ready ? &synthesis : nullptr);
        if (ended.HasError())
            return Outcome::Failure(ended.ErrorValue());
        return Outcome::Success(ready && !ended.Value() ? XRReprojectionStatus::InputsInvalid : status);
    }

    /** @copydoc OpenXRFrameCompositor::EndProjection */
    Result<void> OpenXRFrameCompositor::EndProjection(const XRFrameId &frame, const XRViewRenderPlan &plan,
                                                      std::span<const NativeProjectionTarget> targets, const NativeProjectionDepth *depth) {
        const ProjectionEncoding encoding{plan, targets, depth};
        if (const auto valid = ValidateProjection(frame, encoding); valid.HasError())
            return valid;
        const auto ended = EndEncodedProjection(frame, encoding, nullptr);
        return ended.HasError() ? Result<void>::Failure(ended.ErrorValue()) : Result<void>::Success();
    }

    namespace {
        /** @brief Admits only formats with an actual alpha channel on the selected projection target. */
        bool HasProjectionAlpha(const XRExternalRenderTargetDescriptor &target) noexcept {
            if (target.role != XRExternalRenderTargetRole::ProjectionColor)
                return true;
            using enum Render::RenderTextureFormat;
            switch (target.format) {
                case Rgba8Unorm:
                case Rgba8UnormSrgb:
                case Bgra8Unorm:
                case Bgra8UnormSrgb:
                case Rgba16Float:
                case Rgba32Float:
                    return true;
                default:
                    return false;
            }
        }
    }  // namespace

    /** @copydoc OpenXRFrameCompositor::EndPassthroughProjection */
    Result<void> OpenXRFrameCompositor::EndPassthroughProjection(const XRFrameId &frame, const XRViewRenderPlan &plan,
                                                                 std::span<const NativeProjectionTarget> targets,
                                                                 const NativePassthroughProjection &passthrough,
                                                                 const NativeProjectionDepth *depth) {
        const ProjectionEncoding encoding{plan, targets, depth};
        if (const auto valid = ValidateProjection(frame, encoding); valid.HasError())
            return valid;
        if (plan.Configuration().type != XRViewConfigurationType::PrimaryStereo)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
        const auto &scope = passthrough.scope;
        if (passthrough.frame != frame || scope.session != frame.session)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        auto expectedSpace = scope.world;
        if (const auto &calibration = passthrough.referenceToWorld) {
            const auto reference = calibration->Source().kind;
            if ((reference != XRSpaceKind::Local && reference != XRSpaceKind::Stage) || calibration->Session() != frame.session ||
                calibration->Target() != scope.world || calibration->Purpose() != XRPosePurpose::PresentationPrediction ||
                calibration->Time().renderPrediction != plan.PredictedDisplayTime() || !calibration->Components().positionMeters.value ||
                !calibration->Components().orientation.value)
                return Result<void>::Failure(MakeError(XRErrors::IdentityStale, "Passthrough World calibration is stale"));
            expectedSpace = calibration->Source();
        }
        if (std::ranges::any_of(plan.Views(), [&](const auto &view) {
            return view.pose.target != expectedSpace;
        }))
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (!std::ranges::all_of(plan.Targets(), HasProjectionAlpha))
            return Result<void>::Failure(MakeError(XRErrors::ExternalTargetInvalid, "Passthrough needs projection alpha"));
        const auto capacity = frames_->ValidateEnd(frame, 2);
        if (capacity != XRFrameStatus::Ok)
            return Result<void>::Failure(
                MakeError(capacity == XRFrameStatus::CapacityExceeded ? XRErrors::CapacityExceeded : XRErrors::OperationIncompatible));
        const auto ended = EndEncodedProjection(frame, encoding, nullptr, &passthrough);
        return ended.HasError() ? Result<void>::Failure(ended.ErrorValue()) : Result<void>::Success();
    }

    /** @copydoc OpenXRFrameCompositor::EndEncodedProjection */
    Result<bool> OpenXRFrameCompositor::EndEncodedProjection(const XRFrameId &frame, const ProjectionEncoding &encoding,
                                                             const NativeReprojectionSubmission *synthesis,
                                                             const NativePassthroughProjection *passthrough) {
        const auto &plan = encoding.plan;
        // PERF: fixed bounded native encoding storage; no frame allocation or native discovery.
        std::array<XrCompositionLayerProjectionView, XRHardLimits::MaximumViews> views{};
        std::array<XrCompositionLayerDepthInfoKHR, XRHardLimits::MaximumViews> depths{};
        XrSpace reference{XR_NULL_HANDLE};
        for (std::size_t index = 0; index < plan.Views().size(); ++index) {
            const auto space = EncodeView(encoding, plan.Views()[index], views[index], depths[index]);
            if (space.HasError())
                return Result<bool>::Failure(space.ErrorValue());
            if (reference != XR_NULL_HANDLE && reference != space.Value())
                return Result<bool>::Failure(MakeError(XRErrors::CoordinateSpaceIncompatible));
            reference = space.Value();
        }
        if (plan.Configuration().type == XRViewConfigurationType::QuadView && !ValidNativeInsets(views))
            return Result<bool>::Failure(MakeError(XRErrors::ViewPlanInvalid));
        const auto optional = synthesis ? EncodeReprojection(*synthesis) : std::nullopt;
        XrSession session{XR_NULL_HANDLE};
        if (optional) {
            session = optional->session;
        } else {
            const auto native = Current();
            if (native.HasError())
                return Result<bool>::Failure(native.ErrorValue());
            session = native.Value().session;
        }
        if (optional)
            for (std::size_t eye = 0; eye < plan.Views().size(); ++eye)
                views[eye].next = optional->route == NativeReprojectionRoute::FrameSynthesisExt
                                      ? static_cast<const void *>(&optional->ext[eye])
                                      : static_cast<const void *>(&optional->fb[eye]);
        const XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION,
                                                 nullptr,
                                                 passthrough ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0,
                                                 reference,
                                                 static_cast<std::uint32_t>(plan.Views().size()),
                                                 views.data()};
        // OpenXR defines this native base-header view for projection layer dispatch.
        static_assert(sizeof(const XrCompositionLayerBaseHeader *) == sizeof(&layer));
        const auto ended =
            EndNative(frame, session, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, std::bit_cast<const XrCompositionLayerBaseHeader *>(&layer), &plan,
                      optional ? optional->end : end_, passthrough);
        return ended.HasError() ? Result<bool>::Failure(ended.ErrorValue()) : Result<bool>::Success(optional.has_value());
    }

    /** @copydoc OpenXRFrameCompositor::EndNative */
    Result<void> OpenXRFrameCompositor::EndNative(const XRFrameId &frame, XrSession session, XrEnvironmentBlendMode blend,
                                                  const XrCompositionLayerBaseHeader *layer, const XRViewRenderPlan *projection,
                                                  PFN_xrEndFrame dispatch, const NativePassthroughProjection *passthrough) {
        // SYNC: cleanup must echo the completed native wait even when its prediction could not enter Horo's time contract.
        const XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO, nullptr, nativeDisplayTime_, blend, layer ? 1U : 0U, layer ? &layer : nullptr};
        const auto dispatched = passthrough ? session_->EndPassthroughFrame(passthrough->scope, session, info, dispatch, passthrough->authorization)
                                            : Result<XrResult>::Success(dispatch(session, &info));
        if (dispatched.HasError())
            return Result<void>::Failure(dispatched.ErrorValue());
        const auto result = dispatched.Value();
        if (XR_FAILED(result)) {
            RecordOutcome(NativeFrameMetricOutcome::EndFailed);
            fault_ = passthrough && (result == XR_ERROR_NOT_PERMITTED_PASSTHROUGH_FB || result == XR_ERROR_PERMISSION_INSUFFICIENT)
                         ? MakeError(XRErrors::PermissionDenied, "end frame")
                         : NativeError("end frame", result);
            return Result<void>::Failure(*fault_);
        }
        metrics_.RecordAcceptedEnd(projection);
        metrics_.frameDepth.Observe(0);
        const auto ended = projection ? frames_->EndProjection(frame, *projection) : frames_->End(frame, 0);
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
        const auto native = Current();
        return native.HasError() ? Result<void>::Failure(native.ErrorValue())
                                 : EndNative(frame, native.Value().session, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, nullptr, nullptr, end_);
    }

    /** @copydoc OpenXRFrameCompositor::RetainedFault */
    const Error *OpenXRFrameCompositor::RetainedFault() const noexcept {
        return thread_ == std::this_thread::get_id() && fault_ ? &*fault_ : nullptr;
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
        if (nativeFrame_.IsValid())
            metrics_.frameDepth.Observe(0);
        metrics_ = {};
        owner_ = {};
        nativeFrame_ = {};
        wait_ = nullptr;
        begin_ = nullptr;
        end_ = nullptr;
        begun_ = false;
        fault_.reset();
        period_ = 0;
        nativeDisplayTime_ = 0;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRReprojectionFrameImages::OpenXRReprojectionFrameImages */
    OpenXRReprojectionFrameImages::OpenXRReprojectionFrameImages(XRFrameLifecycle &frames, OpenXRSwapchain &motion,
                                                                 OpenXRSwapchain &depth) noexcept
        : frames_(&frames), owners_{&motion, &depth} {}

    /** @copydoc OpenXRReprojectionFrameImages::ValidateOwners */
    Result<void> OpenXRReprojectionFrameImages::ValidateOwners(const XRFrameId &frame) const {
        for (const auto *owner : owners_) {
            if (const auto valid = owner->CheckCurrent(); valid.HasError())
                return valid;
        }
        const auto &motion = owners_.front()->request_;
        const auto &depth = owners_.back()->request_;
        if (owners_.front()->session_ != owners_.back()->session_ || motion.role != NativeSwapchainRole::Motion ||
            depth.role != NativeSwapchainRole::ReprojectionDepth || motion.target.slot.index == depth.target.slot.index ||
            motion.target.session != frame.session || depth.target.session != frame.session)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        const auto &a = *motion.reprojection;
        const auto &b = *depth.reprojection;
        if (a.attempt != b.attempt || a.route != b.route || a.eyeExtents != b.eyeExtents)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        return Result<void>::Success();
    }

    /** @copydoc OpenXRReprojectionFrameImages::Prepare */
    Result<std::optional<NativeReprojectionFramePair>> OpenXRReprojectionFrameImages::Prepare(
        const XRFrameId &frame, const std::span<const XRSwapchainImageId> projection) {
        using Outcome = Result<std::optional<NativeReprojectionFramePair>>;
        if (const auto thread = owners_.front()->CheckThread(); thread.HasError())
            return Outcome::Failure(thread.ErrorValue());
        if (frames_->ValidateCurrent(frame) != XRFrameStatus::Ok || frames_->Snapshot().phase != XRFramePhase::ImagesAcquired)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        if (phase_ != Phase::Idle || attempted_ == frame)
            return Outcome::Success(std::nullopt);
        if (const auto valid = ValidateOwners(frame); valid.HasError())
            return Outcome::Failure(valid.ErrorValue());
        if (std::ranges::any_of(owners_, [](const auto *owner) {
            return owner->Phase() != NativeSwapchainPhase::Available;
        }))
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));

        attempted_ = frame;
        phase_ = Phase::Omitted;  // Every acquired prefix remains cleanup-only until both imports pass canonical preflight.
        released_.fill(false);
        std::array<NativeReprojectionAcquisition, 2> acquired;
        for (std::size_t index = 0; index < owners_.size(); ++index) {
            auto &owner = *owners_[index];
            if (auto native = owner.Acquire(); native.HasError())
                return Outcome::Failure(native.ErrorValue());
            const auto ready = owner.Wait(0);
            if (ready.HasError())
                return Outcome::Failure(ready.ErrorValue());
            if (!ready.Value())
                return Outcome::Success(std::nullopt);
            if (const auto imported = owner.Import(); imported.HasError())
                return Outcome::Failure(imported.ErrorValue());
            const auto auxiliary = owner.CurrentReprojection();
            if (auxiliary.HasError())
                return Outcome::Failure(auxiliary.ErrorValue());
            acquired[index] = auxiliary.Value();
        }
        if (acquired.front().texture == acquired.back().texture ||
            frames_->ValidateReprojectionSubmission(frame, projection, acquired.front().evidence, acquired.back().evidence) !=
                XRFrameStatus::Ok)
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        pair_.emplace(NativeReprojectionFramePair{frame, acquired.front(), acquired.back()});
        phase_ = Phase::Prepared;
        return Outcome::Success(pair_);
    }

    /** @copydoc OpenXRReprojectionFrameImages::Submit */
    Result<void> OpenXRReprojectionFrameImages::Submit(const XRFrameId &frame, const std::span<const XRSwapchainImageId> projection) {
        if (const auto valid = ValidateOwners(frame); valid.HasError())
            return valid;
        if (phase_ != Phase::Prepared || !pair_ || frame != pair_->frame)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (auto motion = owners_.front()->ValidateAcquisition(pair_->motion.evidence.image); motion.HasError())
            return motion;
        if (auto depth = owners_.back()->ValidateAcquisition(pair_->depth.evidence.image); depth.HasError())
            return depth;
        if (frames_->SubmitReprojection(frame, projection, pair_->motion.evidence, pair_->depth.evidence) != XRFrameStatus::Ok)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        phase_ = Phase::Submitted;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRReprojectionFrameImages::Omit */
    Result<void> OpenXRReprojectionFrameImages::Omit(const XRFrameId &frame) {
        if (const auto thread = owners_.front()->CheckThread(); thread.HasError())
            return thread;
        if (phase_ != Phase::Prepared || !pair_ || frame != pair_->frame)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        pair_.reset();
        phase_ = Phase::Omitted;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRReprojectionFrameImages::Release */
    Result<bool> OpenXRReprojectionFrameImages::Release(OpenXRSwapchain &owner, const XRSwapchainImageId *registered) {
        if (owner.Phase() == NativeSwapchainPhase::Acquired) {
            const auto ready = owner.Wait(0);
            if (ready.HasError() || !ready.Value())
                return ready;
        }
        if (owner.Phase() != NativeSwapchainPhase::Available) {
            const auto released = owner.PollRelease();
            if (released.HasError() || !released.Value())
                return released;
        }
        if (registered) {
            if (const auto released = owner.BorrowReleased(*registered); released.HasError())
                return Result<bool>::Failure(released.ErrorValue());
            if (frames_->Release(attempted_, *registered) != XRFrameStatus::Ok)
                return Result<bool>::Failure(MakeError(XRErrors::OperationIncompatible));
        }
        return Result<bool>::Success(true);
    }

    /** @copydoc OpenXRReprojectionFrameImages::PollRetirement */
    Result<bool> OpenXRReprojectionFrameImages::PollRetirement() {
        if (const auto thread = owners_.front()->CheckThread(); thread.HasError())
            return Result<bool>::Failure(thread.ErrorValue());
        if (phase_ == Phase::Idle)
            return Result<bool>::Success(true);
        if (phase_ == Phase::Prepared || (phase_ == Phase::Submitted && frames_->ValidateCurrent(attempted_) != XRFrameStatus::Ok))
            return Result<bool>::Failure(MakeError(XRErrors::OperationIncompatible));
        std::optional<Error> failure;
        const std::array registered{pair_ ? &pair_->motion.evidence.image : nullptr, pair_ ? &pair_->depth.evidence.image : nullptr};
        for (std::size_t index = 0; index < owners_.size(); ++index) {
            if (released_[index])
                continue;
            const auto released = Release(*owners_[index], registered[index]);
            if (released.HasError()) {
                if (!failure)
                    failure = released.ErrorValue();
                continue;
            }
            released_[index] = released.Value();
        }
        if (failure)
            return Result<bool>::Failure(std::move(*failure));
        if (!released_.front() || !released_.back())
            return Result<bool>::Success(false);
        pair_.reset();
        phase_ = Phase::Idle;
        return Result<bool>::Success(true);
    }
}  // namespace Horo::XR::OpenXRInternal
