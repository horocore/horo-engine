#include "Horo/XR/XRFakeRuntime.h"

#include <algorithm>
#include <cmath>

namespace Horo::XR {
    namespace {
        /** @brief Check finite, canonical scalar/vector values without interpreting Input semantics. */
        [[nodiscard]] bool ValidActionValue(const XRFakeAction &action) noexcept {
            if (!std::isfinite(action.value[0]) || !std::isfinite(action.value[1]))
                return false;
            if (!action.active && action.value != std::array<float, 2>{})
                return false;
            if (action.kind != XRFakeActionKind::Vector2 && action.value[1] != 0)
                return false;
            return action.kind != XRFakeActionKind::Boolean || action.value[0] == 0 || action.value[0] == 1;
        }
    }  // namespace

    /** @copydoc XRFakeRuntime::Resources::Prepare */
    Result<void> XRFakeRuntime::Resources::Prepare(const XRSessionPreparation stage, const XRSessionId &, const XRFeaturePlan &) {
        if (failStage == stage)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        return Result<void>::Success();
    }

    /** @copydoc XRFakeRuntime::Resources::Release */
    void XRFakeRuntime::Resources::Release(XRSessionPreparation, const XRSessionId &) noexcept {
        // Preparation owns no native, GPU or external resources, so fake retirement has no work to perform.
    }

    /** @copydoc XRFakeRuntime::XRFakeRuntime */
    XRFakeRuntime::XRFakeRuntime() noexcept : sessions_(resources_), frames_(sessions_) {}

    /** @copydoc XRFakeRuntime::~XRFakeRuntime */
    XRFakeRuntime::~XRFakeRuntime() {
        Shutdown();
    }

    /** @copydoc XRFakeRuntime::Activate */
    Result<XRSessionId> XRFakeRuntime::Activate(const XRCapabilitySnapshot &capabilities, const XRFeaturePlan &plan,
                                                const XRFrameLimits limits, const std::optional<XRSessionPreparation> failStage) {
        if (shutdown_)
            return Result<XRSessionId>::Failure(MakeError(XRErrors::OperationUnavailable));
        if (failStage && *failStage >= XRSessionPreparation::Count)
            return Result<XRSessionId>::Failure(MakeError(XRErrors::OperationInvalid));
        if (limits.maximumViews > capabilities.Limits().maximumViews || limits.maximumViews > plan.Limits().maximumViews ||
            limits.maximumViews > XRFrameHardLimits::MaximumViews || limits.maximumImages > XRFrameHardLimits::MaximumImages ||
            limits.maximumLayers > XRFrameHardLimits::MaximumLayers)
            return Result<XRSessionId>::Failure(MakeError(XRErrors::CapacityExceeded));
        resources_.failStage = failStage;
        auto activated = sessions_.Activate(capabilities, capabilities.System(), capabilities.Revision(), plan);
        resources_.failStage.reset();
        if (!activated.HasValue())
            return activated;
        frames_.ResetAfterQuiescence();
        const XRViewConfigurationId configuration{activated.Value(), {.index = 1, .generation = 1}};
        const auto bound = frames_.BindConfiguration(configuration, capabilities, limits);
        HORO_INVARIANT(bound == XRFrameStatus::Ok);
        limits_ = limits;
        plan_ = plan;
        script_.clear();
        cursor_ = 0;
        return activated;
    }

    /** @copydoc XRFakeRuntime::ValidateStep */
    XRFrameStatus XRFakeRuntime::ValidateStep(const XRFakeStep &step, const XRSessionId &session) const {
        using enum XRFakeFailure;
        if ((step.event && *step.event > XRSessionEvent::InstanceLost) || step.failure >= Count)
            return XRFrameStatus::InvalidInput;
        if (!step.executeFrame && (!step.event || step.actionCount != 0 || step.viewCount != 0 || step.failure != None))
            return XRFrameStatus::InvalidInput;
        if (!step.shouldRender && step.failure >= Locate && step.failure <= Release)
            return XRFrameStatus::Unsupported;
        if (step.executeFrame && !step.prediction.IsValid())
            return XRFrameStatus::InvalidInput;
        if (step.viewCount > limits_.maximumViews || step.actionCount > XRFakeLimits::MaximumActions ||
            step.actionCount > plan_->Limits().maximumActions)
            return XRFrameStatus::CapacityExceeded;
        if (step.shouldRender && step.executeFrame && (step.viewCount == 0 || limits_.maximumImages == 0 || limits_.maximumLayers == 0))
            return XRFrameStatus::Unsupported;
        const auto views = ValidateViews(step, session);
        return views == XRFrameStatus::Ok ? ValidateActions(step, session) : views;
    }

    /** @copydoc XRFakeRuntime::ValidateViews */
    XRFrameStatus XRFakeRuntime::ValidateViews(const XRFakeStep &step, const XRSessionId &session) const noexcept {
        for (std::size_t i = 0; i < step.views.size(); ++i) {
            if (i >= step.viewCount) {
                if (step.views[i])
                    return XRFrameStatus::InvalidInput;
                continue;
            }
            if (!step.views[i])
                return XRFrameStatus::InvalidInput;
            const auto &pose = *step.views[i];
            if (pose.Session() != session)
                return XRFrameStatus::StaleSession;
            if (pose.Purpose() != XRPosePurpose::PresentationPrediction || pose.Time().renderPrediction != step.prediction)
                return XRFrameStatus::InvalidInput;
        }
        return XRFrameStatus::Ok;
    }

    /** @copydoc XRFakeRuntime::ValidateActions */
    XRFrameStatus XRFakeRuntime::ValidateActions(const XRFakeStep &step, const XRSessionId &session) const noexcept {
        if (step.actionCount != 0 && !step.actionSampleTime.IsValid())
            return XRFrameStatus::InvalidInput;
        for (std::uint32_t i = 0; i < step.actionCount; ++i) {
            const auto &action = step.actions[i];
            if (!action.action.IsValid() || action.kind >= XRFakeActionKind::Count || !ValidActionValue(action))
                return XRFrameStatus::InvalidInput;
            if (action.action.session != session)
                return XRFrameStatus::StaleSession;
            constexpr std::array capabilities{XRCapability::BooleanActions, XRCapability::FloatActions, XRCapability::Vector2Actions};
            if (const auto decision = plan_->Decision(capabilities[static_cast<std::size_t>(action.kind)]).state;
                decision != XRFeatureDecisionState::Required && decision != XRFeatureDecisionState::OptionalEnabled)
                return XRFrameStatus::Unsupported;
            if (std::any_of(step.actions.begin(), step.actions.begin() + i, [&action](const XRFakeAction &prior) {
                return prior.action == action.action;
            }))
                return XRFrameStatus::Duplicate;
        }
        return XRFrameStatus::Ok;
    }

    /** @copydoc XRFakeRuntime::LoadScript */
    XRFrameStatus XRFakeRuntime::LoadScript(const XRSessionId &session, const std::span<const XRFakeStep> steps) {
        if (shutdown_)
            return XRFrameStatus::Shutdown;
        if (!session.IsValid())
            return XRFrameStatus::InvalidInput;
        if (session != sessions_.Snapshot().session)
            return XRFrameStatus::StaleSession;
        if (steps.size() > XRFakeLimits::MaximumSteps)
            return XRFrameStatus::CapacityExceeded;
        XRRenderPredictionTime previous;
        for (const auto &step : steps) {
            if (const auto status = ValidateStep(step, session); status != XRFrameStatus::Ok)
                return status;
            if (step.executeFrame) {
                if (previous.IsValid() && step.prediction <= previous)
                    return XRFrameStatus::InvalidInput;
                previous = step.prediction;
            }
        }
        std::vector<XRFakeStep> copied(steps.begin(), steps.end());
        script_.swap(copied);
        cursor_ = 0;
        return XRFrameStatus::Ok;
    }

    /** @copydoc XRFakeRuntime::RunFrame */
    XRFrameStatus XRFakeRuntime::RunFrame(const XRFakeStep &step, const XRSessionId &session, XRFakeOutcome &outcome) noexcept {
        using enum XRFakeFailure;
        if (const auto reserved = frames_.ReserveWait(session); reserved.status != XRFrameStatus::Ok)
            return reserved.status;
        outcome.failure = step.failure;
        if (step.failure == Wait) {
            static_cast<void>(frames_.CancelWait());
            return XRFrameStatus::Unavailable;
        }
        const auto waited = frames_.RecordWait(step.prediction, step.shouldRender);
        // Begin failures recover through an explicit fake begin retry before zero-layer completion.
        const auto begun = frames_.Begin(waited.frame);
        HORO_INVARIANT(begun == XRFrameStatus::Ok);
        auto status = step.failure == Begin ? XRFrameStatus::Unavailable : XRFrameStatus::Ok;
        if (status == XRFrameStatus::Ok && step.shouldRender)
            status = RenderFrame(step, waited.frame, outcome);
        else
            outcome.frame = frames_.Snapshot();
        if (status == XRFrameStatus::Ok && step.failure == End)
            status = XRFrameStatus::Unavailable;
        if (status != XRFrameStatus::Ok && step.shouldRender && (step.failure == Begin || step.failure == End)) {
            const auto aborted = frames_.Abort(waited.frame);
            HORO_INVARIANT(aborted == XRFrameStatus::Ok);
        }
        const auto ended = frames_.End(waited.frame, status == XRFrameStatus::Ok && step.shouldRender ? 1U : 0U);
        HORO_INVARIANT(ended == XRFrameStatus::Ok);
        return status;
    }

    /** @copydoc XRFakeRuntime::RenderFrame */
    XRFrameStatus XRFakeRuntime::RenderFrame(const XRFakeStep &step, const XRFrameId &frame, XRFakeOutcome &outcome) noexcept {
        using enum XRFakeFailure;
        const XRSwapchainImageId image{{frame.session, {.index = 1, .generation = 1}}, {.index = 1, .generation = 1}};
        auto status = step.failure == Locate ? XRFrameStatus::Unavailable : frames_.LocateViews(frame, step.viewCount);
        bool acquired = false;
        bool released = false;
        if (status == XRFrameStatus::Ok) {
            status = step.failure == Acquire ? XRFrameStatus::Unavailable : frames_.Acquire(frame, image);
            acquired = status == XRFrameStatus::Ok;
        }
        if (status == XRFrameStatus::Ok) {
            const std::array images{image};
            status = step.failure == Submit ? XRFrameStatus::Unavailable : frames_.Submit(frame, images);
        }
        if (status == XRFrameStatus::Ok) {
            status = step.failure == Release ? XRFrameStatus::Unavailable : frames_.Release(frame, image);
            released = status == XRFrameStatus::Ok;
        }
        outcome.frame = frames_.Snapshot();
        if (status != XRFrameStatus::Ok) {
            const auto aborted = frames_.Abort(frame);
            HORO_INVARIANT(aborted == XRFrameStatus::Ok);
            if (acquired && !released) {
                const auto release = frames_.Release(frame, image);
                HORO_INVARIANT(release == XRFrameStatus::Ok);
            }
        }
        return status;
    }

    /** @copydoc XRFakeRuntime::Step */
    XRFakeOutcome XRFakeRuntime::Step(const XRSessionId &session) {
        XRFakeOutcome outcome;
        outcome.nextStep = cursor_;
        outcome.session = sessions_.Snapshot();
        if (shutdown_) {
            outcome.status = XRFrameStatus::Shutdown;
            return outcome;
        }
        if (!session.IsValid()) {
            outcome.status = XRFrameStatus::InvalidInput;
            return outcome;
        }
        if (session != outcome.session.session) {
            outcome.status = XRFrameStatus::StaleSession;
            return outcome;
        }
        if (cursor_ == script_.size())
            return outcome;
        const auto &step = script_[cursor_];
        if (step.event && !sessions_.ApplyEvent(session, *step.event).HasValue()) {
            outcome.status = XRFrameStatus::OutOfOrder;
            return outcome;
        }
        outcome.consumed = true;
        outcome.nextStep = ++cursor_;
        outcome.session = sessions_.Snapshot();
        outcome.status = step.executeFrame ? RunFrame(step, session, outcome) : XRFrameStatus::Ok;
        if (outcome.status == XRFrameStatus::Ok && step.executeFrame)
            PublishEvidence(step, outcome);
        return outcome;
    }

    /** @copydoc XRFakeRuntime::PublishEvidence */
    void XRFakeRuntime::PublishEvidence(const XRFakeStep &step, XRFakeOutcome &outcome) noexcept {
        outcome.viewCount = step.viewCount;
        outcome.views = step.views;
        outcome.actionSampleTime = step.actionSampleTime;
        outcome.actionCount = step.actionCount;
        bool trackingLost = false;
        for (std::uint32_t i = 0; i < step.viewCount; ++i)
            trackingLost = trackingLost || step.views[i]->Components().loss != XRTrackingLossState::None;
        for (std::uint32_t i = 0; i < outcome.actionCount; ++i) {
            outcome.actions[i] = step.actions[i];
            if (!outcome.session.admitsActions || trackingLost) {
                outcome.actions[i].active = false;
                outcome.actions[i].value = {};
            }
        }
    }

    /** @copydoc XRFakeRuntime::Snapshot */
    XRSessionSnapshot XRFakeRuntime::Snapshot() const noexcept {
        return sessions_.Snapshot();
    }

    /** @copydoc XRFakeRuntime::Shutdown */
    void XRFakeRuntime::Shutdown() noexcept {
        if (shutdown_)
            return;
        frames_.Shutdown();
        sessions_.Shutdown();
        script_.clear();
        cursor_ = 0;
        shutdown_ = true;
    }
}  // namespace Horo::XR
