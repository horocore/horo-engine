#pragma once

/** @file GameplayVfxHostAccess.h
 * @brief Noninstalled trusted composition seam; never an ordinary module/script capability or a Scene authority factory.
 */

#include "Horo/Gameplay/GameplayVfxContext.h"
#include "Horo/Vfx/VfxWorld.h"

namespace Horo::Gameplay::Detail {
    struct GameplayVfxSceneState;
    struct GameplayVfxModuleSlot;

    /** @brief Opaque retained source owner, bound only to the genuine World attached during Scene preparation. */
    struct GameplayVfxSceneLease final {
        std::shared_ptr<GameplayVfxSceneState> state;
    };

    using GameplayVfxTickResult = Horo::Gameplay::GameplayVfxTickResult;

    /** @brief Explicit owner-lane controls for the actual Scene/Gameplay host.
     * @details Prepare is cold, copies grants and allocates every semantic command/result slot. Publish receives a genuine
     * Scene publication gate; it creates no Scene/session identity. Revoke must run before the borrowed attached World retires.
     * Submit/commit/update use one canonical World queue; the prepared source queue contains semantic commands and captured
     * immutable policy leases, never a second simulation or resolved effect authority. No callback, file I/O or backend selection
     * occurs here. Rejected callback entries and stale owner cleanup cannot revoke a newer active scope.
     */
    struct GameplayVfxHostAccess final {
        [[nodiscard]] static Result<GameplayVfxSceneLease> Prepare(Vfx::VfxWorld &world,
                                                                   std::shared_ptr<const GameplayVfxBindingTable> table,
                                                                   std::span<const GameplayVfxGrant> grants, GameplayVfxSourceBudget budget,
                                                                   float maximumDeltaSeconds);
        static void Publish(const GameplayVfxSceneLease &lease) noexcept;
        [[nodiscard]] static Vfx::VfxIdentityScope Scene(const GameplayVfxSceneLease &lease) noexcept;
        [[nodiscard]] static Vfx::VfxSpawnStatus PreparedStatus(const GameplayVfxSceneLease &lease) noexcept;
        static void Revoke(const GameplayVfxSceneLease &lease) noexcept;
        [[nodiscard]] static std::shared_ptr<const GameplayVfxContext> Module(const GameplayVfxSceneLease &lease,
                                                                              std::string_view moduleId) noexcept;
        [[nodiscard]] static std::shared_ptr<const GameplayVfxCommittedProducer> CommittedProducer(const GameplayVfxSceneLease &lease,
                                                                                                   std::string_view moduleId) noexcept;
        [[nodiscard]] static Result<std::shared_ptr<const GameplayVfxContext>> ForEntity(
            const std::shared_ptr<const GameplayVfxContext> &module, GameplayEntityRef entity);
        [[nodiscard]] static Result<std::shared_ptr<const GameplayVfxContext>> ForkRuntime(
            const std::shared_ptr<const GameplayVfxContext> &module);
        /** @brief Revokes future entity calls after actual successful Scene structural publication.
         * Admitted commands retain their exact cancellation lifetime; explicit RevokeClient and runner/Scene revocation still cancel them.
         * @param client Genuine entity scope owned by the structural attachment being retired.
         */
        static void RetireEntityClient(const std::shared_ptr<const GameplayVfxContext> &client) noexcept;
        static void RevokeClient(const std::shared_ptr<const GameplayVfxContext> &client) noexcept;
        [[nodiscard]] static bool EnterCallback(const std::shared_ptr<const GameplayVfxContext> &client) noexcept;
        static void LeaveCallback(const std::shared_ptr<const GameplayVfxContext> &client) noexcept;
        /** @brief Cancels only this attachment's uncommitted current-tick commands after a contained callback failure.
         * @details The actual callback window must still be active. Other attachments, sealed ticks and committed cinematic
         * commands remain intact; the client stays live for an explicitly admitted script recovery.
         */
        static void AbortCallback(const std::shared_ptr<const GameplayVfxContext> &client) noexcept;
        [[nodiscard]] static Vfx::VfxSpawnStatus BeginTick(const GameplayVfxSceneLease &lease, std::uint64_t tick,
                                                           float deltaSeconds) noexcept;
        [[nodiscard]] static GameplayVfxTickResult CommitTick(const GameplayVfxSceneLease &lease, std::uint64_t tick) noexcept;
        static void AbortTick(const GameplayVfxSceneLease &lease, std::uint64_t tick) noexcept;
        /** @brief Arms a bounded source cutoff before the actual scheduler commitment; performs no World update. */
        [[nodiscard]] static Vfx::VfxSpawnStatus PrepareSealTick(const GameplayVfxSceneLease &lease, std::uint64_t tick) noexcept;
        /** @brief Seals the already-armed cutoff only after genuine fixed commitment; never allocates or invokes callbacks. */
        static void SealTick(const GameplayVfxSceneLease &lease, std::uint64_t tick) noexcept;
        /** @brief Flushes the oldest sealed cutoff after its actual Scene receipt resolves; rejection cancels only uncommitted behavior. */
        [[nodiscard]] static GameplayVfxTickResult CommitSealedTick(const GameplayVfxSceneLease &lease, std::uint64_t tick,
                                                                    bool sourceCommitted) noexcept;
        [[nodiscard]] static Result<std::shared_ptr<const GameplayVfxBindingTable>> PrepareReload(const GameplayVfxSceneLease &lease,
                                                                                                  GameplayVfxBindingDocument document,
                                                                                                  Assets::AssetRegistrySnapshot registry,
                                                                                                  const Assets::IAssetProvider &provider,
                                                                                                  const CancellationToken &cancellation);
        [[nodiscard]] static Vfx::VfxSpawnStatus PublishReload(const GameplayVfxSceneLease &lease,
                                                               std::shared_ptr<const GameplayVfxBindingTable> replacement) noexcept;

    private:
        [[nodiscard]] static GameplayVfxModuleSlot MakeModule(const std::shared_ptr<GameplayVfxSceneState> &scene,
                                                              const GameplayVfxGrant &grant);
    };
}  // namespace Horo::Gameplay::Detail
