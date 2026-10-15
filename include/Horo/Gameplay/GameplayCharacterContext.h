#pragma once
/** @file GameplayCharacterContext.h
 * @brief Explicit module/scene permission and retirement scope over the canonical Character capability.
 */
#include "Horo/Gameplay/GameplayPhysicsContext.h"
#include "Horo/Physics/CharacterCapability.h"

namespace Horo::Gameplay {
    /** @brief Closed host decision; neither denial grants Character access. */
    enum class GameplayCharacterDenial {
        PermissionDenied,
        Unavailable
    };

    /** @brief Immutable host-selected principal and exact paired-world routing. */
    struct GameplayCharacterBinding final {
        std::string moduleId;
        std::uint64_t scene{};
        std::uint64_t sceneGeneration{};
        Character::CharacterWorldId world;
        Physics::PhysicsWorldId physicsWorld;
        bool moduleEnabled{};
        bool permissionGranted{};
    };

    /** @brief Owner-thread host holder of an independently revocable Character grant.
     * @details Create and private derivation allocate only at lifecycle safe points. Acquire copies a canonical
     * grant without allocating on success. No Scene, controller, native pointer or module callback is retained.
     * The host anchor may serve multiple sequential module generations; retiring one child never retires that anchor.
     * Revoke is cross-thread safe; all other operations and destruction are owner-thread operations.
     */
    class GameplayCharacterContext final {
        struct ConstructionKey {
        private:
            friend class GameplayCharacterContext;
            ConstructionKey() = default;
        };

    public:
        /** @brief Creates explicit unavailable/denied diagnostics with no Character authority.
         * @param binding Copied trusted routing evidence. @param denial Host decision. @return Inert owned holder. */
        [[nodiscard]] static std::shared_ptr<GameplayCharacterContext> Withheld(const GameplayCharacterBinding &binding,
                                                                                GameplayCharacterDenial denial);
        /** @brief Factory-only constructor; a private non-aggregate key prevents bypassing admission.
         * @param binding Copied principal. @param capability Host-issued grant. @param denial Optional diagnostic reason. */
        GameplayCharacterContext(ConstructionKey, GameplayCharacterBinding binding, Character::CharacterCapability capability,
                                 std::optional<GameplayCharacterDenial> denial = std::nullopt);
        /** @brief Admits exact active Character/Physics dependencies and explicit module permission.
         * @param binding Host-owned routing/policy. @param world Selected active world, borrowed only during this call.
         * @param physics Explicit matching Physics context; no service discovery. @param cancellation Host anchor retirement chain.
         * @return Owned anchor or typed permission, dependency, identity, affinity or capacity failure. */
        [[nodiscard]] static Result<std::shared_ptr<GameplayCharacterContext>> Create(const GameplayCharacterBinding &binding,
                                                                                      Character::CharacterWorld *world,
                                                                                      const GameplayPhysicsContext *physics,
                                                                                      CancellationToken cancellation = {});
        ~GameplayCharacterContext();
        GameplayCharacterContext(const GameplayCharacterContext &) = delete;
        GameplayCharacterContext &operator=(const GameplayCharacterContext &) = delete;
        /** @brief Acquires canonical Character access for one exact principal and scene incarnation.
         * @param moduleId Trusted module principal. @param scene Exact runtime Scene ID. @param sceneGeneration Exact activation epoch.
         * @return Copyable canonical grant or typed permission, unavailable, foreign-scene or retired-world failure. */
        [[nodiscard]] Result<Character::CharacterCapability> Acquire(std::string_view moduleId, std::uint64_t scene,
                                                                     std::uint64_t sceneGeneration) const;
        /** @brief Permanently revokes this holder and all derived grants, before consumer teardown callbacks. */
        void Revoke() const noexcept;
        /** @brief Reads copied immutable identity/policy evidence. @return Binding retained by this holder. */
        [[nodiscard]] const GameplayCharacterBinding &Binding() const noexcept;

    private:
        friend class GameModuleHost;
        friend class BehaviorRuntime;
        friend class LuaBehaviorInstance;
        /** @brief Derives a fresh scope without retaining any world or consumer. @return Child or original admission failure. */
        [[nodiscard]] Result<std::shared_ptr<GameplayCharacterContext>> Derive() const;
        /** @brief Validates explicit dependency evidence before creating a native-module or runtime child.
         * @param physics Exact host-supplied dependency, or missing. @return Original typed dependency/identity failure. */
        [[nodiscard]] Result<void> ValidateDependency(const GameplayPhysicsContext *physics) const;
        GameplayCharacterBinding binding_;
        Character::CharacterCapability capability_;
        std::optional<GameplayCharacterDenial> denial_;
    };
}  // namespace Horo::Gameplay
