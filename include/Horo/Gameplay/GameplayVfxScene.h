#pragma once

/** @file GameplayVfxScene.h
 * @brief Genuine Scene activation/publication owner for the shared semantic-to-effect pipeline.
 */

#include "Horo/Gameplay/GameplayVfxContext.h"
#include "Horo/Runtime/Scene/RuntimeScene.h"

namespace Horo::Gameplay {
    /** @brief Explicit application composition; Create copies all borrowed kernel spans before returning.
     * @details Recipe/material admission remains host-owned. Provider and registry are actual existing asset authorities;
     * the feature chooses no backend, filesystem root, module or registry implicitly. Host reservations must include
     * old-plus-new Scene overlap. Register the returned participant explicitly before Scene service startup.
     */
    struct GameplayVfxSceneConfiguration final {
        std::vector<Vfx::VfxSpawnAssetPreparation> recipes;
        Vfx::VfxSpawnBudget spawn;
        Vfx::VfxExtractionBudget extraction;
        GameplayVfxSourceBudget source;
        GameplayVfxBindingDocument bindings;
        Assets::AssetRegistrySnapshot registry;
        std::shared_ptr<const Assets::IAssetProvider> provider;
        std::vector<GameplayVfxGrant> grants;
        Math::WorldCoordinate64 origin;
        std::uint64_t originRevision{1};
    };

    /** @brief Scene-owned attachment and source commit adapter; never a second World or clock.
     * @details The application drives BeginTick from its actual fixed owner opportunity, commits only after real
     * Scene commands succeed, and aborts failed source ticks. This participant does not auto-register lifecycle work.
     * It must outlive Scene service registration. Candidate rollback/old retirement revoke only their exact lease.
     */
    class GameplayVfxScene final : public Runtime::SceneActivationParticipant, public std::enable_shared_from_this<GameplayVfxScene> {
    public:
        /** @brief Captures immutable cold policy/kernel inputs without publishing a Scene or issuing a client capability.
         * @param configuration Explicit host authorities, data and finite reservations.
         * @return Prepared application participant or typed invalid/allocation failure.
         */
        [[nodiscard]] static Result<std::shared_ptr<GameplayVfxScene>> Create(GameplayVfxSceneConfiguration configuration);
        /** @brief Copies this facade's original owned cold policy into a separate unpublished Scene participant.
         * @return Independent prepared facade or typed allocation/lane failure; no active World or client authority is copied.
         * @details Configured multi-world hosts use one sibling per actual WorldRecord, including travel overlap.
         */
        [[nodiscard]] Result<std::shared_ptr<GameplayVfxScene>> CreateSibling() const;
        ~GameplayVfxScene() override;
        GameplayVfxScene(const GameplayVfxScene &) = delete;
        GameplayVfxScene &operator=(const GameplayVfxScene &) = delete;
        /** @brief Creates a unique service registration retaining this SAME shared facade and state.
         * @return Participant for RuntimeSceneService::AddActivationParticipant; no activation or registration occurs inline.
         */
        [[nodiscard]] std::unique_ptr<Runtime::SceneActivationParticipant> MakeActivationParticipant();
        /** @brief Copies actual published scope metadata. @return Actual RuntimeSceneId value, or invalid while absent. */
        [[nodiscard]] Vfx::VfxIdentityScope Scene() const noexcept;

        /** @brief Prepares and attaches the sole canonical World to this genuine unpublished Scene.
         * @param scene Actual Scene service candidate with finalized initial placement.
         * @return Success or typed preparation failure; publishes no contexts and leaves the active Scene untouched.
         */
        [[nodiscard]] Result<void> PrepareOwnedScene(Runtime::RuntimeScene &scene) override;
        /** @brief Binds scoped source state to the already-attached candidate and returns its real publication/retirement lease.
         * @param definition Exact immutable definition. @param scene Borrowed matching candidate view.
         * @return Genuine prepared candidate or typed zero-active-mutation failure.
         */
        [[nodiscard]] Result<std::unique_ptr<Runtime::SceneActivationCandidate>> Prepare(const Runtime::RuntimeSceneDefinition &definition,
                                                                                         const Runtime::RuntimeSceneView &scene) override;
        /** @brief Prepares exact active Scene retirement without touching a replacement.
         * @param scene Actual active view. @return Detached retirement publication candidate or typed mismatch.
         */
        [[nodiscard]] Result<std::unique_ptr<Runtime::SceneActivationCandidate>> PrepareRetirement(
            const Runtime::RuntimeSceneView &scene) override;

        /** @brief Borrows no mutable World; returns only the explicitly granted current module client.
         * @param moduleId Actual selected module principal. @return Retainable client or empty when unavailable.
         */
        [[nodiscard]] std::shared_ptr<const GameplayVfxContext> Module(std::string_view moduleId) const noexcept;
        /** @brief Issues no identity; returns the real active host producer for a commit-gated source adapter.
         * @param moduleId Explicit granted principal. @return Existing sealed producer or empty.
         */
        [[nodiscard]] std::shared_ptr<const GameplayVfxCommittedProducer> CommittedProducer(std::string_view moduleId) const noexcept;
        /** @brief Opens one actual owner tick before behavior/cinematic source callbacks.
         * @param tick Genuine fixed tick; same-tick retry after abort is permitted, backwards time is not.
         * @param deltaSeconds Genuine selected-clock delta within prepared bounds.
         * @return Typed admission status; Accepted opens no callbacks by itself.
         */
        [[nodiscard]] Vfx::VfxSpawnStatus BeginTick(std::uint64_t tick, float deltaSeconds) noexcept;
        /** @brief Resolves staged semantics and advances the canonical World only after actual Scene source commit.
         * @param tick Same genuine tick opened by BeginTick.
         * @return Copied per-cutoff evidence, retaining typed failures in LastTick and client receipts.
         */
        [[nodiscard]] GameplayVfxTickResult CommitTick(std::uint64_t tick) noexcept;
        /** @brief Cancels the failed behavior source transaction without advancing a World or rewinding occurrence IDs.
         * @param tick Exact open tick; independently committed source commands retain their captured identity for retry.
         */
        void AbortTick(std::uint64_t tick) noexcept;
        /** @brief Arms a bounded cutoff before genuine fixed commitment, retaining only its actual Scene transaction receipt.
         * @param tick Exact open fixed tick. @param receipt Actual behavior Scene transaction receipt; empty means no Scene commands.
         * @return Accepted or typed zero-World-mutation rejection. Successful preparation is not source commitment.
         */
        [[nodiscard]] Vfx::VfxSpawnStatus PrepareSealTick(std::uint64_t tick,
                                                          std::shared_ptr<const Runtime::SceneStructuralReceipt> receipt = {}) noexcept;
        /** @brief Seals the prepared semantic cutoff after actual scheduler commitment without advancing the World.
         * @param tick Exact armed and genuinely committed fixed tick.
         */
        void SealTick(std::uint64_t tick) noexcept;
        /** @brief Flushes completed sealed source cutoffs in original tick order after actual Scene publication.
         * @return Last actual cutoff report; pending receipts stop the prefix, rejected receipts cancel only uncommitted behavior.
         * @details Existing live effects still advance on committed fixed ticks whose Scene commands were rejected.
         * Independently committed cinematic commands retain their original identity. No Scene notification is consumed.
         */
        [[nodiscard]] GameplayVfxTickResult FlushSealedTicks() noexcept;
        /** @brief Returns bounded copied last-cutoff evidence. @return Actual last report; no mutable storage escapes. */
        [[nodiscard]] GameplayVfxTickResult LastTick() const noexcept;
        /** @brief Cold immutable table replacement against the current genuine World and actual asset authorities.
         * @param document New strictly increasing generation. @param registry Captured actual asset snapshot.
         * @param provider Actual available payload owner, borrowed only for this call. @param cancellation Cooperative cold cancellation.
         * @return Success or typed failure preserving the current generation and all accepted requests.
         */
        [[nodiscard]] Result<void> Reload(GameplayVfxBindingDocument document, Assets::AssetRegistrySnapshot registry,
                                          const Assets::IAssetProvider &provider, const CancellationToken &cancellation = {});

    private:
        struct State;
        explicit GameplayVfxScene(std::shared_ptr<State> state);
        std::shared_ptr<State> state_;
    };
}  // namespace Horo::Gameplay
