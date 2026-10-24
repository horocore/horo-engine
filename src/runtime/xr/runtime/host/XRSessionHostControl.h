#pragma once
/** @file XRSessionHostControl.h
 * @brief Non-installed exact-owner retirement bridge for explicit application composition.
 */
#include "Horo/XR/XRFrameLifecycle.h"

namespace Horo::XR::RuntimeInternal {
    /** @brief The SAME session resource adapter's fallible observation of completed physical retirement.
     * @details The host first stops producers and progresses actual native end, image release, scene/UI/GUI
     * leases and native child destruction. An error or false result retains its exact ownership and retry debt.
     * The observer must not perform native work, release resources or re-enter either lifecycle. Success means
     * subsequent reverse Release callbacks only discard already-retired bookkeeping and cannot fail.
     * Owner-lane serialization and lifetime requirements are identical to IXRSessionResources.
     */
    class IXRSessionRetirementResources : public IXRSessionResources {
    public:
        /** @brief Observe actual completed retirement for an exact owned session.
         * @param session Exact candidate whose native running epoch and all queued references must have ended.
         * @return True only after all physical obligations are complete, false while pending, or original cleanup error.
         * @post No ownership, admission or completion state changes; no caller-supplied completion boolean is accepted.
         */
        [[nodiscard]] virtual Result<bool> RetirementComplete(const XRSessionId &session) const = 0;
    };

    /** @brief Narrow host bridge; never issues identity or resets the canonical generation counter. */
    class XRSessionHostControl final {
    public:
        /** @brief Retire one physically drained owner without permanently shutting down its coordinator.
         * @param sessions SAME canonical lifecycle that prepared resources.
         * @param frames SAME lifecycle's frame gate, with no open native/compositor transaction.
         * @param resources SAME resource adapter retained by sessions, including its actual cleanup observation.
         * @param session Exact owned generation; stale, foreign and repeated requests are rejected.
         * @return Success after reverse bookkeeping release; typed rejection or preserved cleanup error otherwise.
         * @pre Serialized owner lane; no callbacks may re-enter either coordinator. Running/Visible/Focused are illegal.
         * Lost is eligible only after explicit physical recovery; retirement never revives that generation.
         * @post Failure leaves ownership/publication/debt unchanged. Success clears the old plan/configuration but
         * preserves both canonical session-generation and frame-sequence issuers. A later Activate issues a higher
         * generation. Public Shutdown remains permanent. Pending/error cleanup must be retried or retained through
         * bounded explicit teardown; abandonment cannot manufacture completion.
         */
        [[nodiscard]] static Result<void> Retire(XRSessionLifecycle &sessions, XRFrameLifecycle &frames,
                                                 const IXRSessionRetirementResources &resources, const XRSessionId &session);
    };
}  // namespace Horo::XR::RuntimeInternal
