#include "host/OpenXRPlaneCapture.h"

#include "Horo/Foundation/Assertions.h"

#include <algorithm>
#include <bit>
#include <limits>

namespace Horo::XR::OpenXRInternal {
    namespace {
        /** @brief Guards against reentrant native operations on the control lane. */
        struct ScopedOperation final {
            explicit ScopedOperation(bool &busy) : busy_(busy) {
                busy_ = true;
            }

            ~ScopedOperation() {
                busy_ = false;
            }

            bool &busy_;
        };

        /** @brief Keeps native errors actionable without retaining geometry or runtime entity IDs. */
        Error CaptureError(const char *operation, const XrResult code) {
            switch (code) {
                case XR_ERROR_PERMISSION_INSUFFICIENT:
                    return MakeError(XRErrors::PermissionDenied, operation);
                case XR_SESSION_LOSS_PENDING:
                case XR_ERROR_SESSION_LOST:
                case XR_ERROR_INSTANCE_LOST:
                    return MakeError(XRErrors::IdentityStale, operation);
                case XR_ERROR_FUNCTION_UNSUPPORTED:
                    return MakeError(XRErrors::OperationUnsupported, operation);
                default:
                    return MakeError(XRErrors::OperationUnavailable, operation);
            }
        }

        /** @brief Resolves one enabled native symbol from the exact instance's loader. */
        template <typename Function> bool Load(const NativeDiscoveryBorrow &owner, const char *name, Function &output) {
            PFN_xrVoidFunction function{};
            if (owner.getProc(owner.instance, name, &function) != XR_SUCCESS || !function)
                return false;
            output = std::bit_cast<Function>(function);
            return true;
        }
    }  // namespace

    /** @brief Resolves the complete native ownership and dispatch route before any child is created. */
    bool OpenXRPlaneCapture::Dispatch::Resolve(const NativeDiscoveryBorrow &owner) {
        return Load(owner, "xrCreateSpatialContextAsyncEXT", createContext) &&
               Load(owner, "xrCreateSpatialContextCompleteEXT", completeContext) &&
               Load(owner, "xrDestroySpatialContextEXT", destroyContext) && Load(owner, "xrPollFutureEXT", poll) &&
               Load(owner, "xrCancelFutureEXT", cancel) && Load(owner, "xrCreateSpatialDiscoverySnapshotAsyncEXT", startScan) &&
               Load(owner, "xrCreateSpatialDiscoverySnapshotCompleteEXT", completeScan) &&
               Load(owner, "xrDestroySpatialSnapshotEXT", destroySnapshot) && Load(owner, "xrQuerySpatialComponentDataEXT", query);
    }

    OpenXRPlaneCapture::OpenXRPlaneCapture(OpenXRNativeSession &owner) noexcept : owner_(owner) {}

    OpenXRPlaneCapture::~OpenXRPlaneCapture() {
        HORO_INVARIANT_MSG(!instance_, "Retire the native spatial context before destroying its scope owner");
    }

    /** @copydoc OpenXRPlaneCapture::Start */
    Result<void> OpenXRPlaneCapture::Start(XRLoaderPreflightAttempt attempt, const XRPlaneScope &scope, XrSpace world,
                                           const XRPlaneLimits &limits, XRRuntimeTime now) {
        if (std::this_thread::get_id() != lane_ || busy_ || instance_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        if (world == XR_NULL_HANDLE)
            return Result<void>::Failure(MakeError(XRErrors::CoordinateSpaceIncompatible));
        if (!now.IsValid())
            return Result<void>::Failure(MakeError(XRErrors::TimeDomainIncompatible));
        const ScopedOperation guard{busy_};
        observationRevision_ = owner_.observationRevision_;
        const auto owner = owner_.Borrow(scope.session);
        if (owner.HasError())
            return Result<void>::Failure(owner.ErrorValue());
        const auto discovery = owner_.BorrowFeatureDiscovery(attempt, scope.session);
        if (discovery.HasError())
            return Result<void>::Failure(discovery.ErrorValue());
        if (discovery.Value().instance != owner.Value().instance)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        const auto census = owner_.DiscoverPlaneProvider(attempt, scope.session);
        if (census.HasError())
            return Result<void>::Failure(census.ErrorValue());
        const auto feature = census.Value();
        if (feature.state == XRCapabilityState::Denied)
            return Result<void>::Failure(MakeError(XRErrors::PermissionDenied));
        if (feature.state != XRCapabilityState::Available)
            return Result<void>::Failure(MakeError(XRErrors::OperationUnsupported));
        Dispatch functions;
        if (!functions.Resolve(discovery.Value()))
            return Result<void>::Failure(MakeError(XRErrors::LoaderIncompatible, "Spatial plane dispatch is incomplete"));
        const auto current = owner_.Borrow(scope.session);
        if (current.HasError() || current.Value().incarnation != owner.Value().incarnation)
            return Result<void>::Failure(MakeError(XRErrors::IdentityStale));
        if (const auto activated = publication_.Activate(scope, limits); activated.HasError())
            return activated;
        if (!ConsentCurrent()) {
            publication_.Revoke();
            return Result<void>::Failure(MakeError(XRErrors::PermissionDenied));
        }

        dispatch_ = functions;
        scope_ = scope;
        limits_ = limits;
        census_ = feature;
        instance_ = owner.Value().instance;
        world_ = world;
        incarnation_ = owner.Value().incarnation;
        lastCutoff_ = now;
        auto started = BeginContext(current.Value(), now);
        if (started.HasValue() && !ConsentCurrent())
            started = Result<void>::Failure(MakeError(XRErrors::PermissionDenied));
        if (started.HasError()) {
            publication_.Revoke();
            if (future_ == XR_NULL_FUTURE_EXT)
                static_cast<void>(Retire());
        }
        return started;
    }

    /** @brief Queues an exact plane-only context with bounded rectangle and optional semantic components. */
    Result<void> OpenXRPlaneCapture::BeginContext(const NativeSessionBorrow &native, XRRuntimeTime now) {
        constexpr std::array components{XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT, XR_SPATIAL_COMPONENT_TYPE_PLANE_SEMANTIC_LABEL_EXT};
        const XrSpatialCapabilityConfigurationPlaneTrackingEXT config{XR_TYPE_SPATIAL_CAPABILITY_CONFIGURATION_PLANE_TRACKING_EXT, nullptr,
                                                                      XR_SPATIAL_CAPABILITY_PLANE_TRACKING_EXT,
                                                                      census_.semanticLabels ? 2U : 1U, components.data()};
        const auto *descriptor = reinterpret_cast<const XrSpatialCapabilityConfigurationBaseHeaderEXT *>(&config);
        const XrSpatialContextCreateInfoEXT info{XR_TYPE_SPATIAL_CONTEXT_CREATE_INFO_EXT, nullptr, 1, &descriptor};
        const auto result = dispatch_.createContext(native.session, &info, &future_);
        lastRequest_ = now;
        if (result != XR_SUCCESS)
            return Result<void>::Failure(CaptureError("xrCreateSpatialContextAsyncEXT", result));
        if (future_ == XR_NULL_FUTURE_EXT)
            return Result<void>::Failure(MakeError(XRErrors::OperationInvalid, "Spatial context returned no future"));
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlaneCapture::Advance */
    Result<bool> OpenXRPlaneCapture::Advance(const XRPlaneScope &scope, XrSpace world, XRRuntimeTime now) {
        using Outcome = Result<bool>;
        if (std::this_thread::get_id() != lane_ || busy_ || !instance_)
            return Outcome::Failure(MakeError(XRErrors::OperationIncompatible));
        const ScopedOperation guard{busy_};
        if (scope != scope_) {
            publication_.Revoke();
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        }
        if (world != world_) {
            publication_.Revoke();
            return Outcome::Failure(MakeError(XRErrors::CoordinateSpaceIncompatible));
        }
        if (!now.IsValid() || now < lastCutoff_)
            return Outcome::Failure(MakeError(XRErrors::TimeDomainIncompatible));
        const auto native = owner_.Borrow(scope.session);
        if (native.HasError() || native.Value().incarnation != incarnation_) {
            publication_.Revoke();
            return Outcome::Failure(MakeError(XRErrors::IdentityStale));
        }
        if (!ConsentCurrent()) {
            publication_.Revoke();
            return Outcome::Failure(MakeError(XRErrors::PermissionDenied));
        }
        const auto active = publication_.IsDue(scope, now);
        if (active.HasError())
            return Outcome::Failure(active.ErrorValue());
        lastCutoff_ = now;
        if (const auto released = ReleaseSnapshot(); released.HasError())
            return Outcome::Failure(released.ErrorValue());
        if (!ConsentCurrent()) {
            publication_.Revoke();
            return Outcome::Failure(MakeError(XRErrors::PermissionDenied));
        }
        if (future_ != XR_NULL_FUTURE_EXT)
            return Poll(native.Value(), now);
        if (!context_ || !active.Value())
            return Outcome::Success(false);
        if (now.Nanoseconds() - lastRequest_.Nanoseconds() < limits_.minimumUpdateInterval.ToNanoseconds())
            return Outcome::Success(false);
        return QueueScan(now);
    }

    /** @brief Polls once and retains ownership of pending work until completion or explicit cancellation. */
    Result<bool> OpenXRPlaneCapture::Poll(const NativeSessionBorrow &native, XRRuntimeTime now) {
        using Outcome = Result<bool>;
        if (now.Nanoseconds() - lastRequest_.Nanoseconds() > limits_.maximumAge.ToNanoseconds()) {
            publication_.Revoke();
            const auto retired = Retire();
            return Outcome::Failure(retired.HasError() ? retired.ErrorValue() : MakeError(XRErrors::TrackingSnapshotStale));
        }
        const XrFuturePollInfoEXT request{XR_TYPE_FUTURE_POLL_INFO_EXT, nullptr, future_};
        XrFuturePollResultEXT response{XR_TYPE_FUTURE_POLL_RESULT_EXT};
        const auto result = dispatch_.poll(instance_, &request, &response);
        if (result != XR_SUCCESS) {
            publication_.Revoke();
            return Outcome::Failure(CaptureError("xrPollFutureEXT", result));
        }
        if (!ConsentCurrent()) {
            publication_.Revoke();
            return Outcome::Failure(MakeError(XRErrors::PermissionDenied));
        }
        if (response.state == XR_FUTURE_STATE_PENDING_EXT)
            return Outcome::Success(false);
        if (response.type != XR_TYPE_FUTURE_POLL_RESULT_EXT || response.next || response.state != XR_FUTURE_STATE_READY_EXT) {
            publication_.Revoke();
            return Outcome::Failure(MakeError(XRErrors::OperationInvalid, "Malformed spatial future state"));
        }
        if (const auto admitted = publication_.IsDue(scope_, now); admitted.HasError())
            return Outcome::Failure(MakeError(XRErrors::PermissionDenied));
        return context_ ? FinishScan(now) : FinishContext(native);
    }

    /** @brief Consumes a ready context future and preserves any returned native handle for cleanup. */
    Result<bool> OpenXRPlaneCapture::FinishContext(const NativeSessionBorrow &native) {
        using Outcome = Result<bool>;
        XrCreateSpatialContextCompletionEXT completed{XR_TYPE_CREATE_SPATIAL_CONTEXT_COMPLETION_EXT};
        const auto result = dispatch_.completeContext(native.session, future_, &completed);
        if (XR_SUCCEEDED(result)) {
            context_ = completed.spatialContext;
            future_ = XR_NULL_FUTURE_EXT;
        }
        if (result != XR_SUCCESS || completed.futureResult != XR_SUCCESS) {
            publication_.Revoke();
            return Outcome::Failure(
                CaptureError("xrCreateSpatialContextCompleteEXT", result != XR_SUCCESS ? result : completed.futureResult));
        }
        if (!context_)
            return Outcome::Failure(MakeError(XRErrors::OperationInvalid, "Spatial context completion returned no context"));
        return Outcome::Success(false);
    }

    /** @brief Starts one immutable discovery request from the admitted spatial context. */
    Result<bool> OpenXRPlaneCapture::QueueScan(XRRuntimeTime now) {
        constexpr auto component = XR_SPATIAL_COMPONENT_TYPE_BOUNDED_2D_EXT;
        const XrSpatialDiscoverySnapshotCreateInfoEXT request{XR_TYPE_SPATIAL_DISCOVERY_SNAPSHOT_CREATE_INFO_EXT, nullptr, 1, &component};
        lastRequest_ = now;
        const auto result = dispatch_.startScan(context_, &request, &future_);
        if (result != XR_SUCCESS) {
            const auto failure = CaptureError("xrCreateSpatialDiscoverySnapshotAsyncEXT", result);
            if (failure.code.Value() == XRErrors::PermissionDenied.code.Value() ||
                failure.code.Value() == XRErrors::IdentityStale.code.Value())
                publication_.Revoke();
            return Result<bool>::Failure(failure);
        }
        if (future_ == XR_NULL_FUTURE_EXT)
            return Result<bool>::Failure(MakeError(XRErrors::OperationInvalid, "Spatial snapshot returned no future"));
        return Result<bool>::Success(false);
    }

    /** @brief Resolves the native snapshot in the current calibrated space and publishes only after cleanup succeeds. */
    Result<bool> OpenXRPlaneCapture::FinishScan(XRRuntimeTime now) {
        using Outcome = Result<bool>;
        const XrCreateSpatialDiscoverySnapshotCompletionInfoEXT request{XR_TYPE_CREATE_SPATIAL_DISCOVERY_SNAPSHOT_COMPLETION_INFO_EXT,
                                                                        nullptr, world_, now.Nanoseconds(), future_};
        XrCreateSpatialDiscoverySnapshotCompletionEXT completed{XR_TYPE_CREATE_SPATIAL_DISCOVERY_SNAPSHOT_COMPLETION_EXT};
        const auto result = dispatch_.completeScan(context_, &request, &completed);
        if (XR_SUCCEEDED(result)) {
            snapshot_ = completed.snapshot;
            future_ = XR_NULL_FUTURE_EXT;
        }
        if (result != XR_SUCCESS || completed.futureResult != XR_SUCCESS) {
            publication_.Revoke();
            return Outcome::Failure(
                CaptureError("xrCreateSpatialDiscoverySnapshotCompleteEXT", result != XR_SUCCESS ? result : completed.futureResult));
        }
        const NativePlaneReadAuthorization authorization{this, [](const void *context) {
            const auto &capture = *static_cast<const OpenXRPlaneCapture *>(context);
            return capture.ConsentCurrent() && capture.publication_.IsDue(capture.scope_, capture.lastCutoff_).HasValue();
        }};
        const auto batch = ReadNativeSpatialPlanes(snapshot_, dispatch_.query, census_, limits_.maximumPlanes, authorization);
        const auto destroyed = ReleaseSnapshot();
        if (destroyed.HasError())
            return Outcome::Failure(destroyed.ErrorValue());
        if (!ConsentCurrent()) {
            publication_.Revoke();
            return Outcome::Failure(MakeError(XRErrors::PermissionDenied));
        }
        if (batch.HasError()) {
            const auto &failure = batch.ErrorValue();
            if (failure.code.Value() == XRErrors::PermissionDenied.code.Value() ||
                failure.code.Value() == XRErrors::IdentityStale.code.Value())
                publication_.Revoke();
            return Outcome::Failure(failure);
        }
        const auto published = Publish(batch.Value(), now);
        return published.HasError() ? Outcome::Failure(published.ErrorValue()) : Outcome::Success(true);
    }

    /** @brief Stages stable portable identities and replaces both catalog and snapshot atomically on success. */
    Result<void> OpenXRPlaneCapture::Publish(const NativeSpatialPlaneBatch &batch, XRRuntimeTime now) {
        auto slots = slots_;
        const auto planes = batch.Planes();
        for (auto &slot : slots)
            if (std::ranges::find(planes, slot.entity, &NativeSpatialPlane::entity) == planes.end())
                slot.entity = XR_NULL_SPATIAL_ENTITY_ID_EXT;
        std::array<XRPlaneObservation, XRPlaneSnapshot::MaximumPlanes> observations{};
        std::size_t count{};
        const auto live = std::span{slots}.first(limits_.maximumPlanes);
        for (const auto &plane : planes) {
            auto slot = std::ranges::find(live, plane.entity, &PlaneSlot::entity);
            if (slot == live.end()) {
                slot = std::ranges::find_if(live, [](const PlaneSlot &candidate) {
                    return candidate.entity == XR_NULL_SPATIAL_ENTITY_ID_EXT &&
                           candidate.generation < std::numeric_limits<std::uint32_t>::max();
                });
                if (slot == live.end())
                    return Result<void>::Failure(MakeError(XRErrors::CapacityExceeded));
                slot->entity = plane.entity;
                ++slot->generation;
            }
            observations[count++] = {{scope_.session, {static_cast<std::uint32_t>(slot - live.begin()), slot->generation}},
                                     plane.centerMeters,
                                     plane.orientation,
                                     plane.extentMeters,
                                     plane.classification,
                                     plane.confidence};
        }
        const auto used = std::span{observations}.first(count);
        std::ranges::sort(used, {}, &XRPlaneObservation::id);
        const auto published = publication_.Publish(scope_, now, used);
        if (published.HasValue())
            slots_ = slots;
        return published;
    }

    /** @copydoc OpenXRPlaneCapture::Read */
    Result<XRPlaneSnapshot> OpenXRPlaneCapture::Read(const XRPlaneScope &scope, XRRuntimeTime now) const {
        if (std::this_thread::get_id() != lane_ || busy_)
            return Result<XRPlaneSnapshot>::Failure(MakeError(XRErrors::OperationIncompatible));
        const ScopedOperation guard{busy_};
        const auto native = owner_.Borrow(scope.session);
        if (native.HasError() || native.Value().incarnation != incarnation_)
            return Result<XRPlaneSnapshot>::Failure(MakeError(XRErrors::IdentityStale));
        if (!ConsentCurrent())
            return Result<XRPlaneSnapshot>::Failure(MakeError(XRErrors::PermissionDenied));
        return publication_.Read(scope, now);
    }

    /** @copydoc OpenXRPlaneCapture::ConsentCurrent */
    bool OpenXRPlaneCapture::ConsentCurrent() const noexcept {
        return observationRevision_ == owner_.observationRevision_ && observationRevision_ != std::numeric_limits<std::uint64_t>::max();
    }

    /** @brief Keeps unfinished snapshot destruction retryable without returning unretired geometry. */
    Result<void> OpenXRPlaneCapture::ReleaseSnapshot() {
        if (!snapshot_)
            return Result<void>::Success();
        const auto result = dispatch_.destroySnapshot(snapshot_);
        if (result != XR_SUCCESS)
            return Result<void>::Failure(CaptureError("xrDestroySpatialSnapshotEXT", result));
        snapshot_ = XR_NULL_HANDLE;
        return Result<void>::Success();
    }

    /** @brief Cancels futures and releases snapshot/context, retaining a failed child for explicit retry. */
    Result<void> OpenXRPlaneCapture::Retire() {
        if (future_ != XR_NULL_FUTURE_EXT) {
            const XrFutureCancelInfoEXT request{XR_TYPE_FUTURE_CANCEL_INFO_EXT, nullptr, future_};
            const auto result = dispatch_.cancel(instance_, &request);
            if (result != XR_SUCCESS && result != XR_ERROR_FUTURE_INVALID_EXT)
                return Result<void>::Failure(CaptureError("xrCancelFutureEXT", result));
            future_ = XR_NULL_FUTURE_EXT;
        }
        if (const auto released = ReleaseSnapshot(); released.HasError())
            return released;
        if (context_) {
            const auto result = dispatch_.destroyContext(context_);
            if (result != XR_SUCCESS)
                return Result<void>::Failure(CaptureError("xrDestroySpatialContextEXT", result));
            context_ = XR_NULL_HANDLE;
        }
        for (auto &slot : slots_)
            slot.entity = XR_NULL_SPATIAL_ENTITY_ID_EXT;
        instance_ = XR_NULL_HANDLE;
        world_ = XR_NULL_HANDLE;
        scope_ = {};
        incarnation_ = 0;
        return Result<void>::Success();
    }

    /** @copydoc OpenXRPlaneCapture::Revoke */
    void OpenXRPlaneCapture::Revoke() noexcept {
        if (std::this_thread::get_id() == lane_)
            publication_.Revoke();
    }

    /** @copydoc OpenXRPlaneCapture::Close */
    Result<void> OpenXRPlaneCapture::Close() {
        if (std::this_thread::get_id() != lane_ || busy_)
            return Result<void>::Failure(MakeError(XRErrors::OperationIncompatible));
        const ScopedOperation guard{busy_};
        publication_.Revoke();
        return Retire();
    }
}  // namespace Horo::XR::OpenXRInternal
