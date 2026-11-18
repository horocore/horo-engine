#include "Horo/XR/XRPlaneTracking.h"

#include <cmath>
#include <utility>

namespace Horo::XR {
    namespace {
        /** @brief Validates the application-owned plane clearance and coordinate identity. */
        bool ValidScope(const XRPlaneScope &scope) noexcept {
            return scope.session.IsValid() && scope.grant.IsValid() && scope.space.id.IsValid() &&
                   scope.space.id.session == scope.session && scope.space.kind == XRSpaceKind::World &&
                   scope.space.worldOriginRevision.IsValid() &&
                   scope.space.convention == XRCoordinateConvention::RightHandedYUpNegativeZForward;
        }

        /** @brief Validates channel values without fabricating confidence or spatial dimensions. */
        bool ValidPlane(const XRPlaneObservation &plane, const XRSessionId &session) noexcept {
            return plane.id.IsValid() && plane.id.session == session && Math::IsFinite(plane.centerMeters) &&
                   std::isfinite(plane.extentMeters.x) && plane.extentMeters.x > 0.0F && std::isfinite(plane.extentMeters.y) &&
                   plane.extentMeters.y > 0.0F && plane.classification < XRPlaneClassification::Count &&
                   (plane.confidence == XRTrackingConfidence::Low || plane.confidence == XRTrackingConfidence::High);
        }
    }  // namespace

    /** @copydoc XRPlanePublication::Activate */
    Result<void> XRPlanePublication::Activate(const XRPlaneScope &scope, const XRPlaneLimits &limits) {
        if (!ValidScope(scope) || limits.maximumPlanes == 0 || limits.minimumUpdateInterval <= Duration{} ||
            limits.maximumAge <= Duration{})
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        if (limits.maximumPlanes > XRPlaneSnapshot::MaximumPlanes)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        if (active_.grant.IsValid())
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (scope.grant <= lastGrant_)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        active_ = scope;
        lastGrant_ = scope.grant;
        limits_ = limits;
        return Result<void>::Success();
    }

    /** @copydoc XRPlanePublication::IsDue */
    Result<bool> XRPlanePublication::IsDue(const XRPlaneScope &scope, XRRuntimeTime now) const {
        if (!active_.grant.IsValid())
            return Result<bool>::Failure(MakeError(XRErrors::OperationUnavailable));
        if (scope != active_)
            return Result<bool>::Failure(MakeError(XRErrors::IdentityStale));
        if (!now.IsValid() || (current_ && now < current_->SampledAt()))
            return Result<bool>::Failure(MakeError(XRErrors::TimeDomainIncompatible));
        return Result<bool>::Success(!current_ || now.Nanoseconds() - current_->SampledAt().Nanoseconds() >=
                                                      limits_.minimumUpdateInterval.ToNanoseconds());
    }

    /** @copydoc XRPlanePublication::Publish */
    Result<void> XRPlanePublication::Publish(const XRPlaneScope &scope, XRRuntimeTime sampledAt,
                                             std::span<const XRPlaneObservation> planes) {
        const auto due = IsDue(scope, sampledAt);
        if (due.HasError())
            return Result<void>::Failure(due.ErrorValue());
        if (!due.Value())
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        if (planes.size() > limits_.maximumPlanes)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));

        XRPlaneSnapshot candidate;
        candidate.scope_ = scope;
        candidate.sampledAt_ = sampledAt;
        for (const auto &plane : planes) {
            if (!ValidPlane(plane, scope.session) || (candidate.count_ != 0 && candidate.planes_[candidate.count_ - 1].id >= plane.id))
                return Result<void>::Failure(MakeError(XRErrors::TrackingSnapshotInvalid));
            const auto orientation = plane.orientation.TryNormalized();
            if (orientation.HasError())
                return Result<void>::Failure(MakeError(XRErrors::PoseInvalid));
            auto &stored = candidate.planes_[candidate.count_++];
            stored = plane;
            stored.orientation = orientation.Value();
        }
        current_ = std::move(candidate);
        return Result<void>::Success();
    }

    /** @copydoc XRPlanePublication::Read */
    Result<XRPlaneSnapshot> XRPlanePublication::Read(const XRPlaneScope &scope, XRRuntimeTime now) const {
        using Outcome = Result<XRPlaneSnapshot>;
        if (!active_.grant.IsValid())
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        if (scope != active_)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        if (!current_)
            return Outcome::Failure(MakeError(XRErrors::OperationUnavailable));
        const auto sample = current_->SampledAt();
        if (!now.IsValid() || now < sample)
            return Outcome::Failure(MakeError(XRErrors::TimeDomainIncompatible));
        if (now.Nanoseconds() - sample.Nanoseconds() > limits_.maximumAge.ToNanoseconds())
            return Outcome::Failure(MakeError(XRErrors::TrackingSnapshotStale));
        return Outcome::Success(*current_);
    }

    /** @copydoc XRPlanePublication::Revoke */
    void XRPlanePublication::Revoke() noexcept {
        current_.reset();
        active_ = {};
        limits_ = {};
    }
}  // namespace Horo::XR
