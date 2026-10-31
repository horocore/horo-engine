#pragma once

/** @file XRInputProjection.h
 * @brief Host-composed scalar projection from the existing immutable XR snapshot into one canonical Input cutoff.
 */
#include "Horo/Runtime/InputCollection.h"
#include "Horo/XR/XRActionBindings.h"

namespace Horo::XR {
    /** @brief Bounded registration adapter; owns no session, snapshot publication, player assignment or input commit.
     * Construct at lifecycle time from a host-bound Input port and the existing resolved plan. Native samplers never
     * receive this port. Reset/replacement requires Input's real neutral commit before a new registration is admitted.
     */
    class XRInputProjection final {
    public:
        XRInputProjection(XRInputProjection &&) noexcept = default;
        XRInputProjection &operator=(XRInputProjection &&) noexcept = default;
        XRInputProjection(const XRInputProjection &) = delete;
        XRInputProjection &operator=(const XRInputProjection &) = delete;
        /** @brief Registers the exact current resolved schemas without creating canonical actions.
         * @param port Host-bound canonical Input context/player port.
         * @param session Exact activated session to which the plan belongs.
         * @param plan Existing resolved binding publication, copied only as bounded role/type metadata.
         * @param bindingRevision Exact non-zero revision of that publication.
         * @return Adapter or typed identity, invalid, capacity or Input registration failure.
         * @throws std::bad_alloc When bounded lifecycle-only descriptor copies cannot be allocated. */
        [[nodiscard]] static Result<XRInputProjection> Create(const Input::InputActionCollection &port, const XRSessionId &session,
                                                              const XRResolvedActionBindings &plan, std::uint64_t bindingRevision);
        /** @brief Reserves exactly one existing Input source before canonical XR candidate issuance.
         * @param port Existing host-bound canonical context/player collection port.
         * @param plan Validated existing resolved product binding plan, with no native path identities.
         * @return Unbound reservation or original Input/schema failure; no Horo session identity is issued.
         * @throws std::bad_alloc For bounded lifecycle descriptor copies.
         * @post Neither a session identity nor binding revision is predicted. Append remains unavailable until Bind succeeds;
         * failure/destruction retains normal neutral-commit policy.
         */
        [[nodiscard]] static Result<XRInputProjection> Prepare(const Input::InputActionCollection &port,
                                                               const XRResolvedActionBindings &plan);
        /** @brief Binds the SAME reserved source to the lifecycle-issued XR candidate without registering again.
         * @param session Exact issued current candidate, supplied only by the host's canonical resource preparation callback.
         * @param bindingRevision Actual nonzero revision published by the canonical binding coordinator for that candidate.
         * @return Success or original typed stale Input reservation/invalid/already-bound failure.
         * @post A failed bind cannot append input or make a revoked reservation fresh.
         */
        [[nodiscard]] Result<void> Bind(const XRSessionId &session, std::uint64_t bindingRevision);
        /** @brief Stages this snapshot's scalar evidence at the canonical simulation input cutoff.
         * @param snapshot Existing XRRuntime publication, never a presentation-late pose.
         * @param activeSession Canonical current session fence.
         * @param expectedSnapshotRevision Exact runtime publication selected for this cutoff.
         * @param activeOriginRevision Canonical current world-origin fence.
         * @param currentBindingRevision Current coordinator plan revision.
         * @param frame Existing Input-owned transaction; this adapter never commits it.
         * @return Success or exact typed provenance/value failure; failure appends no partial values.
         * @post Fixed-capacity success path performs no allocation, native call or player assignment.
         * The host still commits an empty contribution on loss/failure so physical input survives. */
        [[nodiscard]] Result<void> Append(const XRTrackingSnapshot &snapshot, const XRSessionId &activeSession,
                                          XRTrackingSnapshotRevision expectedSnapshotRevision, XRWorldOriginRevision activeOriginRevision,
                                          std::uint64_t currentBindingRevision, const Input::InputCollectionFrame &frame) const;
        /** @brief Revokes the exact registration. @return Input's typed owner-thread/retirement outcome. */
        [[nodiscard]] Result<void> Reset();

    private:
        struct Binding final {
            XRTrackedDeviceRole role{};
            XRTrackedActionKind kind{};
        };

        explicit XRInputProjection(Input::InputActionSourceToken source) : source_(std::move(source)) {}

        Input::InputActionSourceToken source_;
        XRSessionId session_;
        std::uint64_t bindingRevision_{};
        std::array<Binding, XRActionBindingLimits::MaximumActions> bindings_{};
        std::size_t count_{};
    };
}  // namespace Horo::XR
