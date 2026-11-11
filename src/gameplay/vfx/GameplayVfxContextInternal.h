#pragma once

#include "GameplayVfxBindingInternal.h"
#include "GameplayVfxHostAccess.h"

#include <thread>

namespace Horo::Gameplay::Detail {
    struct GameplayVfxScopeLifetime final {
        bool revoked{};
        std::shared_ptr<const GameplayVfxScopeLifetime> parent;
    };

    struct GameplayVfxModuleState final {
        GameplayVfxGrant grant;
        Vfx::VfxIdentityScope scene;
        std::thread::id owner;
        std::uint64_t nextSequence{1};
        bool active{true};
    };

    struct GameplayVfxClientState final {
        std::weak_ptr<GameplayVfxSceneState> scene;
        std::shared_ptr<GameplayVfxModuleState> module;
        std::shared_ptr<GameplayVfxScopeLifetime> lifetime;
        GameplayEntityRef entity;
        bool callback{};
        bool retired{}; /**< Successful entity retirement denies new calls without cancelling already admitted source work. */
    };

    struct GameplayVfxProducerState final {
        std::weak_ptr<GameplayVfxSceneState> scene;
        std::shared_ptr<GameplayVfxModuleState> module;
    };

    struct GameplayVfxModuleSlot final {
        std::shared_ptr<GameplayVfxModuleState> state;
        std::shared_ptr<const GameplayVfxContext> client;
        std::shared_ptr<const GameplayVfxCommittedProducer> producer;
    };

    /** @brief Source-owned semantic command, not a second resolved VFX queue; policy identity is retained until source completion. */
    struct GameplayVfxPending final {
        std::shared_ptr<const GameplayVfxBindingTable> table;
        GameplayVfxEventKey key;
        GameplayVfxEventPayload payload;
        GameplayVfxReceipt receipt;
        std::uint64_t tick{};
        bool committed{};
        std::shared_ptr<const GameplayVfxScopeLifetime> lifetime;
    };

    struct GameplayVfxSourceRecord final {
        GameplayVfxReceipt receipt;
        GameplayVfxResult result;
    };

    /** @brief Genuine fixed commitment metadata; semantic commands remain in the single source-owned pending array. */
    struct GameplayVfxSealedTick final {
        std::uint64_t tick{};
        float delta{};
    };

    struct GameplayVfxSceneState final {
        Vfx::VfxWorld *world{}; /**< Genuine attached candidate; cleared before its Scene retires. */
        Vfx::VfxIdentityScope scene;
        Assets::AssetId witness;
        std::thread::id owner;
        std::shared_ptr<const GameplayVfxBindingTable> table;
        std::vector<GameplayVfxModuleSlot> modules;
        std::vector<GameplayVfxPending> pending;
        std::vector<GameplayVfxSourceRecord> results;
        std::vector<GameplayVfxSealedTick> sealed;
        std::size_t sealHead{};
        std::size_t sealCount{};
        std::uint64_t lastSealedTick{};
        bool sealPrepared{};
        std::size_t pendingCount{};
        std::size_t resultCount{};
        std::uint64_t tick{};
        std::uint64_t lastCommittedTick{};
        float delta{};
        float maximumDelta{};
        bool active{};
        bool sourceOpen{};
        bool busy{};
    };
}  // namespace Horo::Gameplay::Detail

namespace Horo::Gameplay::VfxContextDetail {
    /** @brief Parent chains are cold-issued module/runtime/entity scopes, never an unbounded client-created hierarchy. */
    [[nodiscard]] inline bool ScopeRevoked(const std::shared_ptr<const Detail::GameplayVfxScopeLifetime> &scope) noexcept {
        if (!scope)
            return true;
        const auto *current = scope.get();
        for (std::size_t depth = 0; current && depth < 3; ++depth) {
            if (current->revoked)
                return true;
            current = current->parent.get();
        }
        return current != nullptr;
    }

    [[nodiscard]] Vfx::VfxSpawnStatus Status(const Detail::GameplayVfxSceneState &state) noexcept;
    [[nodiscard]] Vfx::VfxSpawnStatus ClientStatus(const Detail::GameplayVfxClientState &client,
                                                   std::shared_ptr<Detail::GameplayVfxSceneState> &scene) noexcept;
    [[nodiscard]] Vfx::VfxSpawnStatus ProducerStatus(const Detail::GameplayVfxProducerState &producer,
                                                     std::shared_ptr<Detail::GameplayVfxSceneState> &scene) noexcept;
    [[nodiscard]] bool Allowed(const Detail::GameplayVfxModuleState &moduleState,
                               const Detail::GameplayVfxCompiledBinding &binding) noexcept;
    [[nodiscard]] GameplayVfxSubmission Stage(Detail::GameplayVfxSceneState &scene, Detail::GameplayVfxModuleState &moduleState,
                                              GameplayVfxEventKey key, const GameplayVfxEventPayload &payload,
                                              std::shared_ptr<const Detail::GameplayVfxScopeLifetime> lifetime = {}) noexcept;
    [[nodiscard]] GameplayVfxSubmission EmitCommitted(Detail::GameplayVfxSceneState &scene, Detail::GameplayVfxModuleState &moduleState,
                                                      GameplayVfxEventKey key, const GameplayVfxEventPayload &payload,
                                                      std::uint64_t tick) noexcept;
    [[nodiscard]] GameplayVfxResult Poll(const Detail::GameplayVfxSceneState &scene, const Detail::GameplayVfxModuleState &moduleState,
                                         const GameplayVfxReceipt &receipt) noexcept;
    [[nodiscard]] Vfx::VfxSpawnRequest Basis(const Detail::GameplayVfxSceneState &scene, const GameplayVfxReceipt &receipt) noexcept;
    [[nodiscard]] bool SameReceipt(const GameplayVfxReceipt &left, const GameplayVfxReceipt &right) noexcept;
}  // namespace Horo::Gameplay::VfxContextDetail
