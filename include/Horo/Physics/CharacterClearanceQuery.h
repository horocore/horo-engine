#pragma once

/** @file CharacterClearanceQuery.h
 * @brief Bounded production Character clearance through generation-affine Physics capabilities.
 */

#include "Horo/Physics/CharacterControllerContracts.h"
#include "Horo/Physics/PhysicsQueryEventCapability.h"

namespace Horo::Physics {
    /** @brief Owned per-operation capsule-clearance adapter with no borrowed PhysicsWorld pointer.
     * Capture on the Physics owner thread, then keep this object stationary until the synchronous
     * Character operation completes. Context is borrowed and must not outlive or survive a move of
     * this object. No native shape, body, allocation or depenetration is introduced by a probe.
     * This adapter supplies clearance only; overlapping spawn recovery requires a recovery adapter.
     * Retained capabilities preserve revocation/stale-world errors after module/world shutdown.
     */
    class CharacterClearanceQuery final {
    public:
        /** @brief Captures one exact operation identity without querying or mutating Physics.
         * @param capability Host-admitted Physics client, including its module cancellation fence.
         * @param expected Exact Character/Physics generations, tick and Physics publication revision.
         * @return Owned adapter or a typed identity/snapshot error.
         */
        [[nodiscard]] static Result<CharacterClearanceQuery> Capture(PhysicsQueryEventCapability capability,
                                                                     Character::CharacterPhysicsQueryExpectations expected);
        /** @brief Borrows the clearance context for one synchronous owner-thread operation.
         * @return Context valid until this adapter is destroyed or moved; no sweep/recovery is supplied.
         */
        [[nodiscard]] Character::CharacterPhysicsQueryContext Context() & noexcept;

    private:
        /** @brief Stores an already admitted capability and immutable operation identity. */
        CharacterClearanceQuery(PhysicsQueryEventCapability capability, Character::CharacterPhysicsQueryExpectations expected);
        /** @brief Reduces one exact operation to blocking presence without depenetration. */
        static Result<Character::CharacterOverlapProbeResult> Probe(void *context,
                                                                    const Character::CharacterOverlapProbeRequest &request) noexcept;
        PhysicsQueryEventCapability capability_;
        Character::CharacterPhysicsQueryExpectations expected_;
    };
}  // namespace Horo::Physics
