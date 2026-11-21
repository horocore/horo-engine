#pragma once

/** @file CharacterSceneFixedAccess.h
 * @brief Non-installed Character fixed-tick transaction for the Scene owner.
 */
#include "Horo/Physics/CharacterWorld.h"

namespace Horo::Character::Detail {
    /** @brief Matches one attempt within the exact borrowed Character owner. */
    struct CharacterSceneTickAttempt final {
        std::uint64_t tick{};    /**< Next local simulation tick of this Character world. */
        std::uint64_t attempt{}; /**< Attempt from the owning scheduler, including retries. */

        [[nodiscard]] constexpr auto operator<=>(const CharacterSceneTickAttempt &) const noexcept = default;
    };

    /** @brief Synchronous input from the paired Scene Physics owner. */
    struct CharacterSceneFixedInput final {
        Duration delta;
        CharacterPhysicsQueryContext query;
        std::span<const Physics::PhysicsKinematicTargetCommand> platformTargets;
    };

    /** @brief Splits the existing command solver from its publication for scheduler-owned composition. */
    class CharacterSceneFixedAccess final {
    public:
        /** @brief Freezes the exact queue and resolves movement without publishing. The world owns one live candidate.
         * @param world Exact paired Character owner. @param attempt The scheduler's current attempt for the local tick.
         * @param input Fixed quantum and paired Physics evidence borrowed for this call.
         * @return Success or a typed failure. A failure releases the tick fence and preserves queued intents.
         */
        [[nodiscard]] static Result<void> Stage(CharacterWorld &world, CharacterSceneTickAttempt attempt,
                                                const CharacterSceneFixedInput &input);

        /** @brief Copies one staged root-motion result for Animation's same-attempt feedback.
         * @param world Exact paired owner. @param attempt Current staged attempt. @param controller Exact resident identity.
         * @return Copied feedback, empty if no root motion was solved, or a typed stale/lifecycle error.
         */
        [[nodiscard]] static Result<std::optional<CharacterRootMotionFeedback>> ReadRootMotion(const CharacterWorld &world,
                                                                                               CharacterSceneTickAttempt attempt,
                                                                                               CharacterControllerHandle controller);

        /** @brief Publishes the preflighted candidate after the scheduler commitment fence.
         * @param world Exact active paired owner. @param attempt Current staged attempt. A mismatched caller breaks the commit invariant.
         */
        static void Commit(CharacterWorld &world, CharacterSceneTickAttempt attempt) noexcept;

        /** @brief Releases the exact matching candidate and retains queued intents for retry.
         * @param world Exact paired owner. @param attempt Attempt being aborted. A stale abort leaves the current attempt alone.
         */
        static void Abort(CharacterWorld &world, CharacterSceneTickAttempt attempt) noexcept;
    };
}  // namespace Horo::Character::Detail
