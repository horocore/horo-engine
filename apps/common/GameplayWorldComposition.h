#pragma once

/** @file GameplayWorldComposition.h
 * @brief Non-installed application composition shared by headless and graphical world owners.
 */

#include "Horo/Gameplay/BehaviorRuntime.h"
#include "Horo/Gameplay/GameModuleHost.h"
#include "Horo/Gameplay/GameplayPhysicsContext.h"
#include "Horo/Gameplay/LuaBehavior.h"

namespace Horo::Application::Internal {
    /** @brief Trusted host policy resolved before invoking project code; no ambient grant. */
    enum class GameplayPhysicsPermission {
        Denied,
        Granted
    };

    /** @brief One explicitly selected native module or script and its host-resolved permission. */
    struct GameplayWorldSelection final {
        std::string moduleId;
        std::filesystem::path nativeArtifact;
        std::uint64_t descriptorRevision{};
        std::filesystem::path scriptSource;
        std::filesystem::path scriptSidecar;
        GameplayPhysicsPermission physicsPermission{GameplayPhysicsPermission::Denied};
        bool enabled{true}; /**< Trusted host enablement; disabled modules never execute a callback. */
    };

    /** @brief Owner-thread gameplay lifetime borrowing an explicit world and Scene.
     * Revoke runs before behavior/module stop and before the owning host retires world/Scene storage.
     * The Physics holder itself retains neither borrowed object. Scripts and native callbacks use
     * the same Physics-owned capability, through their normal execution contexts.
     */
    class GameplayWorldComposition final {
    public:
        /** @brief Activate the selected module/script for an exact Scene activation epoch.
         * @param scene Host-owned Scene outliving this composition.
         * @param world Explicitly selected world; null means unavailable, never fallback.
         * @param sceneGeneration Non-zero host activation epoch used by Physics ticks.
         * @param selection Trusted resolved artifact identity and permission policy.
         * @return Active composition or typed activation failure with complete rollback.
         */
        [[nodiscard]] static Result<std::unique_ptr<GameplayWorldComposition>> Create(Runtime::RuntimeScene &scene,
                                                                                      Physics::PhysicsWorld *world,
                                                                                      std::uint64_t sceneGeneration,
                                                                                      const GameplayWorldSelection &selection);
        ~GameplayWorldComposition();
        GameplayWorldComposition() = default;
        GameplayWorldComposition(const GameplayWorldComposition &) = delete;
        GameplayWorldComposition &operator=(const GameplayWorldComposition &) = delete;
        /** @brief Dispatch the canonical fixed Gameplay phase. @return First callback/transaction failure. */
        [[nodiscard]] Result<void> FixedUpdate(Gameplay::FixedDeltaTime delta);
        /** @brief Revoke the whole world scope before any shutdown callback or storage retirement. */
        void Shutdown() noexcept;
        /** @brief Retire just the module/script; the explicit Physics world may stay active. */
        void UnloadModule() noexcept;
        /** @brief Borrow the immutable Physics execution context for integrated host diagnostics. */
        [[nodiscard]] std::shared_ptr<const Gameplay::GameplayPhysicsContext> PhysicsContext() const noexcept;
        /** @brief Borrows this active runner for an aggregate Scene group transaction.
         * @return Gameplay participant or null when the selected module is not active.
         * @pre This composition outlives the returned adapter and its prepared candidates.
         */
        [[nodiscard]] std::unique_ptr<Runtime::SceneStructuralParticipant> MakeStructuralParticipant();

    private:
        /** @brief Validates artifacts and activates callbacks only after Physics admission is resolved. */
        [[nodiscard]] Result<void> Activate(Runtime::RuntimeScene &scene, const GameplayWorldSelection &selection);
        std::shared_ptr<Gameplay::GameplayPhysicsContext> physics_;
        std::unique_ptr<Gameplay::LoadedGameModule> native_;
        std::unique_ptr<Gameplay::LuaBehaviorProgram> script_;
        Gameplay::BehaviorRegistry registry_;
        std::unique_ptr<Gameplay::BehaviorRuntime> behaviors_;
    };

    /** @brief Composes the Gameplay structural owner before Scene startup using an explicit application-owned active slot.
     * @param active Host slot populated only after successful module/scene activation, retired at drained lifecycle safe points.
     * @return Owned adapter; the slot and its current composition outlive each synchronous group transaction.
     * @details Empty/retired slots reject required Gameplay work rather than discover or load another module.
     */
    [[nodiscard]] std::unique_ptr<Runtime::SceneStructuralParticipant> MakeGameplayStructuralParticipant(
        const std::unique_ptr<GameplayWorldComposition> &active);
}  // namespace Horo::Application::Internal
