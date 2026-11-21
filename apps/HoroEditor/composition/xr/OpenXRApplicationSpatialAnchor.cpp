#include "OpenXRApplicationSession.h"

#include <limits>
#include <new>

namespace Horo::Application::Internal {
    using namespace XR;
    using namespace XR::OpenXRInternal;

    namespace {
        constexpr XRAnchorLimits PreviewLimits{1, Duration::FromMilliseconds(250)};
        constexpr auto SampleInterval = Duration::FromMilliseconds(100);
    }  // namespace

    bool OpenXRApplicationSession::MayArmSpatialAnchor() const {
        if (!settings_.AllowsSpatialAnchorPreview() || !native_)
            return false;
        const auto route = native_->ReadExtensionAdmission(selection_.attempt, XR_MSFT_SPATIAL_ANCHOR_EXTENSION_NAME);
        return route.HasValue() && route.Value().state == NativeExtensionState::Enabled;
    }

    Result<void> OpenXRApplicationSession::SetSpatialAnchorConsent(bool allowed,
                                                                   const std::shared_ptr<const XRSessionLifecycle> &lifecycle) {
        RevokeSpatialAnchor();
        if (!allowed)
            return Result<void>::Success();
        if (spatialAnchor_.scope || !MayArmSpatialAnchor() || !lifecycle)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        if (spatialAnchor_.grant == std::numeric_limits<std::uint64_t>::max())
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        try {
            spatialAnchor_.publication = std::make_shared<XRSpatialAnchors>(lifecycle);
        } catch (const std::bad_alloc &) {
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        }
        ++spatialAnchor_.grant;
        spatialAnchor_.allowed = true;
        return Result<void>::Success();
    }

    void OpenXRApplicationSession::RevokeSpatialAnchor() noexcept {
        spatialAnchor_.allowed = false;
        spatialAnchor_.localized = false;
        spatialAnchor_.sampled = {};
        spatialAnchor_.cutoff = {};
        if (spatialAnchor_.scope && spatialAnchor_.publication)
            (void)spatialAnchor_.publication->Withdraw(*spatialAnchor_.scope, XRAnchorWithdrawal::AccessRevoked);
    }

    Result<void> OpenXRApplicationSession::ProgressSpatialAnchorRetirement() {
        if (!spatialAnchor_.allowed && spatialAnchor_.scope) {
            const auto stopped = native_->StopSpatialAnchors(*spatialAnchor_.scope, XRAnchorWithdrawal::AccessRevoked);
            if (stopped.HasError())
                return stopped;
            spatialAnchor_.scope.reset();
            spatialAnchor_.id = {};
        }
        if (!spatialAnchor_.allowed)
            spatialAnchor_.publication.reset();
        return Result<void>::Success();
    }

    XRCapabilityState OpenXRApplicationSession::ReadSpatialAnchorState() const noexcept {
        if (!spatialAnchor_.allowed)
            return XRCapabilityState::Unavailable;
        const bool fresh = spatialAnchor_.localized && spatialAnchor_.cutoff.Nanoseconds() - spatialAnchor_.sampled.Nanoseconds() <=
                                                           PreviewLimits.maximumSampleAge.ToNanoseconds();
        return fresh ? XRCapabilityState::Available : XRCapabilityState::TemporarilyUnavailable;
    }

    Result<void> OpenXRApplicationSession::StartSpatialAnchor(const XRCoordinateSpace &space, XrSpace world, XRRuntimeTime now) {
        const XRAnchorScope scope{space, XRAnchorProviderGeneration::Create(selection_.attempt.Value()).Value(),
                                  XRAnchorGrant::Create(spatialAnchor_.grant).Value()};
        const auto opened = native_->StartSpatialAnchors(scope, world, PreviewLimits, spatialAnchor_.publication);
        if (opened.HasError())
            return opened;
        spatialAnchor_.scope = scope;
        if (!spatialAnchor_.allowed)
            return Result<void>::Failure(MakeError(XRErrors::PermissionDenied));
        // The preview anchors the calibrated native World origin. No room pose leaves the XR owner.
        const auto created = native_->CreateSpatialAnchor(scope, world, {}, now);
        if (created.HasError())
            return Result<void>::Failure(created.ErrorValue());
        spatialAnchor_.id = created.Value();
        spatialAnchor_.sampled = now;
        return Result<void>::Success();
    }

    void OpenXRApplicationSession::AdvanceSpatialAnchor(const XRTrackingSnapshot &tracking) {
        if (!spatialAnchor_.allowed)
            return;
        if (tracking.Focus() != XRTrackingFocusState::Focused || tracking.State() != XRTrackingState::Active) {
            if (spatialAnchor_.scope)
                RevokeSpatialAnchor();
            return;
        }
        const auto space = WorldSpace(tracking.Session());
        if (space.HasError() || (spatialAnchor_.scope && space.Value() != spatialAnchor_.scope->space)) {
            RevokeSpatialAnchor();
            return;
        }
        const auto world = BorrowWorldSpace(space.Value());
        if (world.HasError() || !spatialAnchor_.allowed) {
            RevokeSpatialAnchor();
            return;
        }
        const auto now = tracking.SampledAt();
        spatialAnchor_.cutoff = now;
        if (!spatialAnchor_.scope) {
            if (StartSpatialAnchor(space.Value(), world.Value(), now).HasError())
                RevokeSpatialAnchor();
            return;
        }
        if (now.Nanoseconds() - spatialAnchor_.sampled.Nanoseconds() < SampleInterval.ToNanoseconds())
            return;
        const auto sampled = native_->SampleSpatialAnchor(*spatialAnchor_.scope, world.Value(), spatialAnchor_.id, now);
        if (sampled.HasError() || !spatialAnchor_.allowed) {
            RevokeSpatialAnchor();
            return;
        }
        const auto anchor = spatialAnchor_.publication->Read(*spatialAnchor_.scope, spatialAnchor_.id, now);
        spatialAnchor_.localized = anchor.HasValue() && anchor.Value().Observation().state == XRAnchorLocalization::Localized;
        spatialAnchor_.sampled = now;
    }
}  // namespace Horo::Application::Internal
