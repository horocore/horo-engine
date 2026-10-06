#pragma once

/** @file GameplayPhysicsContext.h
 * @brief Explicit host permission and lifetime binding of the existing Physics query/event capability.
 */

#include "Horo/Gameplay/GameplayRegistration.h"
#include "Horo/Physics/CharacterClearanceQuery.h"
#include "Horo/Physics/PhysicsQueryEventCapability.h"

#include <memory>
#include <optional>
#include <string>

namespace Horo::Physics {
    class PhysicsWorld;
}

namespace Horo::Gameplay {
    /** @brief Host-resolved reason for withholding Physics; neither state grants a client. */
    enum class GameplayPhysicsDenial {
        PermissionDenied,
        Unavailable
    };

    /** @brief Host-owned admission evidence for one exact module and play-scene/world generation. */
    struct GameplayPhysicsBinding final {
        std::string moduleId;
        std::uint64_t scene{};
        std::uint64_t sceneGeneration{};
        Physics::PhysicsWorldId world;
        bool moduleEnabled{};
        bool permissionGranted{};
    };

    /** @brief Host execution-context holder, not a Physics query/event interface.
     * Admission and acquisition run on the Physics owner thread. Revoke is an atomic cancellation
     * fence and retains no world pointer. Hosts revoke before scene/module unload, play stop or failure.
     * Copied Physics clients remain safe even if the world is destroyed before this holder.
     */
    class GameplayPhysicsContext final {
        struct ConstructionKey {};

    public:
        /** @brief Creates an inert diagnostic context for an omitted service or rejected permission.
         * @param binding Explicit principal/routing evidence; empty principal is allowed only for unavailable hosts.
         * @param denial Closed host-resolved denial reason.
         * @return Holder with no Physics capability and no world reference.
         */
        [[nodiscard]] static std::shared_ptr<GameplayPhysicsContext> Withheld(const GameplayPhysicsBinding &binding,
                                                                              GameplayPhysicsDenial denial);
        /** @brief Factory-only construction; the private key prevents bypassing admission. */
        GameplayPhysicsContext(ConstructionKey, const GameplayPhysicsBinding &binding, CancellationSource revocation,
                               Physics::PhysicsQueryEventCapability capability, std::optional<GameplayPhysicsDenial> denial = std::nullopt);
        /** @brief Admits only explicit permission and exact active-world routing.
         * @param binding Host-resolved module policy and exact world/scene evidence.
         * @param world Selected world, or null for an unavailable composition; never discovered globally.
         * @param cancellation Parent module-generation cancellation revoked before unload.
         * @return Context or stable permission, unavailable, identity or Physics admission error.
         */
        [[nodiscard]] static Result<std::shared_ptr<GameplayPhysicsContext>> Create(const GameplayPhysicsBinding &binding,
                                                                                    Physics::PhysicsWorld *world,
                                                                                    CancellationToken cancellation = {});
        ~GameplayPhysicsContext();
        GameplayPhysicsContext(const GameplayPhysicsContext &) = delete;
        GameplayPhysicsContext &operator=(const GameplayPhysicsContext &) = delete;
        /** @brief Acquires the existing PHY-004.9 capability, never a wrapper query API.
         * @param moduleId Exact module principal selected by host composition.
         * @param scene Exact runtime scene identity; zero does not mean a default world.
         * @param sceneGeneration Exact host activation epoch; reuse of a Scene ID is not admission.
         * @return Copy of the Physics capability or typed permission/revoked/foreign-context error.
         */
        [[nodiscard]] Result<Physics::PhysicsQueryEventCapability> Acquire(std::string_view moduleId, std::uint64_t scene,
                                                                           std::uint64_t sceneGeneration) const;
        /** @brief Acquires a production per-operation Character clearance adapter under the same permission fence.
         * @param moduleId Exact module principal selected by host composition.
         * @param scene Exact runtime scene identity.
         * @param expected Exact paired Character/Physics generations, tick and Physics publication revision.
         * @return Clearance adapter or original permission, identity or capability error.
         */
        [[nodiscard]] Result<Physics::CharacterClearanceQuery> AcquireCharacterClearance(
            std::string_view moduleId, std::uint64_t scene, Character::CharacterPhysicsQueryExpectations expected) const;
        /** @brief Closes this scope permanently before teardown; retained clients observe the same fence. */
        void Revoke() const noexcept;
        /** @brief Returns immutable exact routing evidence. @return Host-copied binding. */
        [[nodiscard]] const GameplayPhysicsBinding &Binding() const noexcept;

    private:
        GameplayPhysicsBinding binding_;
        CancellationSource revocation_;
        Physics::PhysicsQueryEventCapability capability_;
        std::optional<GameplayPhysicsDenial> denial_;
    };
}  // namespace Horo::Gameplay
