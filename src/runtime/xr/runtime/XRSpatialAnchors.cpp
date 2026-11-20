#include "Horo/XR/XRSpatialAnchors.h"

#include "Horo/XR/XRSessionErrors.h"

#include <cmath>
#include <limits>

namespace Horo::XR {
    namespace {
        /** @brief Translates the selected provider's capability evidence without guessing about authorization. */
        Result<void> CheckSupport(const XRCapabilityState support) {
            using enum XRCapabilityState;
            switch (support) {
                case Available:
                    return Result<void>::Success();
                case Unsupported:
                    return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
                case PermissionRequired:
                case Denied:
                case DisabledByPolicy:
                    return Result<void>::Failure(MakeError(XRErrors::PermissionDenied));
                case Incompatible:
                    return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
                case Unavailable:
                case DependencyMissing:
                case TemporarilyUnavailable:
                case Lost:
                    return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
                default:
                    return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
            }
        }

        /** @brief Validates generation shape and the stable reference kinds eligible for spatial localization. */
        bool ValidScope(const XRAnchorScope &scope) noexcept {
            const auto &space = scope.space;
            return scope.grant.IsValid() && scope.provider.IsValid() && space.id.IsValid() && space.worldOriginRevision.IsValid() &&
                   space.convention == XRCoordinateConvention::RightHandedYUpNegativeZForward &&
                   (space.kind == XRSpaceKind::Local || space.kind == XRSpaceKind::Stage || space.kind == XRSpaceKind::World);
        }

        /** @brief Rejects absent, non-finite or unnormalized transforms and payload on location-loss states. */
        bool ValidObservation(const XRAnchorObservation &observation) noexcept {
            using enum XRAnchorLocalization;
            switch (observation.state) {
                case Locating:
                case Limited:
                case Lost:
                    return !observation.pose;
                case Localized:
                    break;
                default:
                    return false;
            }
            if (!observation.pose)
                return false;
            const auto &[position, orientation] = *observation.pose;
            const auto &[x, y, z, w] = orientation;
            return Math::IsFinite(position) && std::abs(x * x + y * y + z * z + w * w - 1.0F) <= 0.0001F;
        }
    }  // namespace

    /** @copydoc XRSpatialAnchors::XRSpatialAnchors */
    XRSpatialAnchors::XRSpatialAnchors(const XRSessionLifecycle &lifecycle) noexcept : lifecycle_(lifecycle) {}

    /** @copydoc XRSpatialAnchors::Activate */
    Result<void> XRSpatialAnchors::Activate(const XRAnchorScope &scope, const XRAnchorLimits &limits, const XRCapabilityState support) {
        if (const auto owner = lifecycle_.Admit(scope.space.id.session, true); owner.HasError())
            return owner;
        if (!ValidScope(scope) || limits.maximumAnchors == 0 || limits.maximumSampleAge <= Duration{})
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        if (limits.maximumAnchors > MaximumAnchors || lifecycle_.trackingContinuity_ == 0)
            return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
        if (scope.grant <= scope_.grant)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (const auto available = CheckSupport(support); available.HasError())
            return available;

        slots_ = {};
        scope_ = scope;
        limits_ = limits;
        continuity_ = lifecycle_.trackingContinuity_;
        lastSlotGeneration_ = 0;
        withdrawal_.reset();
        return Result<void>::Success();
    }

    /** @copydoc XRSpatialAnchors::Create */
    Result<XRSpatialAnchorId> XRSpatialAnchors::Create(const XRAnchorScope &scope, const XRRuntimeTime createdAt) {
        using Outcome = Result<XRSpatialAnchorId>;
        if (const auto current = CheckScope(scope); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (!createdAt.IsValid())
            return Outcome::Failure(MakeError(XRErrors::TimeDomainIncompatible));
        if (lastSlotGeneration_ == std::numeric_limits<std::uint32_t>::max())
            return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
        for (std::uint32_t index = 0; index < limits_.maximumAnchors; ++index) {
            auto &slot = slots_[index];
            if (slot.id.IsValid())
                continue;
            slot.id.object_ = {scope.space.id.session, {index, ++lastSlotGeneration_}};
            slot.id.grant_ = scope.grant;
            slot.observation = {.sampledAt = createdAt, .state = XRAnchorLocalization::Locating, .pose = std::nullopt};
            return Outcome::Success(slot.id);
        }
        return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
    }

    /** @copydoc XRSpatialAnchors::Publish */
    Result<void> XRSpatialAnchors::Publish(const XRAnchorScope &scope, const XRSpatialAnchorId &id,
                                           const XRAnchorObservation &observation) {
        const auto found = Find(scope, id);
        if (found.HasError())
            return Result<void>::Failure(found.ErrorValue());
        auto &current = slots_[found.Value()].observation;
        if (observation.sampledAt <= current.sampledAt)
            return Result<void>::Failure(MakeError(XRErrors::TimeDomainIncompatible));
        if (!ValidObservation(observation))
            return Result<void>::Failure(MakeError(XRErrors::PoseInvalid));
        current = observation;
        return Result<void>::Success();
    }

    /** @copydoc XRSpatialAnchors::Read */
    Result<XRAnchorSnapshot> XRSpatialAnchors::Read(const XRAnchorScope &scope, const XRSpatialAnchorId &id,
                                                    const XRRuntimeTime now) const {
        using Outcome = Result<XRAnchorSnapshot>;
        const auto found = Find(scope, id);
        if (found.HasError())
            return Outcome::Failure(found.ErrorValue());
        const auto &current = slots_[found.Value()].observation;
        if (now < current.sampledAt)
            return Outcome::Failure(MakeError(XRErrors::TimeDomainIncompatible));
        if (now.Nanoseconds() - current.sampledAt.Nanoseconds() > limits_.maximumSampleAge.ToNanoseconds())
            return Outcome::Success({id, scope, {current.sampledAt, XRAnchorLocalization::Expired, std::nullopt}});
        return Outcome::Success({id, scope, current});
    }

    /** @copydoc XRSpatialAnchors::Retire */
    Result<void> XRSpatialAnchors::Retire(const XRAnchorScope &scope, const XRSpatialAnchorId &id) {
        const auto found = Find(scope, id);
        if (found.HasError())
            return Result<void>::Failure(found.ErrorValue());
        slots_[found.Value()] = {};
        return Result<void>::Success();
    }

    /** @copydoc XRSpatialAnchors::Withdraw */
    Result<void> XRSpatialAnchors::Withdraw(const XRAnchorScope &scope, const XRAnchorWithdrawal reason) {
        if (!lifecycle_.IsOwnerThread())
            return Result<void>::Failure(MakeError(XRSessionErrors::WrongThread));
        if (!ValidScope(scope) || (reason != XRAnchorWithdrawal::ProviderStopped && reason != XRAnchorWithdrawal::AccessRevoked))
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        if (scope != scope_)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        slots_ = {};
        if (withdrawal_ != XRAnchorWithdrawal::AccessRevoked)
            withdrawal_ = reason;
        return Result<void>::Success();
    }

    /** @brief Checks the canonical control lane and every retained producer and consent generation. */
    Result<void> XRSpatialAnchors::CheckScope(const XRAnchorScope &scope) const {
        if (!lifecycle_.IsOwnerThread())
            return Result<void>::Failure(MakeError(XRSessionErrors::WrongThread));
        if (!ValidScope(scope))
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        if (!scope_.grant.IsValid())
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        if (scope != scope_ || continuity_ != lifecycle_.trackingContinuity_)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (withdrawal_)
            return Result<void>::Failure(
                MakeError(*withdrawal_ == XRAnchorWithdrawal::AccessRevoked ? XRErrors::PermissionDenied : XRErrors::OperationUnavailable));
        return lifecycle_.Admit(scope.space.id.session, true);
    }

    /** @brief Checks opaque slot incarnation after canonical owner validation, before indexing any data. */
    Result<std::size_t> XRSpatialAnchors::Find(const XRAnchorScope &scope, const XRSpatialAnchorId &id) const {
        using Outcome = Result<std::size_t>;
        if (const auto current = CheckScope(scope); current.HasError())
            return Outcome::Failure(current.ErrorValue());
        if (!id.IsValid())
            return Outcome::Failure(MakeError(XRErrors::IdentityInvalid));
        const auto index = id.object_.slot.index;
        if (index >= limits_.maximumAnchors || slots_[index].id != id)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        return Outcome::Success(index);
    }
}  // namespace Horo::XR
