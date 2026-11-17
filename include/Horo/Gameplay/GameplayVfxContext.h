#pragma once

/** @file GameplayVfxContext.h
 * @brief Revocable exact-Scene semantic presentation capability shared by native and Lua behavior clients.
 */

#include "Horo/Gameplay/GameplayVfxBindingTable.h"

namespace Horo::Gameplay {
    /** @brief Explicit cold source-command reservation; derived behavior scopes use their owning behavior admission limits. */
    struct GameplayVfxSourceBudget final {
        std::size_t maximumPending{64};
        std::size_t maximumModules{16};
        std::size_t maximumBytes{1024U * 1024U};
    };

    /** @brief Explicit cold application grant; asset knowledge never extends the permitted semantic set. */
    struct GameplayVfxGrant final {
        std::string moduleId;
        Vfx::VfxIdentityScope owner;
        std::uint64_t ownerGeneration{};
        std::uint64_t producer{};
        std::vector<GameplayEventTypeId> events;
        std::vector<GameplayVfxEffectTag> tags;
    };

    /** @brief Copied application-issued receipt for one semantic source command, independent of eventual layered effect identities. */
    struct GameplayVfxReceipt final {
        Vfx::VfxIdentityScope scene;
        Vfx::VfxIdentityScope owner;
        std::uint64_t ownerGeneration{};
        Vfx::VfxSpawnOccurrence occurrence;
    };

    /** @brief Accepted means staged by the source command owner; it never claims an effect or source tick has committed. */
    struct GameplayVfxSubmission final {
        Vfx::VfxSpawnStatus status{Vfx::VfxSpawnStatus::PermissionDenied};
        GameplayVfxReceipt receipt;
    };

    /** @brief Finite copied semantic result, including each actual canonical World layer outcome. */
    struct GameplayVfxResult final {
        Vfx::VfxSpawnStatus status{Vfx::VfxSpawnStatus::Empty};
        bool pending{};
        std::array<Vfx::VfxSpawnOutcome, Vfx::VfxSpawnHardLimits::Layers> layers{};
        std::uint32_t layerCount{};
    };

    /** @brief Copied actual source cutoff evidence; successful source publication is distinct from each effect outcome. */
    struct GameplayVfxTickResult final {
        Vfx::VfxSpawnStatus status{Vfx::VfxSpawnStatus::Empty};
        Vfx::VfxSpawnUpdate world;
        std::uint32_t submitted{};
        std::uint32_t rejected{};
    };

    namespace Detail {
        struct GameplayVfxClientState;
        struct GameplayVfxHostAccess;
        struct GameplayVfxProducerState;
    }  // namespace Detail

    /** @brief Retainable data-only client; all calls are owner-lane fenced and fail without touching a retired World.
     * @details Context creation, callback windows, source commit/abort, reload and retirement are trusted host controls.
     * Ordinary clients cannot construct scopes or force a source commit. Submitted semantic commands retain their captured
     * immutable table generation until the real source owner publishes or aborts them. This is not another gameplay event bus.
     */
    class GameplayVfxContext final {
    public:
        /** @brief Releases this retained data-only scope without touching an already retired World. */
        ~GameplayVfxContext();
        GameplayVfxContext(const GameplayVfxContext &) = delete;
        GameplayVfxContext &operator=(const GameplayVfxContext &) = delete;

        /** @brief Resolves one exact permitted semantic event without allocating.
         * @param event Borrowed registered identity text. @param schemaVersion Source semantic schema revision.
         * @return Exact key or typed permission/unknown/schema/stale/lifecycle outcome.
         */
        [[nodiscard]] GameplayVfxLookup ResolveEvent(std::string_view event, std::uint32_t schemaVersion = 1) const noexcept;
        /** @brief Resolves one explicitly permitted cooked effect tag without inferring gameplay meaning.
         * @param tag Stable registry-backed tag. @param schemaVersion Source semantic schema revision.
         * @return Exact key or typed permission/unknown/schema/stale/lifecycle outcome.
         */
        [[nodiscard]] GameplayVfxLookup ResolveTag(GameplayVfxEffectTag tag, std::uint32_t schemaVersion = 1) const noexcept;
        /** @brief Stages a bounded semantic source command during this exact behavior's admitted callback window.
         * @param key Fresh generation-fenced lookup; it grants no permission by itself.
         * @param payload Copied bounded placement/scalar arguments; no asset or routing fields are caller selectable.
         * @return Source receipt or typed zero-mutation rejection. Actual World queue admission occurs only after source commit.
         */
        [[nodiscard]] GameplayVfxSubmission Emit(GameplayVfxEventKey key, const GameplayVfxEventPayload &payload) const noexcept;
        /** @brief Copies the source owner's latest bounded evidence for one own receipt.
         * @param receipt Application-issued exact own occurrence; foreign owners are denied.
         * @return Pending, terminal/expired or revoked evidence; no mutable storage or native pointer escapes.
         */
        [[nodiscard]] GameplayVfxResult Poll(const GameplayVfxReceipt &receipt) const noexcept;
        /** @brief Reports whether the exact scope is live on this owner lane; it does not open a callback window.
         * @return False for retired owners or foreign threads.
         */
        [[nodiscard]] bool IsUsable() const noexcept;
        /** @brief Copies the exact admitted Scene scope. @return Scene identity, never a lifetime or permission grant. */
        [[nodiscard]] Vfx::VfxIdentityScope Scene() const noexcept;
        /** @brief Borrows immutable admitted principal metadata. @return Exact host-selected module identity. */
        [[nodiscard]] std::string_view ModuleId() const noexcept;

    private:
        friend struct Detail::GameplayVfxHostAccess;
        explicit GameplayVfxContext(std::shared_ptr<Detail::GameplayVfxClientState> state);
        std::shared_ptr<Detail::GameplayVfxClientState> state_;
    };

    /** @brief Host-only already-committed event capability; ordinary BehaviorContext never exposes this producer.
     * @details Created only by trusted actual Scene composition. It is suitable for a commit-gated cinematic registration,
     * not a user assertion that an arbitrary event committed. It shares the same mapping/World queue as behavior commands.
     */
    class GameplayVfxCommittedProducer final {
    public:
        /** @brief Releases the application producer after its owning registration lease retires. */
        ~GameplayVfxCommittedProducer();
        GameplayVfxCommittedProducer(const GameplayVfxCommittedProducer &) = delete;
        GameplayVfxCommittedProducer &operator=(const GameplayVfxCommittedProducer &) = delete;
        /** @brief Looks up an exact permitted semantic event for a genuine committed-source adapter.
         * @param event Registered semantic identity. @param schemaVersion Source schema revision.
         * @return Typed scoped lookup; no table or World reference escapes.
         */
        [[nodiscard]] GameplayVfxLookup ResolveEvent(std::string_view event, std::uint32_t schemaVersion = 1) const noexcept;
        /** @brief Stages one genuine committed occurrence for the same source-owner cutoff and canonical World admission.
         * @param key Exact authorized mapping generation. @param payload Copied semantic arguments.
         * @param committedTick Actual committed source tick observed by the owning adapter.
         * @return Typed source acceptance; this does not claim spawning or invoke an inline completion callback.
         */
        [[nodiscard]] GameplayVfxSubmission Emit(GameplayVfxEventKey key, const GameplayVfxEventPayload &payload,
                                                 std::uint64_t committedTick) const noexcept;
        /** @brief Reports exact owner-lane lifetime admission. @return False after retirement or on another lane. */
        [[nodiscard]] bool IsUsable() const noexcept;

    private:
        friend struct Detail::GameplayVfxHostAccess;
        explicit GameplayVfxCommittedProducer(std::shared_ptr<Detail::GameplayVfxProducerState> state);
        std::shared_ptr<Detail::GameplayVfxProducerState> state_;
    };
}  // namespace Horo::Gameplay
