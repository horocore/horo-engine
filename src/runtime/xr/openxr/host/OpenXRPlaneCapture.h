#pragma once

/** @file OpenXRPlaneCapture.h @brief Owner-lane asynchronous native spatial context and plane publication. */
#include "OpenXRNativeSession.h"
#include "OpenXRPlaneSnapshot.h"

namespace Horo::XR::OpenXRInternal {
    /** @brief Single authorized spatial context. The host revokes it before retiring World space or native session. */
    class OpenXRPlaneCapture final {
    public:
        explicit OpenXRPlaneCapture(OpenXRNativeSession &owner) noexcept;
        ~OpenXRPlaneCapture();
        OpenXRPlaneCapture(const OpenXRPlaneCapture &) = delete;
        OpenXRPlaneCapture &operator=(const OpenXRPlaneCapture &) = delete;

        /** @brief Starts one nonblocking context creation for an already consented native session.
         * @param attempt Exact discovery that supplied the active native session.
         * @param scope Current purpose-bound grant and calibrated World identity.
         * @param world The exact native World space borrowed for this grant.
         * @param limits Finite batch, request cadence and age policy.
         * @param now Current chosen runtime-clock time.
         * @return Success once queued, or typed unsupported, permission, identity or native failure. */
        [[nodiscard]] Result<void> Start(XRLoaderPreflightAttempt attempt, const XRPlaneScope &scope, XrSpace world,
                                         const XRPlaneLimits &limits, XRRuntimeTime now);

        /** @brief Advances one step without waiting on a native future.
         * @param scope Exact still-consented scope paired with the borrowed space.
         * @param world The owner's current calibrated native space. A change revokes the grant before polling.
         * @param now Current runtime-clock cutoff.
         * @return True for one complete publication, false for pending/cadence, or typed failure. */
        [[nodiscard]] Result<bool> Advance(const XRPlaneScope &scope, XrSpace world, XRRuntimeTime now);

        /** @brief Copies the currently eligible immutable portable batch.
         * @param scope Exact still-consented scope.
         * @param now Runtime-clock cutoff used for expiry.
         * @return Current complete portable observations or typed absence/staleness. */
        [[nodiscard]] Result<XRPlaneSnapshot> Read(const XRPlaneScope &scope, XRRuntimeTime now) const;

        /** @brief Revokes geometry synchronously without calling native code. */
        void Revoke() noexcept;
        /** @brief Revokes publication immediately and releases pending future, snapshot and context in order.
         * @return Successful retirement, or typed cleanup failure retaining native debt for explicit retry. */
        [[nodiscard]] Result<void> Close();

    private:
        struct Dispatch final {
            PFN_xrCreateSpatialContextAsyncEXT createContext{};
            PFN_xrCreateSpatialContextCompleteEXT completeContext{};
            PFN_xrDestroySpatialContextEXT destroyContext{};
            PFN_xrPollFutureEXT poll{};
            PFN_xrCancelFutureEXT cancel{};
            PFN_xrCreateSpatialDiscoverySnapshotAsyncEXT startScan{};
            PFN_xrCreateSpatialDiscoverySnapshotCompleteEXT completeScan{};
            PFN_xrDestroySpatialSnapshotEXT destroySnapshot{};
            PFN_xrQuerySpatialComponentDataEXT query{};

            [[nodiscard]] bool Resolve(const NativeDiscoveryBorrow &owner);
        };

        struct PlaneSlot final {
            XrSpatialEntityIdEXT entity{};
            std::uint32_t generation{};
        };

        [[nodiscard]] Result<void> BeginContext(const NativeSessionBorrow &native, XRRuntimeTime now);
        [[nodiscard]] Result<bool> Poll(const NativeSessionBorrow &native, XRRuntimeTime now);
        [[nodiscard]] Result<bool> FinishContext(const NativeSessionBorrow &native);
        [[nodiscard]] Result<bool> FinishScan(XRRuntimeTime now);
        /** @brief Reads the completed scan and revokes denied observations before attempting native cleanup. */
        [[nodiscard]] Result<bool> ReadCompletedScan(XRRuntimeTime now);
        [[nodiscard]] Result<bool> QueueScan(XRRuntimeTime now);
        [[nodiscard]] Result<void> Publish(const NativeSpatialPlaneBatch &batch, XRRuntimeTime now);
        [[nodiscard]] Result<void> ReleaseSnapshot();
        [[nodiscard]] Result<void> Retire();
        /** @brief Checks whether the session has withdrawn the capture's owner-lane consent. */
        [[nodiscard]] bool ConsentCurrent() const noexcept;

        OpenXRNativeSession &owner_;
        const std::thread::id lane_{std::this_thread::get_id()};
        Dispatch dispatch_;
        XRPlaneScope scope_;
        XRPlaneLimits limits_;
        NativePlaneCensus census_;
        XRPlanePublication publication_;
        XrInstance instance_{XR_NULL_HANDLE};
        XrSpace world_{XR_NULL_HANDLE}; /**< Borrowed for the exact grant until native retirement. */
        std::uint64_t incarnation_{};
        std::uint64_t observationRevision_{};
        XrFutureEXT future_{XR_NULL_FUTURE_EXT};
        XrSpatialContextEXT context_{XR_NULL_HANDLE};
        XrSpatialSnapshotEXT snapshot_{XR_NULL_HANDLE};
        XRRuntimeTime lastRequest_;
        XRRuntimeTime lastCutoff_;
        std::array<PlaneSlot, XRPlaneSnapshot::MaximumPlanes> slots_{};
        mutable bool busy_{};
    };
}  // namespace Horo::XR::OpenXRInternal
