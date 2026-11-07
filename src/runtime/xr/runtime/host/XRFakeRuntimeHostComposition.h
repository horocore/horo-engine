#pragma once
/** @file XRFakeRuntimeHostComposition.h @brief Non-installed same-lifecycle tracking composition for explicit simulator hosts. */
#include "Horo/XR/XRFakeRuntime.h"
#include "XRTrackingPublication.h"

#include <memory>
#include <thread>

namespace Horo::XR::RuntimeInternal {
    /** @brief Owns one fake and exact-candidate tracking publications, with no second session/frame authority.
     * Optional resource children are borrowed trusted composition and must outlive this owner. Prepare receives only
     * lifecycle-issued candidates. Each child must clean its failed preparation before returning/throwing, like the
     * canonical IXRSessionResources contract; completed stages receive exact-generation reverse Release once.
     * All calls and destruction use the creating lane. No mutable lifecycle or identity factory is exposed.
     */
    class XRFakeRuntimeHostComposition final : private IXRSessionResources {
    public:
        explicit XRFakeRuntimeHostComposition(IXRSessionResources *children = nullptr) noexcept;
        ~XRFakeRuntimeHostComposition() override;
        XRFakeRuntimeHostComposition(const XRFakeRuntimeHostComposition &) = delete;
        XRFakeRuntimeHostComposition &operator=(const XRFakeRuntimeHostComposition &) = delete;
        /** @brief Prepare tracking against the SAME actual candidate; failure preserves prior active publication. */
        [[nodiscard]] Result<XRSessionId> Activate(const XRCapabilitySnapshot &capabilities, const XRFeaturePlan &plan,
                                                   XRFrameLimits frames, XRTrackingSnapshotLimits tracking, XRWorldOriginRevision origin,
                                                   std::optional<XRSessionPreparation> failStage = {});
        /** @brief Run one supplied prepared step through the existing fake execution and exact-owner fences. */
        [[nodiscard]] Result<XRFakeOutcome> Step(const XRSessionId &session, const XRFakeStep &step);
        /** @brief Observe current sources only after this owner's successful sampled frame. */
        [[nodiscard]] Result<void> ObserveSources(const XRSessionId &session, std::span<const TrackingSourceEvidence> sources);
        /** @brief Atomically publish evidence with the exact successful fake frame's sampling timestamp. */
        [[nodiscard]] Result<void> Publish(const XRSessionId &session, const TrackingPublicationRequest &request);
        /** @brief Applies an explicit observed source disconnection through the existing canonical device issuer.
         * @param session Exact current sampled owner. @param role Known hand/controller/head role.
         * @param state Explicit observed state. @param capabilities Existing exact prepared source capabilities.
         * @return Original typed device transition result; no synthetic replacement identity is guessed.
         */
        [[nodiscard]] Result<XRDeviceId> ObserveDevice(const XRSessionId &session, XRTrackedDeviceRole role, XRTrackedDeviceState state,
                                                       XRTrackedDeviceCapabilities capabilities);
        /** @brief Revoke the current publication on a failed host sampling/binding opportunity; no other owner changes. */
        void RevokeTracking() noexcept;
        [[nodiscard]] std::span<const XRTrackedDeviceRecord> Devices() const noexcept;
        /** @brief Borrow current immutable evidence until the next owner operation; never grants capture consent. */
        [[nodiscard]] const XRTrackingSnapshot *Current() const noexcept;
        [[nodiscard]] XRSessionSnapshot Snapshot() const noexcept;
        /** @brief Close producers and reverse exact owned preparations before parent destruction; idempotent. */
        void Shutdown() noexcept;

    private:
        struct State final {
            XRSessionId session;
            std::unique_ptr<XRTrackingPublication> tracking;
            std::array<bool, static_cast<std::size_t>(XRSessionPreparation::Count)> children{};
        };

        [[nodiscard]] Result<void> Lane() const;
        [[nodiscard]] Result<void> AdmitSample(const XRSessionId &session) const;
        [[nodiscard]] Result<void> Prepare(XRSessionPreparation stage, const XRSessionId &candidate, const XRFeaturePlan &plan) override;
        [[nodiscard]] Result<void> PrepareTracking(const XRSessionId &candidate, const XRFeaturePlan &plan);
        void Release(XRSessionPreparation stage, const XRSessionId &session) noexcept override;
        [[nodiscard]] State *Find(const XRSessionId &session) noexcept;

        struct Operation final {
            bool &busy;

            explicit Operation(bool &value) noexcept : busy(value) {
                busy = true;
            }

            ~Operation() {
                busy = false;
            }
        };

        XRFakeRuntime fake_;
        State active_;
        State candidate_;
        IXRSessionResources *children_{};
        XRTrackingSnapshotLimits nextLimits_{};
        XRWorldOriginRevision nextOrigin_;
        XRRuntimeTime sampledAt_;
        const std::thread::id lane_{std::this_thread::get_id()};
        bool busy_{};
        bool shutdown_{};
    };
}  // namespace Horo::XR::RuntimeInternal
