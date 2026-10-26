#pragma once
/** @file XRTrackingPublication.h
 * @brief Non-installed XRRuntime owner of bounded source generations and simulation snapshot publication.
 */
#include "Horo/XR/XRSessionLifecycle.h"
#include "Horo/XR/XRTrackingSnapshot.h"

#include <optional>
#include <thread>
#include <vector>

namespace Horo::XR::RuntimeInternal {
    /** @brief Normalized backend evidence; no native handle, sample revision or Input action authority. */
    struct TrackingPoseEvidence final {
        XRDeviceId device;
        XRTrackedPoseKind kind;
        XRPoseDescriptor pose;
    };

    /** @brief Normalized physical-source observation before this owner issues its device generation.
     * @details Activity is native binding evidence, not a profile-derived presence or disconnection signal.
     * Pose validity remains independent; inactive observations carry no valid components.
     */
    struct TrackingSourceEvidence final {
        XRTrackedDeviceRole role;
        XRTrackedPoseKind kind;
        bool active{};
        XRPoseDescriptor pose;
    };

    /** @brief One complete simulation-cutoff candidate; caller borrows are consumed synchronously. */
    struct TrackingPublicationRequest final {
        XRTimeEvidence time;
        XRWorldOriginRevision origin;
        XRTrackingFocusState focus;
        XRTrackingVisibilityState visibility;
        XRTrackingState state;
        std::uint64_t bindingRevision{};
        std::span<const TrackingPoseEvidence> poses;
        std::span<const XRTrackedActionRecord> actions;
    };

    /** @brief Owner-lane canonical tracking publication, composed with the SAME canonical session lifecycle.
     * @details Bind runs only inside the lifecycle's resource preparation for its issued candidate. Publish
     * rechecks that lifecycle before sampling can escape. Native adapters provide observations, never Horo
     * device/sample generations. Existing Input projection consumes Current before its collection cutoff;
     * it cannot commit Input here. No native calls, locks, clock reads or per-frame allocations occur.
     */
    class XRTrackingPublication final {
    public:
        /** @brief Borrows the canonical coordinator for this owner's complete lifetime. */
        explicit XRTrackingPublication(XRSessionLifecycle &lifecycle) noexcept;
        XRTrackingPublication(const XRTrackingPublication &) = delete;
        XRTrackingPublication &operator=(const XRTrackingPublication &) = delete;
        /** @brief Prepares bounded storage for an exact issued candidate; never issues session identity.
         * @param candidate Candidate supplied by IXRSessionResources::Prepare.
         * @param limits Accepted plan's bounded tracking storage.
         * @param origin Current canonical world-origin generation.
         * @return Success or typed owner/capacity/invalid failure; failed allocation leaves owner unbound.
         * @throws std::bad_alloc If bounded lifecycle-time pose preparation storage cannot be allocated.
         */
        [[nodiscard]] Result<void> Bind(const XRSessionId &candidate, XRTrackingSnapshotLimits limits, XRWorldOriginRevision origin);
        /** @brief Records an observed head/controller incarnation; replacement revokes the current publication.
         * @param role Exact observed role, restricted to the initial head/left/right profile.
         * @param state Actual connected/tracking state. Disconnected is retained as explicit neutral evidence.
         * @param capabilities Actual prepared native pose capabilities, never fabricated defaults.
         * @return Runtime-issued generation-safe device ID or typed owner/capacity/invalid failure.
         * @post Reconnect/capability replacement advances generation; ordinary loss does not invent a new device.
         */
        [[nodiscard]] Result<XRDeviceId> ObserveDevice(XRTrackedDeviceRole role, XRTrackedDeviceState state,
                                                       XRTrackedDeviceCapabilities capabilities);
        /** @brief Joins one complete native observation packet to the SAME canonical device issuer.
         * @param sources At most five unique head/grip/aim observations from this session's simulation cutoff.
         * @return Success or typed source/session/origin/time/capacity failure; invalid input cannot partly replace devices.
         * @post No absent source is invented. Inactivity retains known devices as TrackingLost, never fabricated Disconnected.
         * Previously observed capabilities survive temporary loss; genuine replacements use ObserveDevice's existing generations.
         * Current is revoked before validation. No snapshot publication, Input commit, native call or successful-path allocation occurs.
         * @pre Owner lane, Bind succeeded; borrows are consumed synchronously and never retained.
         */
        [[nodiscard]] Result<void> ObserveSources(std::span<const TrackingSourceEvidence> sources);
        /** @brief Borrows current canonical source records for one synchronous native sampling operation.
         * @return Bounded canonical slot order; invalidated by ObserveDevice/Bind/Close.
         * @pre Owner-lane serialized call, no retained pointer across a foreign lifecycle operation.
         */
        [[nodiscard]] std::span<const XRTrackedDeviceRecord> Devices() const noexcept;
        /** @brief Validates and publishes one complete cutoff atomically against the actual lifecycle.
         * @param request Normalized observations at the exact native/simulation time and binding generation.
         * @return Success or original typed validation/currentness failure. Failure revokes Current immediately.
         * @post Only success advances the owner-issued snapshot revision/sequence; no partial candidate escapes.
         */
        [[nodiscard]] Result<void> Publish(const TrackingPublicationRequest &request);
        /** @brief Borrows the sole current immutable snapshot. @return Null on failure/loss/shutdown/stale lifecycle.
         * @pre Owner-lane use; the borrow ends at Publish/ObserveDevice/Bind/Close.
         */
        [[nodiscard]] const XRTrackingSnapshot *Current() const noexcept;
        /** @brief Revokes a failed native sampling opportunity without reusing its earlier snapshot.
         * @pre Owner-lane call, before the host commits missing-source neutral Input.
         * @post No sequence advancement, native call, allocation or partial publication.
         */
        void Revoke() noexcept;
        /** @brief Revokes the exact generation without affecting a newer bound owner. @param session Owner being retired. */
        void Close(const XRSessionId &session) noexcept;

    private:
        [[nodiscard]] Result<void> Lane() const;
        [[nodiscard]] Result<void> PreparePoses(const TrackingPublicationRequest &request);
        XRSessionLifecycle *lifecycle_;
        const std::thread::id lane_{std::this_thread::get_id()};
        XRSessionId session_;
        XRTrackingSnapshotLimits limits_;
        XRWorldOriginRevision origin_;
        std::array<XRTrackedDeviceRecord, 3> devices_{};
        std::size_t deviceCount_{};
        std::vector<XRTrackedPoseRecord> poses_;  // Bounded allocation at Bind, retained through all frame work.
        std::optional<XRTrackingSnapshot> current_;
        std::uint64_t revision_{};
    };
}  // namespace Horo::XR::RuntimeInternal
