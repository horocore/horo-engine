#include "host/OpenXRNativeSession.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace Horo::XR::OpenXRInternal {
    namespace {
        constexpr auto Extension = XR_MSFT_SPATIAL_ANCHOR_EXTENSION_NAME;

        struct AnchorCall final {
            explicit AnchorCall(bool &active) noexcept : active_(active) {
                active_ = true;
            }

            ~AnchorCall() {
                active_ = false;
            }

            AnchorCall(const AnchorCall &) = delete;
            AnchorCall &operator=(const AnchorCall &) = delete;

        private:
            bool &active_;
        };

        template <typename Function>
        bool ResolveFunction(const NativeSessionBorrow &native, PFN_xrGetInstanceProcAddr getProc, const char *name, Function &function) {
            PFN_xrVoidFunction raw{};
            if (getProc(native.instance, name, &raw) != XR_SUCCESS || !raw)
                return false;
            function = std::bit_cast<Function>(raw);
            return true;
        }

        Error AnchorFailure(const char *operation, XrResult result) {
            switch (result) {
                case XR_ERROR_PERMISSION_INSUFFICIENT:
                    return MakeError(XRErrors::PermissionDenied, operation);
                case XR_ERROR_FUNCTION_UNSUPPORTED:
                case XR_ERROR_FEATURE_UNSUPPORTED:
                    return MakeError(XRErrors::OperationUnsupported, operation);
                case XR_ERROR_LIMIT_REACHED:
                    return MakeError(XRErrors::CapacityExceeded, operation);
                case XR_ERROR_POSE_INVALID:
                    return MakeError(XRErrors::PoseInvalid, operation);
                case XR_ERROR_TIME_INVALID:
                    return MakeError(XRErrors::TimeDomainIncompatible, operation);
                case XR_SESSION_LOSS_PENDING:
                case XR_ERROR_SESSION_LOST:
                case XR_ERROR_INSTANCE_LOST:
                    return MakeError(XRErrors::IdentityStale, operation);
                default:
                    return MakeError(XRErrors::OperationUnavailable, operation);
            }
        }

        bool ValidPose(const XRAnchorPose &pose) noexcept {
            const auto &[x, y, z, w] = pose.orientation;
            return Math::IsFinite(pose.positionMeters) && std::abs(x * x + y * y + z * z + w * w - 1.0F) <= 0.0001F;
        }

        std::optional<XRAnchorWithdrawal> WithdrawalFor(XrResult result) noexcept {
            switch (result) {
                case XR_ERROR_PERMISSION_INSUFFICIENT:
                    return XRAnchorWithdrawal::AccessRevoked;
                case XR_SESSION_LOSS_PENDING:
                case XR_ERROR_SESSION_LOST:
                case XR_ERROR_INSTANCE_LOST:
                    return XRAnchorWithdrawal::ProviderStopped;
                default:
                    return std::nullopt;
            }
        }

        XRAnchorObservation Observation(const XrSpaceLocation &located, XRRuntimeTime now) {
            using enum XRAnchorLocalization;
            constexpr auto valid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            constexpr auto tracked = XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
            if ((located.locationFlags & valid) != valid)
                return {now, Lost, {}};
            if ((located.locationFlags & tracked) != tracked)
                return {now, Limited, {}};
            const auto &pose = located.pose;
            return {now, Localized,
                    XRAnchorPose{{pose.position.x, pose.position.y, pose.position.z},
                                 {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w}}};
        }
    }  // namespace

    bool OpenXRNativeSession::SpatialAnchors::Resolve(const NativeSessionBorrow &native, PFN_xrGetInstanceProcAddr getProc) {
        return ResolveFunction(native, getProc, "xrCreateSpatialAnchorMSFT", create) &&
               ResolveFunction(native, getProc, "xrCreateSpatialAnchorSpaceMSFT", createSpace) &&
               ResolveFunction(native, getProc, "xrDestroySpatialAnchorMSFT", destroy) &&
               ResolveFunction(native, getProc, "xrDestroySpace", destroySpace) &&
               ResolveFunction(native, getProc, "xrLocateSpace", locate);
    }

    Result<void> OpenXRNativeSession::SpatialAnchors::Release(Slot &slot) {
        slot.id = {};
        if (slot.space != XR_NULL_HANDLE) {
            const auto status = destroySpace(slot.space);
            if (XR_FAILED(status))
                return Result<void>::Failure(AnchorFailure("xrDestroySpace", status));
            slot.space = XR_NULL_HANDLE;
        }
        if (slot.anchor != XR_NULL_HANDLE) {
            const auto status = destroy(slot.anchor);
            if (XR_FAILED(status))
                return Result<void>::Failure(AnchorFailure("xrDestroySpatialAnchorMSFT", status));
        }
        slot = {};
        return Result<void>::Success();
    }

    void OpenXRNativeSession::SpatialAnchors::Withdraw(XRAnchorWithdrawal reason) {
        admitted = false;
        withdrawn = true;
        if (const auto live = publication.lock())
            (void)live->Withdraw(scope, reason);
    }

    Result<void> OpenXRNativeSession::StartSpatialAnchors(const XRAnchorScope &scope, XrSpace world, const XRAnchorLimits &limits,
                                                          const std::shared_ptr<XRSpatialAnchors> &publication) {
        if (controlThread_ != std::this_thread::get_id() || retirementCall_ || anchorCall_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (anchors_.scope.grant.IsValid())
            return Result<void>::Failure(MakeError(XRErrors::OperationUnavailable));
        if (!publication || world == XR_NULL_HANDLE || scope.space.kind != XRSpaceKind::World || !scope.grant.IsValid())
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        if (scope.grant <= lastAnchorGrant_)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        const AnchorCall call{anchorCall_};
        anchors_.withdrawn = false;
        const auto native = Borrow(scope.space.id.session);
        if (native.HasError())
            return Result<void>::Failure(native.ErrorValue());
        const auto extension = ReadExtensionAdmission(scope.space.id.session, Extension);
        if (extension.HasError())
            return Result<void>::Failure(extension.ErrorValue());
        if (extension.Value().state == NativeExtensionState::Denied)
            return Result<void>::Failure(MakeError(XRErrors::PermissionDenied));
        if (extension.Value().state == NativeExtensionState::VersionIncompatible)
            return Result<void>::Failure(MakeError(XRErrors::ContractVersionIncompatible));
        if (extension.Value().state != NativeExtensionState::Enabled)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
        if (!anchors_.Resolve(native.Value(), dispatch_.getProc))
            return Result<void>::Failure(MakeError(XRErrors::LoaderIncompatible, "Native anchor dispatch is incomplete"));
        if (anchors_.withdrawn) {
            lastAnchorGrant_ = scope.grant;
            return Result<void>::Failure(MakeError(XRErrors::PermissionDenied));
        }
        const auto opened = publication->Activate(scope, limits, XRCapabilityState::Available);
        if (opened.HasError())
            return opened;
        anchors_.scope = scope;
        anchors_.world = world;
        anchors_.maximumAnchors = limits.maximumAnchors;
        anchors_.publication = publication;
        anchors_.admitted = true;
        lastAnchorGrant_ = scope.grant;
        return Result<void>::Success();
    }

    Result<std::shared_ptr<XRSpatialAnchors>> OpenXRNativeSession::BorrowAnchorPublication(const XRAnchorScope &scope,
                                                                                           XrSpace world) const {
        using Outcome = Result<std::shared_ptr<XRSpatialAnchors>>;
        const auto live = Validate(scope.space.id.session);
        if (live.HasError())
            return Outcome::Failure(live.ErrorValue());
        if (!anchors_.admitted || scope != anchors_.scope || world != anchors_.world)
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        auto publication = anchors_.publication.lock();
        return publication ? Outcome::Success(std::move(publication)) : Outcome::Failure(MakeError(XRErrors::IdentityStale));
    }

    Result<XRSpatialAnchorId> OpenXRNativeSession::CreateSpatialAnchor(const XRAnchorScope &scope, XrSpace world, const XRAnchorPose &pose,
                                                                       XRRuntimeTime now) {
        using Outcome = Result<XRSpatialAnchorId>;
        if (controlThread_ != std::this_thread::get_id() || retirementCall_ || anchorCall_)
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        const AnchorCall call{anchorCall_};
        auto publication = BorrowAnchorPublication(scope, world);
        if (publication.HasError())
            return Outcome::Failure(publication.ErrorValue());
        if (!ValidPose(pose))
            return Outcome::Failure(MakeError(XRErrors::PoseInvalid));
        auto slots = std::span{anchors_.slots}.first(anchors_.maximumAnchors);
        const auto free = std::ranges::find_if(slots, [](const auto &slot) {
            return slot.anchor == XR_NULL_HANDLE && slot.space == XR_NULL_HANDLE;
        });
        if (free == slots.end())
            return Outcome::Failure(MakeError(XRErrors::CapacityExceeded));
        const auto id = publication.Value()->Create(scope, now);
        if (id.HasError())
            return id;
        const auto &[x, y, z, w] = pose.orientation;
        const auto &[px, py, pz] = pose.positionMeters;
        const XrSpatialAnchorCreateInfoMSFT info{XR_TYPE_SPATIAL_ANCHOR_CREATE_INFO_MSFT,
                                                 nullptr,
                                                 world,
                                                 {{x, y, z, w}, {px, py, pz}},
                                                 now.Nanoseconds()};
        XrSpatialAnchorMSFT created{XR_NULL_HANDLE};
        auto status = anchors_.create(session_, &info, &created);
        if (XR_SUCCEEDED(status))
            free->anchor = created;
        if (status == XR_SUCCESS && free->anchor != XR_NULL_HANDLE) {
            const XrSpatialAnchorSpaceCreateInfoMSFT space{XR_TYPE_SPATIAL_ANCHOR_SPACE_CREATE_INFO_MSFT,
                                                           nullptr,
                                                           free->anchor,
                                                           {{0, 0, 0, 1}, {0, 0, 0}}};
            XrSpace located{XR_NULL_HANDLE};
            status = anchors_.createSpace(session_, &space, &located);
            if (XR_SUCCEEDED(status))
                free->space = located;
        }
        const auto current = publication.Value()->Read(scope, id.Value(), now);
        if (status == XR_SUCCESS && free->space != XR_NULL_HANDLE && current.HasValue()) {
            free->id = id.Value();
            free->lastSample = now;
            return id;
        }
        if (const auto reason = WithdrawalFor(status))
            anchors_.Withdraw(*reason);
        else
            (void)publication.Value()->Retire(scope, id.Value());
        if (const auto released = anchors_.Release(*free); released.HasError())
            return Outcome::Failure(released.ErrorValue());
        if (current.HasError())
            return Outcome::Failure(current.ErrorValue());
        return Outcome::Failure(AnchorFailure("OpenXR anchor creation", status));
    }

    Result<void> OpenXRNativeSession::SampleSpatialAnchor(const XRAnchorScope &scope, XrSpace world, const XRSpatialAnchorId &id,
                                                          XRRuntimeTime now) {
        if (controlThread_ != std::this_thread::get_id() || retirementCall_ || anchorCall_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        const AnchorCall call{anchorCall_};
        auto publication = BorrowAnchorPublication(scope, world);
        if (publication.HasError())
            return Result<void>::Failure(publication.ErrorValue());
        const auto current = publication.Value()->Read(scope, id, now);
        if (current.HasError())
            return Result<void>::Failure(current.ErrorValue());
        const auto slot = std::ranges::find(anchors_.slots, id, &SpatialAnchors::Slot::id);
        if (slot == anchors_.slots.end())
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (now <= slot->lastSample)
            return Result<void>::Failure(MakeError(XRErrors::TimeDomainIncompatible));
        XrSpaceLocation located{XR_TYPE_SPACE_LOCATION};
        const auto status = anchors_.locate(slot->space, world, now.Nanoseconds(), &located);
        if (status != XR_SUCCESS) {
            if (const auto reason = WithdrawalFor(status)) {
                anchors_.Withdraw(*reason);
            } else {
                (void)publication.Value()->Publish(scope, id, {now, XRAnchorLocalization::Lost, {}});
                slot->lastSample = now;
            }
            return Result<void>::Failure(AnchorFailure("xrLocateSpace(anchor)", status));
        }
        const bool wellFormed = located.type == XR_TYPE_SPACE_LOCATION && located.next == nullptr;
        const auto observation = wellFormed ? Observation(located, now) : XRAnchorObservation{now, XRAnchorLocalization::Lost, {}};
        const bool validPose = !observation.pose || ValidPose(*observation.pose);
        const auto safe = validPose ? observation : XRAnchorObservation{now, XRAnchorLocalization::Lost, {}};
        const auto result = publication.Value()->Publish(scope, id, safe);
        if (result.HasValue())
            slot->lastSample = now;
        if (!wellFormed)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        return validPose ? result : Result<void>::Failure(MakeError(XRErrors::PoseInvalid));
    }

    Result<void> OpenXRNativeSession::RetireSpatialAnchor(const XRAnchorScope &scope, XrSpace world, const XRSpatialAnchorId &id) {
        if (controlThread_ != std::this_thread::get_id() || retirementCall_ || anchorCall_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        const AnchorCall call{anchorCall_};
        auto publication = BorrowAnchorPublication(scope, world);
        if (publication.HasError())
            return Result<void>::Failure(publication.ErrorValue());
        if (!id.IsValid())
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        const auto slot = std::ranges::find(anchors_.slots, id, &SpatialAnchors::Slot::id);
        if (slot == anchors_.slots.end())
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        const auto retired = publication.Value()->Retire(scope, id);
        if (retired.HasError())
            return retired;
        return anchors_.Release(*slot);
    }

    Result<void> OpenXRNativeSession::StopSpatialAnchors(const XRAnchorScope &scope, XRAnchorWithdrawal reason) {
        if (controlThread_ != std::this_thread::get_id() || retirementCall_ || anchorCall_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (!scope.grant.IsValid() || scope != anchors_.scope)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (reason != XRAnchorWithdrawal::ProviderStopped && reason != XRAnchorWithdrawal::AccessRevoked)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid));
        const AnchorCall call{anchorCall_};
        return RetireSpatialAnchors(reason);
    }

    Result<void> OpenXRNativeSession::RetireSpatialAnchors(XRAnchorWithdrawal reason) {
        if (!anchors_.scope.grant.IsValid())
            return Result<void>::Success();
        anchors_.Withdraw(reason);
        if (const auto store = RetireSpatialAnchorStore(); store.HasError())
            return store;
        for (auto &slot : std::span{anchors_.slots}.first(anchors_.maximumAnchors))
            if (const auto released = anchors_.Release(slot); released.HasError())
                return released;
        anchors_ = {};
        return Result<void>::Success();
    }

    /** @copydoc OpenXRNativeSession::RevokeCompositionObservations */
    void OpenXRNativeSession::RevokeCompositionObservations() {
        if (observationRevision_ != std::numeric_limits<std::uint64_t>::max())
            ++observationRevision_;
        RevokeLightEstimation();
        anchorStore_.admitted = false;
        anchors_.Withdraw(XRAnchorWithdrawal::AccessRevoked);
        RevokePassthrough();
    }
}  // namespace Horo::XR::OpenXRInternal
