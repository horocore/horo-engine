#pragma once
/** @file CharacterScenePlacement.h
 * @brief Non-installed production Scene-to-Character initial-placement boundary; never an SDK export.
 */
#include "Horo/Physics/CharacterWorld.h"
#include "Horo/Runtime/Scene/SceneIdentity.h"

namespace Horo::Character::Detail {
    /** @brief Grants the paired Scene host the ordinary single-spawn recovery with an authored world heading.
     * @details This boundary does not expose mutable Character implementation state or bypass placement guards.
     */
    class CharacterScenePlacement final {
    public:
        /** @brief Admits initial real collider queries for the exact paired private Scene without advancing simulation.
         * @param physics Activated owner with resident staged bodies, no tick and no prior query Scene binding.
         * @param character Prepared paired owner whose descriptor proves the exact Physics and Scene identity.
         * @param scene Exact candidate runtime identity, not a durable Scene definition ID.
         * @return Success or typed stale/lifecycle/affinity failure; no body pose, tick or publication changes.
         */
        [[nodiscard]] static Result<void> BindInitialQueries(Physics::PhysicsWorld &physics, const CharacterWorld &character,
                                                             Runtime::SceneRuntimeId scene);

        /** @brief Performs the existing bounded recovery and atomic placement once, preserving the supplied unit heading.
         * @param world Prepared owner world. @param handle Exact resident unspawned controller.
         * @param query Real paired Physics adapter. @param heading Validated initial world rotation.
         * @return Coherent placement or typed error with the same ownership and once-only semantics as SpawnController.
         */
        [[nodiscard]] static Result<CharacterPlacementResult> Spawn(CharacterWorld &world, const CharacterControllerHandle &handle,
                                                                    const CharacterPhysicsQueryContext &query, Math::Quaternion heading) {
            return world.SpawnControllerWithHeading(handle, query, heading);
        }
    };
}  // namespace Horo::Character::Detail
